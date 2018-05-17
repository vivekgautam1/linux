// SPDX-License-Identifier: GPL-2.0
/*
 * Interconnect framework core driver
 *
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/interconnect.h>
#include <linux/interconnect-provider.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/uaccess.h>

static DEFINE_IDR(icc_idr);
static LIST_HEAD(icc_provider_list);
static DEFINE_MUTEX(icc_provider_list_mutex);
static DEFINE_MUTEX(icc_path_mutex);
static struct dentry *icc_debugfs_dir;

/**
 * struct icc_req - constraints that are attached to each node
 *
 * @req_node: entry in list of requests for the particular @node
 * @node: the interconnect node to which this constraint applies
 * @dev: reference to the device that sets the constraints
 * @avg_bw: an integer describing the average bandwidth in kbps
 * @peak_bw: an integer describing the peak bandwidth in kbps
 */
struct icc_req {
	struct hlist_node req_node;
	struct icc_node *node;
	struct device *dev;
	u32 avg_bw;
	u32 peak_bw;
};

/**
 * struct icc_path - interconnect path structure
 * @num_nodes: number of hops (nodes)
 * @reqs: array of the requests applicable to this path of nodes
 */
struct icc_path {
	size_t num_nodes;
	struct icc_req reqs[0];
};

#ifdef CONFIG_DEBUG_FS

static void icc_summary_show_one(struct seq_file *s, struct icc_node *n)
{
	struct icc_req *r;

	if (!n)
		return;

	seq_printf(s, "%-30s %12d %12d\n",
		   n->name, n->avg_bw, n->peak_bw);

	hlist_for_each_entry(r, &n->req_list, req_node) {
		seq_printf(s, "    %-26s %12d %12d\n",
			   dev_name(r->dev), r->avg_bw, r->peak_bw);
	}
}

static int icc_summary_show(struct seq_file *s, void *data)
{
	struct icc_provider *provider;

	seq_puts(s, " node                                   avg         peak\n");
	seq_puts(s, "--------------------------------------------------------\n");

	mutex_lock(&icc_provider_list_mutex);

	list_for_each_entry(provider, &icc_provider_list, provider_list) {
		struct icc_node *n;

		mutex_lock(&provider->lock);
		list_for_each_entry(n, &provider->nodes, node_list) {
			icc_summary_show_one(s, n);
		}
		mutex_unlock(&provider->lock);
	}

	mutex_unlock(&icc_provider_list_mutex);

	return 0;
}

static int icc_summary_open(struct inode *inode, struct file *file)
{
	return single_open(file, icc_summary_show, inode->i_private);
}

static const struct file_operations icc_summary_fops = {
	.open		= icc_summary_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init icc_debugfs_init(void)
{
	struct dentry *file;

	icc_debugfs_dir = debugfs_create_dir("interconnect", NULL);
	if (!icc_debugfs_dir) {
		pr_err("interconnect: error creating debugfs directory\n");
		return -ENODEV;
	}

	file = debugfs_create_file("interconnect_summary", 0444,
				   icc_debugfs_dir, NULL, &icc_summary_fops);
	if (!file)
		return -ENODEV;

	return 0;
}
late_initcall(icc_debugfs_init);
#endif

static struct icc_node *node_find(const int id)
{
	struct icc_node *node;

	node = idr_find(&icc_idr, id);

	return node;
}

static struct icc_path *path_allocate(struct icc_node *node, ssize_t num_nodes)
{
	struct icc_path *path;
	size_t i;

	path = kzalloc(sizeof(*path) + num_nodes * sizeof(*path->reqs),
		       GFP_KERNEL);
	if (!path)
		return ERR_PTR(-ENOMEM);

	path->num_nodes = num_nodes;

	for (i = 0; i < num_nodes; i++) {
		hlist_add_head(&path->reqs[i].req_node, &node->req_list);

		path->reqs[i].node = node;
		/* reference to previous node was saved during path traversal */
		node = node->reverse;
	}

	return path;
}

static struct icc_path *path_find(struct icc_node *src, struct icc_node *dst)
{
	struct icc_node *tmp_node, *node = NULL;
	struct icc_provider *provider;
	struct list_head traverse_list;
	struct list_head edge_list;
	struct list_head route_list;
	size_t i, number = 0;
	bool found = false;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&route_list);

	list_add_tail(&src->search_list, &traverse_list);

	do {
		list_for_each_entry_safe(node, tmp_node, &traverse_list, search_list) {
			if (node == dst) {
				found = true;
				list_add(&node->search_list, &route_list);
				break;
			}
			for (i = 0; i < node->num_links; i++) {
				struct icc_node *tmp = node->links[i];

				if (!tmp)
					return ERR_PTR(-ENOENT);

				if (tmp->is_traversed)
					continue;

				tmp->is_traversed = true;
				tmp->reverse = node;
				list_add(&tmp->search_list, &edge_list);
			}
		}
		if (found)
			break;

		list_splice_init(&traverse_list, &route_list);
		list_splice_init(&edge_list, &traverse_list);

		/* count the number of nodes */
		number++;

	} while (!list_empty(&traverse_list));

	/* reset the traversed state */
	mutex_lock(&icc_provider_list_mutex);
	list_for_each_entry(provider, &icc_provider_list, provider_list) {
		mutex_lock(&provider->lock);
		list_for_each_entry(tmp_node, &provider->nodes, node_list)
			if (tmp_node->is_traversed)
				tmp_node->is_traversed = false;
		mutex_unlock(&provider->lock);
	}
	mutex_unlock(&icc_provider_list_mutex);

	if (found)
		return path_allocate(dst, number);

	return ERR_PTR(-EPROBE_DEFER);
}

static int path_init(struct device *dev, struct icc_path *path)
{
	struct icc_node *node;
	size_t i;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;
		path->reqs[i].dev = dev;

		mutex_lock(&node->provider->lock);
		node->provider->users++;
		mutex_unlock(&node->provider->lock);
	}

	return 0;
}

static int aggregate(struct icc_node *node, u32 avg_bw, u32 peak_bw,
		     u32 *agg_avg, u32 *agg_peak)
{
	*agg_avg += node->avg_bw + avg_bw;
	*agg_peak = max(node->peak_bw, peak_bw);

	return 0;
}

static void provider_aggregate(struct icc_provider *provider, u32 *avg_bw,
			       u32 *peak_bw)
{
	struct icc_node *n;
	u32 agg_avg = 0;
	u32 agg_peak = 0;

	/* aggregate for the interconnect provider */
	list_for_each_entry(n, &provider->nodes, node_list) {
		if (provider->aggregate)
			provider->aggregate(n, agg_avg, agg_peak,
					    &agg_avg, &agg_peak);
		else
			aggregate(n, agg_avg, agg_peak,
				  &agg_avg, &agg_peak);
	}

	*avg_bw = agg_avg;
	*peak_bw = agg_peak;
}

static int constraints_apply(struct icc_path *path)
{
	struct icc_node *next, *prev = NULL;
	int i;

	for (i = 0; i < path->num_nodes; i++, prev = next) {
		struct icc_provider *provider;
		u32 avg_bw = 0;
		u32 peak_bw = 0;
		int ret;

		next = path->reqs[i].node;
		/*
		 * Both endpoints should be valid master-slave pairs of the
		 * same interconnect provider that will be configured.
		 */
		if (!next || !prev)
			continue;

		if (next->provider != prev->provider)
			continue;

		provider = next->provider;
		mutex_lock(&provider->lock);

		/* aggregate requests for the provider */
		provider_aggregate(provider, &avg_bw, &peak_bw);

		if (provider->set) {
			/* set the constraints */
			ret = provider->set(prev, next, avg_bw, peak_bw);
		}

		mutex_unlock(&provider->lock);

		if (ret)
			return ret;
	}

	return 0;
}

struct icc_path *of_icc_get(struct device *dev, const char *name)
{
	struct device_node *np = NULL;
	u32 src_id, dst_id;
	int index = 0;
	int ret;

	if (dev->of_node)
		np = dev->of_node;

	if (name) {
		index = of_property_match_string(np, "interconnect-names", name);
		if (index < 0)
			return ERR_PTR(index);
	}

	/*
	 * We use a combination of phandle and specifier for endpoint. For now
	 * lets support only global ids and extend this is the future if needed
	 * without breaking DT compatibility.
	 */
	ret = of_property_read_u32_index(np, "interconnects", index * 4 + 1,
					 &src_id);
	if (ret) {
		pr_err("%s: %s src port is invalid (%d)\n", __func__, np->name,
		       ret);
		return ERR_PTR(ret);
	}
	ret = of_property_read_u32_index(np, "interconnects", index * 4 + 3,
					 &dst_id);
	if (ret) {
		pr_err("%s: %s dst port is invalid (%d)\n", __func__, np->name,
		       ret);
		return ERR_PTR(ret);
	}

	return icc_get(dev, src_id, dst_id);
}
EXPORT_SYMBOL_GPL(of_icc_get);

/**
 * icc_set() - set constraints on an interconnect path between two endpoints
 * @path: reference to the path returned by icc_get()
 * @avg_bw: average bandwidth in kbps
 * @peak_bw: peak bandwidth in kbps
 *
 * This function is used by an interconnect consumer to express its own needs
 * in term of bandwidth and QoS for a previously requested path between two
 * endpoints. The requests are aggregated and each node is updated accordingly.
 *
 * Returns 0 on success, or an approproate error code otherwise.
 */
int icc_set(struct icc_path *path, u32 avg_bw, u32 peak_bw)
{
	struct icc_node *node;
	struct icc_provider *p;
	size_t i;
	int ret;

	if (!path)
		return 0;

	for (i = 0; i < path->num_nodes; i++) {
		struct icc_req *r;
		u32 agg_avg = 0;
		u32 agg_peak = 0;

		node = path->reqs[i].node;
		p = node->provider;

		mutex_lock(&icc_path_mutex);

		/* update the consumer request for this path */
		path->reqs[i].avg_bw = avg_bw;
		path->reqs[i].peak_bw = peak_bw;

		/* aggregate requests for this node */
		if (p->aggregate) {
			hlist_for_each_entry(r, &node->req_list, req_node) {
				p->aggregate(node, r->avg_bw, r->peak_bw,
						  &agg_avg, &agg_peak);
			}
			node->avg_bw = agg_avg;
			node->peak_bw = agg_peak;
		} else {
			hlist_for_each_entry(r, &node->req_list, req_node) {
				/* sum(averages) and max(peaks) */
				agg_avg += r->avg_bw;
				agg_peak = max(agg_peak, r->peak_bw);
			}
			node->avg_bw = agg_avg;
			node->peak_bw = agg_peak;
		}

		mutex_unlock(&icc_path_mutex);
	}

	ret = constraints_apply(path);
	if (ret)
		pr_err("interconnect: error applying constraints (%d)", ret);

	return ret;
}
EXPORT_SYMBOL_GPL(icc_set);

/**
 * icc_get() - return a handle for path between two endpoints
 * @dev: the device requesting the path
 * @src_id: source device port id
 * @dst_id: destination device port id
 *
 * This function will search for a path between two endpoints and return an
 * icc_path handle on success. Use icc_put() to release
 * constraints when the they are not needed anymore.
 *
 * Return: icc_path pointer on success, or ERR_PTR() on error
 */
struct icc_path *icc_get(struct device *dev, const int src_id, const int dst_id)
{
	struct icc_node *src, *dst;
	struct icc_path *path = ERR_PTR(-EPROBE_DEFER);

	src = node_find(src_id);
	if (!src) {
		dev_err(dev, "%s: invalid src=%d\n", __func__, src_id);
		goto out;
	}

	dst = node_find(dst_id);
	if (!dst) {
		dev_err(dev, "%s: invalid dst=%d\n", __func__, dst_id);
		goto out;
	}

	mutex_lock(&icc_path_mutex);
	path = path_find(src, dst);
	mutex_unlock(&icc_path_mutex);
	if (IS_ERR(path)) {
		dev_err(dev, "%s: invalid path=%ld\n", __func__, PTR_ERR(path));
		goto out;
	}

	path_init(dev, path);

out:
	return path;
}
EXPORT_SYMBOL_GPL(icc_get);

/**
 * icc_put() - release the reference to the icc_path
 * @path: interconnect path
 *
 * Use this function to release the constraints on a path when the path is
 * no longer needed. The constraints will be re-aggregated.
 */
void icc_put(struct icc_path *path)
{
	struct icc_node *node;
	size_t i;
	int ret;

	if (!path || WARN_ON_ONCE(IS_ERR(path)))
		return;

	ret = icc_set(path, 0, 0);
	if (ret)
		pr_err("%s: error (%d)\n", __func__, ret);

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;
		hlist_del(&path->reqs[i].req_node);

		mutex_lock(&node->provider->lock);
		node->provider->users--;
		mutex_unlock(&node->provider->lock);
	}

	kfree(path);
}
EXPORT_SYMBOL_GPL(icc_put);

/**
 * icc_node_create() - create a node
 * @id: node id
 *
 * Return: icc_node pointer on success, or ERR_PTR() on error
 */
struct icc_node *icc_node_create(int id)
{
	struct icc_node *node;

	/* check if node already exists */
	node = node_find(id);
	if (node)
		return node;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return ERR_PTR(-ENOMEM);

	id = idr_alloc(&icc_idr, node, id, id + 1, GFP_KERNEL);
	if (WARN(id < 0, "couldn't get idr"))
		return ERR_PTR(id);

	node->id = id;

	return node;
}
EXPORT_SYMBOL_GPL(icc_node_create);

/**
 * icc_link_create() - create a link between two nodes
 * @src_id: source node id
 * @dst_id: destination node id
 *
 * Return: 0 on success, or an error code otherwise
 */
int icc_link_create(struct icc_node *node, const int dst_id)
{
	struct icc_node *dst;
	struct icc_node **new;
	int ret = 0;

	if (IS_ERR_OR_NULL(node))
		return PTR_ERR(node);

	mutex_lock(&node->provider->lock);

	dst = node_find(dst_id);
	if (!dst)
		dst = icc_node_create(dst_id);

	new = krealloc(node->links,
		       (node->num_links + 1) * sizeof(*node->links),
		       GFP_KERNEL);
	if (!new) {
		ret = -ENOMEM;
		goto out;
	}

	node->links = new;
	node->links[node->num_links++] = dst;

out:
	mutex_unlock(&node->provider->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(icc_link_create);

/**
 * icc_add_node() - add an interconnect node to interconnect provider
 * @node: pointer to the interconnect node
 * @provider: pointer to the interconnect provider
 *
 * Return: 0 on success, or an error code otherwise
 */
int icc_node_add(struct icc_node *node, struct icc_provider *provider)
{
	if (WARN_ON(!node))
		return -EINVAL;

	if (WARN_ON(!provider))
		return -EINVAL;

	node->provider = provider;

	mutex_lock(&provider->lock);
	list_add(&node->node_list, &provider->nodes);
	mutex_unlock(&provider->lock);

	return 0;
}

/**
 * icc_provider_add() - add a new interconnect provider
 * @icc_provider: the interconnect provider that will be added into topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int icc_provider_add(struct icc_provider *provider)
{
	if (WARN_ON(!provider))
		return -EINVAL;

	if (WARN_ON(!provider->set))
		return -EINVAL;

	mutex_init(&provider->lock);
	INIT_LIST_HEAD(&provider->nodes);

	mutex_lock(&icc_provider_list_mutex);
	list_add(&provider->provider_list, &icc_provider_list);
	mutex_unlock(&icc_provider_list_mutex);

	dev_dbg(provider->dev, "interconnect provider added to topology\n");

	return 0;
}
EXPORT_SYMBOL_GPL(icc_provider_add);

/**
 * icc_provider_del() - delete previously added interconnect provider
 * @icc_provider: the interconnect provider that will be removed from topology
 *
 * Return: 0 on success, or an error code otherwise
 */
int icc_provider_del(struct icc_provider *provider)
{
	mutex_lock(&provider->lock);
	if (provider->users) {
		pr_warn("interconnect provider still has %d users\n",
			provider->users);
	}
	mutex_unlock(&provider->lock);

	mutex_lock(&icc_provider_list_mutex);
	list_del(&provider->provider_list);
	mutex_unlock(&icc_provider_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(icc_provider_del);

MODULE_AUTHOR("Georgi Djakov <georgi.djakov@linaro.org");
MODULE_DESCRIPTION("Interconnect Driver Core");
MODULE_LICENSE("GPL v2");
