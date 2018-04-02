/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#ifndef _LINUX_INTERCONNECT_PROVIDER_H
#define _LINUX_INTERCONNECT_PROVIDER_H

#include <linux/interconnect.h>

struct icc_node;

/**
 * struct icc_provider - interconnect provider (controller) entity that might
 * provide multiple interconnect controls
 *
 * @provider_list: list of the registered interconnect providers
 * @nodes: internal list of the interconnect provider nodes
 * @set: pointer to device specific set operation function
 * @aggregate: pointer to device specific aggregate operation function
 * @dev: the device this interconnect provider belongs to
 * @lock: lock to provide consistency during aggregation/update of constraints
 * @users: count of active users
 * @data: pointer to private data
 */
struct icc_provider {
	struct list_head	provider_list;
	struct list_head	nodes;
	int (*set)(struct icc_node *src, struct icc_node *dst,
		   u32 avg_bw, u32 peak_bw);
	int (*aggregate)(struct icc_node *node, u32 avg_bw, u32 peak_bw,
			 u32 *agg_avg, u32 *agg_peak);
	struct device		*dev;
	struct mutex		lock;
	int			users;
	void			*data;
};

/**
 * struct icc_node - entity that is part of the interconnect topology
 *
 * @id: platform specific node id
 * @name: node name used in debugfs
 * @links: a list of targets where we can go next when traversing
 * @num_links: number of links to other interconnect nodes
 * @provider: points to the interconnect provider of this node
 * @node_list: list of interconnect nodes associated with @provider
 * @search_list: list used when walking the nodes graph
 * @reverse: pointer to previous node when walking the nodes graph
 * @is_traversed: flag that is used when walking the nodes graph
 * @req_list: a list of QoS constraint requests associated with this node
 * @avg_bw: aggregated value of average bandwidth
 * @peak_bw: aggregated value of peak bandwidth
 * @data: pointer to private data
 */
struct icc_node {
	int			id;
	const char              *name;
	struct icc_node		**links;
	size_t			num_links;

	struct icc_provider	*provider;
	struct list_head	node_list;
	struct list_head	orphan_list;
	struct list_head	search_list;
	struct icc_node		*reverse;
	bool			is_traversed;
	struct hlist_head	req_list;
	u32			avg_bw;
	u32			peak_bw;
	void			*data;
};

#if IS_ENABLED(CONFIG_INTERCONNECT)

struct icc_node *icc_node_create(int id);
int icc_node_add(struct icc_node *node, struct icc_provider *provider);
int icc_link_create(struct icc_node *node, const int dst_id);
int icc_provider_add(struct icc_provider *provider);
int icc_provider_del(struct icc_provider *provider);

#else

static inline struct icc_node *icc_node_create(int id)
{
	return ERR_PTR(-ENOTSUPP);
}

int icc_node_add(struct icc_node *node, struct icc_provider *provider)
{
	return -ENOTSUPP;
}

static inline int icc_link_create(struct icc_node *node, const int dst_id)
{
	return -ENOTSUPP;
}

static inline int icc_provider_add(struct icc_provider *provider)
{
	return -ENOTSUPP;
}

static inline int icc_provider_del(struct icc_provider *provider)
{
	return -ENOTSUPP;
}

#endif /* CONFIG_INTERCONNECT */

#endif /* _LINUX_INTERCONNECT_PROVIDER_H */
