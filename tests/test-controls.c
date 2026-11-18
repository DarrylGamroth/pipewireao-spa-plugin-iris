/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <string.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include "controls.h"

static void check_status_mapping(void)
{
	static const struct { iris_status_t status; int error; } cases[] = {
		{ IRIS_OK, 0 },
		{ IRIS_INVALID_ARGUMENT, -EINVAL },
		{ IRIS_NOT_FOUND, -ENOENT },
		{ IRIS_AMBIGUOUS, -ENOTUNIQ },
		{ IRIS_NOT_SUPPORTED, -ENOTSUP },
		{ IRIS_ACCESS_DENIED, -EACCES },
		{ IRIS_BUSY, -EBUSY },
		{ IRIS_NO_MEMORY, -ENOMEM },
		{ IRIS_OUT_OF_RANGE, -ERANGE },
		{ IRIS_IO_ERROR, -EIO },
	};
	for (size_t i = 0; i < SPA_N_ELEMENTS(cases); i++)
		spa_assert_se(iris_result(cases[i].status) == cases[i].error);
}

static int set_control(const struct iris_controls *controls, const char *name,
		const struct spa_pod *value, uint32_t flags)
{
	uint8_t storage[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod_frame object, params;

	spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &params);
	spa_pod_builder_string(&builder, name);
	spa_pod_builder_primitive(&builder, value);
	spa_pod_builder_pop(&builder, &params);
	return iris_controls_set(controls, SPA_PARAM_Props, flags,
			spa_pod_builder_pop(&builder, &object), NULL);
}

static void exercise_controls(iris_node_map_t *map)
{
	struct spa_dict_item item = SPA_DICT_ITEM_INIT("api.iris.runtime-controls", NULL);
	const struct spa_dict props = SPA_DICT_INIT(&item, 1);
	static const struct { const char *list; int error; } malformed[] = {
		{ "Width", -EINVAL },
		{ "[Width Width]", -EINVAL },
		{ "[[Width]]", -EINVAL },
		{ "[TLParamsLocked]", -EINVAL },
		{ "[MissingFeature]", -ENOENT },
		{ "[TriggerSoftware]", -ENOTSUP },
	};
	struct iris_controls controls = { 0 };
	for (size_t i = 0; i < SPA_N_ELEMENTS(malformed); i++) {
		item.value = malformed[i].list;
		spa_assert_se(iris_controls_init(&controls, map, &props, NULL) == malformed[i].error);
		iris_controls_clear(&controls);
	}
	item.value = "[Width ExposureTime ReverseX DeviceUserID TriggerMode]";
	spa_assert_se(iris_controls_init(&controls, NULL, &props, NULL) == -EINVAL);
	spa_assert_se(iris_controls_init(&controls, map, &props, NULL) == 0);
	spa_assert_se(controls.count == 5);
	struct spa_hook_list hooks;
	spa_hook_list_init(&hooks);
	spa_assert_se(iris_controls_enum(&controls, &hooks, 0, SPA_PARAM_PropInfo, 0, 5, NULL) == 0);
	spa_assert_se(iris_controls_enum(&controls, &hooks, 0, SPA_PARAM_Props, 0, 1, NULL) == 0);

	const struct spa_pod_int width = SPA_POD_INIT_Int(64);
	const struct spa_pod_float exposure = SPA_POD_INIT_Float(1000.0f);
	const struct spa_pod_bool reverse = SPA_POD_INIT_Bool(true);
	const struct spa_pod_int wrong = SPA_POD_INIT_Int(0);
	uint8_t malformed_pod[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(malformed_pod, sizeof(malformed_pod));
	const struct spa_pod *bad = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_params, SPA_POD_Int(0));
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Props, 0, bad, NULL) == -EINVAL);
	spa_pod_builder_init(&builder, malformed_pod, sizeof(malformed_pod));
	const struct spa_pod *empty = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, 0);
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Props, 0, empty, NULL) == 0);
	spa_pod_builder_init(&builder, malformed_pod, sizeof(malformed_pod));
	const struct spa_pod *mute = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(true));
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Props, 0, mute, NULL) == 0);
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Props, 0, NULL, NULL) == 0);
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Props, 0, &wrong.pod, NULL) == -EINVAL);
	spa_assert_se(iris_controls_set(&controls, SPA_PARAM_Format, 0, NULL, NULL) == -ENOENT);
	spa_assert_se(set_control(&controls, "genicam.NotExposed", &width.pod, 0) == -ENOENT);
	spa_assert_se(set_control(&controls, "genicam.Width", &reverse.pod, 0) == -EINVAL);
	spa_assert_se(set_control(&controls, "genicam.ExposureTime", &reverse.pod, 0) == -EINVAL);
	spa_assert_se(set_control(&controls, "genicam.ReverseX", &wrong.pod, 0) == -EINVAL);
	spa_assert_se(set_control(&controls, "genicam.TriggerMode", &wrong.pod, 0) == -EINVAL);
	spa_assert_se(set_control(&controls, "genicam.Width", &width.pod, 0) == 1);
	spa_assert_se(set_control(&controls, "genicam.ExposureTime", &exposure.pod, 0) == 1);
	spa_assert_se(set_control(&controls, "genicam.ReverseX", &reverse.pod, 0) == 1);

	int64_t actual_width;
	double actual_exposure;
	uint32_t actual_reverse;
	spa_assert_se(iris_feature_get_integer(map, "Width", &actual_width) == IRIS_OK &&
			actual_width == 64);
	spa_assert_se(iris_feature_get_float(map, "ExposureTime", &actual_exposure) == IRIS_OK &&
			actual_exposure == 1000.0);
	spa_assert_se(iris_feature_get_boolean(map, "ReverseX", &actual_reverse) == IRIS_OK &&
			actual_reverse);
	iris_controls_clear(&controls);
}

int main(int argc, char **argv)
{
	check_status_mapping();
	if (argc == 1)
		return 0;

	spa_assert_se(argc == 2);
	iris_context_t *context = NULL;
	iris_device_t *device = NULL;
	iris_node_map_t *map = NULL;
	const iris_device_selector_t selector = { argv[1], "test-interface", "test-device", NULL };

	spa_assert_se(iris_context_create(&context) == IRIS_OK);
	spa_assert_se(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 250, &device) == IRIS_OK);
	spa_assert_se(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map) == IRIS_OK);
	exercise_controls(map);
	spa_assert_se(iris_device_close(&device) == IRIS_OK);
	spa_assert_se(iris_context_close(&context) == IRIS_OK);
	return 0;
}
