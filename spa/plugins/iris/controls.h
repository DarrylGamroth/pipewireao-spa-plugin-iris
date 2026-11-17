/* SPDX-License-Identifier: MIT */
#ifndef PIPEWIREAO_IRIS_CONTROLS_H
#define PIPEWIREAO_IRIS_CONTROLS_H

#include <spa/node/node.h>

#include "camera.h"

struct iris_controls {
	iris_node_map_t *map;
	char **names;
	uint32_t count;
};

int iris_controls_init(struct iris_controls *controls, iris_node_map_t *map,
		const struct spa_dict *props, struct spa_log *log);
void iris_controls_clear(struct iris_controls *controls);
int iris_controls_enum(const struct iris_controls *controls, struct spa_hook_list *hooks,
		int seq, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter);
int iris_controls_set(const struct iris_controls *controls, uint32_t id, uint32_t flags,
		const struct spa_pod *param, struct spa_log *log);

#endif
