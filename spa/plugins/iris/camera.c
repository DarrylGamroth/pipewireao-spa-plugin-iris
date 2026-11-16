/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <string.h>

#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "camera.h"

static const struct {
	const char *symbol;
	uint32_t pfnc;
	enum spa_video_format spa;
	uint32_t bytes_per_pixel;
} pixel_formats[] = {
	{ "Mono8",  0x01080001, SPA_VIDEO_FORMAT_GRAY8,     1 },
	{ "Mono10", 0x01100003, SPA_VIDEO_FORMAT_GRAY16_LE, 2 },
	{ "Mono12", 0x01100005, SPA_VIDEO_FORMAT_GRAY16_LE, 2 },
	{ "Mono14", 0x01100025, SPA_VIDEO_FORMAT_GRAY16_LE, 2 },
	{ "Mono16", 0x01100007, SPA_VIDEO_FORMAT_GRAY16_LE, 2 },
};

int iris_result(iris_status_t status)
{
	switch (status) {
	case IRIS_OK: return 0;
	case IRIS_INVALID_ARGUMENT: return -EINVAL;
	case IRIS_NOT_FOUND: return -ENOENT;
	case IRIS_AMBIGUOUS: return -ENOTUNIQ;
	case IRIS_NOT_SUPPORTED: return -ENOTSUP;
	case IRIS_ACCESS_DENIED: return -EACCES;
	case IRIS_BUSY: return -EBUSY;
	case IRIS_NO_MEMORY: return -ENOMEM;
	case IRIS_OUT_OF_RANGE: return -ERANGE;
	default: return -EIO;
	}
}

int iris_report_error(struct spa_log *log, iris_status_t status)
{
	iris_error_t error;
	iris_last_error(&error);
	spa_log_error(log, "Iris %s: %s (status %d)",
			error.operation ? error.operation : "camera",
			error.message ? error.message : "unknown error", status);
	return iris_result(status);
}

static const char *property(const struct spa_dict *props, const char *key)
{
	return props ? spa_dict_lookup(props, key) : NULL;
}

static int open_device(struct iris_camera *camera, const struct spa_dict *props,
		struct spa_log *log)
{
	const uint64_t update_timeout_ms = 250;
	iris_device_selector_t selector = {
		.producer = property(props, "api.iris.producer"),
		.interface_id = property(props, "api.iris.interface-id"),
		.device_id = property(props, "api.iris.device-id"),
		.serial_number = property(props, "api.iris.serial-number"),
	};
	iris_device_list_t *devices = NULL;
	iris_device_info_t info;
	iris_status_t status;
	int res;

	if (selector.producer == NULL || *selector.producer == '\0') {
		spa_log_error(log, "api.iris.producer is required");
		return -EINVAL;
	}
	if ((selector.device_id && (!selector.interface_id || selector.serial_number)) ||
			(selector.interface_id && !selector.device_id && !selector.serial_number))
		return -EINVAL;

	if (!selector.device_id && !selector.serial_number) {
		status = iris_discover(camera->context, selector.producer,
				update_timeout_ms, &devices);
		if (status != IRIS_OK)
			return iris_report_error(log, status);
		size_t count = iris_device_list_count(devices);
		if (count != 1) {
			spa_log_error(log, "%s: found %zu cameras; select by serial or exact IDs",
					selector.producer, count);
			res = count ? -ENOTUNIQ : -ENOENT;
			goto done;
		}
		status = iris_device_list_get(devices, 0, &info);
		if (status != IRIS_OK) {
			res = iris_report_error(log, status);
			goto done;
		}
		selector.interface_id = info.interface_id;
		selector.device_id = info.device_id;
	}
	status = iris_device_open(camera->context, &selector, IRIS_DEVICE_CONTROL,
			update_timeout_ms, &camera->device);
	res = status == IRIS_OK ? 0 : iris_report_error(log, status);
done:
	iris_device_list_close(&devices);
	return res;
}

static int configure_features(iris_node_map_t *map, const char *controls,
		struct spa_log *log)
{
	struct spa_json root, values;
	const char *value;
	char name[256], setting[1024];
	int len;

	if (controls == NULL)
		return 0;
	spa_json_init(&root, controls, strlen(controls));
	if (spa_json_enter_array(&root, &values) <= 0)
		return -EINVAL;
	while ((len = spa_json_next(&values, &value)) > 0) {
		if (spa_json_is_container(value, len) ||
				spa_json_parse_stringn(value, len, name, sizeof(name)) <= 0)
			return -EINVAL;
		len = spa_json_next(&values, &value);
		if (len <= 0 || spa_json_is_container(value, len) ||
				spa_json_parse_stringn(value, len, setting, sizeof(setting)) < 0)
			return -EINVAL;
		iris_status_t status = iris_feature_from_string(map, name, setting);
		if (status != IRIS_OK) {
			spa_log_error(log, "api.iris.initial-controls: cannot set %s", name);
			return iris_report_error(log, status);
		}
	}
	return len < 0 || spa_json_next(&root, &value) != 0 ? -EINVAL : 0;
}

static int read_format(struct iris_camera *camera, iris_node_map_t *map,
		struct spa_log *log)
{
	char symbol[64];
	size_t length = sizeof(symbol);
	int64_t width, height;
	iris_status_t status;

	status = iris_feature_get_enumeration(map, "PixelFormat", symbol, &length);
	if (status == IRIS_OK)
		status = iris_feature_get_integer(map, "Width", &width);
	if (status == IRIS_OK)
		status = iris_feature_get_integer(map, "Height", &height);
	if (status != IRIS_OK)
		return iris_report_error(log, status);
	if (width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX)
		return -ERANGE;
	for (size_t i = 0; i < SPA_N_ELEMENTS(pixel_formats); i++) {
		if (!spa_streq(symbol, pixel_formats[i].symbol))
			continue;
		uint64_t stride = width * pixel_formats[i].bytes_per_pixel;
		if (stride > INT32_MAX || (uint64_t)height > INT32_MAX / stride)
			return -ERANGE;
		camera->pixel_format = pixel_formats[i].pfnc;
		camera->format.format = pixel_formats[i].spa;
		camera->format.size = SPA_RECTANGLE(width, height);
		camera->format.framerate = SPA_FRACTION(0, 1);
		camera->stride = stride;
		camera->size = stride * height;
		return 0;
	}
	spa_log_error(log, "unsupported GenICam PixelFormat %s", symbol);
	return -ENOTSUP;
}

int iris_camera_open(struct iris_camera *camera, const struct spa_dict *props,
		struct spa_log *log)
{
	iris_node_map_t *map = NULL;
	iris_status_t status;
	const char *value;
	uint32_t stream_index = 0;
	int res;

	camera->options.acquisition_control = IRIS_ACQUISITION_REMOTE_AND_STREAM;
	if ((value = property(props, "api.iris.acquisition-control")) != NULL) {
		if (spa_streq(value, "stream-only"))
			camera->options.acquisition_control = IRIS_ACQUISITION_STREAM_ONLY;
		else if (!spa_streq(value, "remote-and-stream"))
			return -EINVAL;
	}
	if ((value = property(props, "api.iris.stream-index")) != NULL &&
			!spa_atou32(value, &stream_index, 10))
		return -EINVAL;
	if ((value = property(props, "api.iris.buffer-count")) != NULL &&
			(!spa_atou32(value, &camera->options.buffer_count, 10) ||
			 camera->options.buffer_count == 0))
		return -EINVAL;

	status = iris_context_create(&camera->context);
	if (status != IRIS_OK)
		return iris_report_error(log, status);
	if ((res = open_device(camera, props, log)) < 0)
		return res;
	status = iris_device_node_map(camera->device, IRIS_REMOTE_DEVICE, &map);
	if (status != IRIS_OK)
		return iris_report_error(log, status);
	if ((res = configure_features(map, property(props, "api.iris.initial-controls"), log)) < 0 ||
			(res = read_format(camera, map, log)) < 0)
		return res;
	status = iris_stream_open(camera->device, stream_index, &camera->stream);
	return status == IRIS_OK ? 0 : iris_report_error(log, status);
}

int iris_camera_close(struct iris_camera *camera, struct spa_log *log)
{
	iris_status_t status = IRIS_OK;
	if (camera->stream != NULL)
		status = iris_stream_close(&camera->stream);
	if (status == IRIS_OK && camera->device != NULL)
		status = iris_device_close(&camera->device);
	if (status == IRIS_OK && camera->context != NULL)
		status = iris_context_close(&camera->context);
	return status == IRIS_OK ? 0 : iris_report_error(log, status);
}
