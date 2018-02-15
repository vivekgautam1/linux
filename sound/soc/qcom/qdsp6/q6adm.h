// SPDX-License-Identifier: GPL-2.0
#ifndef __Q6_ADM_V2_H__
#define __Q6_ADM_V2_H__

#define ADM_PATH_PLAYBACK	0x1
#define MAX_COPPS_PER_PORT	8
#define NULL_COPP_TOPOLOGY	0x00010312

/* multiple copp per stream. */
struct route_payload {
	int num_copps;
	int session_id;
	int copp_idx[MAX_COPPS_PER_PORT];
	int port_id[MAX_COPPS_PER_PORT];
};

int q6pcm_routing_probe(struct device *dev);
int q6pcm_routing_remove(struct device *dev);

void *q6adm_get_routing_data(struct device *dev);
void q6adm_set_routing_data(struct device *dev, void *data);
int q6adm_open(struct device *dev, int port_id, int path, int rate,
	       int channel_mode, int topology, int perf_mode,
	       uint16_t bit_width, int app_type, int acdb_id);
int q6adm_close(struct device *dev, int port, int topology, int perf_mode);
int q6adm_matrix_map(struct device *dev, int path,
		     struct route_payload payload_map, int perf_mode);

#endif /* __Q6_ADM_V2_H__ */
