/* SPDX-License-Identifier: MIT */
#ifndef PIPEWIREAO_IRIS_CAMERA_H
#define PIPEWIREAO_IRIS_CAMERA_H

#include <stdbool.h>
#include <iris.h>
#include <spa/param/video/raw.h>
#include <spa/support/log.h>
#include <spa/utils/dict.h>

struct iris_camera {
	iris_context_t *context;
	iris_device_t *device;
	iris_device_t *control_device;
	iris_node_map_t *control_map;
	bool control_locked;
	iris_stream_t *stream;
	iris_start_options_t options;
	struct spa_video_info_raw format;
	uint64_t pixel_format;
	uint32_t stride;
	uint32_t size;
};

int iris_camera_open(struct iris_camera *camera, const struct spa_dict *props,
		struct spa_log *log);
int iris_camera_close(struct iris_camera *camera, struct spa_log *log);
int iris_result(iris_status_t status);
int iris_report_error(struct spa_log *log, iris_status_t status);

#endif
