/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/node/utils.h>
#include <spa/param/props.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "controls.h"

static const char prefix[] = "genicam.";

void iris_controls_clear(struct iris_controls *controls)
{
	for (uint32_t i = 0; i < controls->count; i++)
		free(controls->names[i]);
	free(controls->names);
	*controls = (struct iris_controls) { 0 };
}

static int add_feature(struct iris_controls *controls, const char *value, int len,
		struct spa_log *log)
{
	char *name = malloc(sizeof(prefix) + len);
	if (!name)
		return -ENOMEM;
	memcpy(name, prefix, sizeof(prefix) - 1);
	char *feature = name + sizeof(prefix) - 1;
	if (spa_json_is_container(value, len) || spa_json_parse_stringn(value, len, feature, len + 1) <= 0 ||
			spa_streq(feature, "TLParamsLocked")) {
		free(name);
		return -EINVAL;
	}
	for (uint32_t i = 0; i < controls->count; i++) {
		if (spa_streq(controls->names[i], name)) {
			free(name);
			return -EINVAL;
		}
	}
	iris_feature_info_t info;
	iris_status_t status = iris_feature_info(controls->map, feature, &info);
	if (status != IRIS_OK || info.type < IRIS_FEATURE_INTEGER || info.type > IRIS_FEATURE_STRING) {
		spa_log_error(log, "Iris runtime control %s must be a scalar GenApi feature", feature);
		free(name);
		return status == IRIS_OK ? -ENOTSUP : iris_result(status);
	}
	const size_t count = controls->count;
	if (count == UINT32_MAX || count >= SIZE_MAX / sizeof(*controls->names)) {
		free(name);
		return -EOVERFLOW;
	}
	char **names = realloc(controls->names, (count + 1) * sizeof(*names));
	if (!names) {
		free(name);
		return -ENOMEM;
	}
	controls->names = names;
	controls->names[controls->count++] = name;
	return 0;
}

int iris_controls_init(struct iris_controls *controls, iris_node_map_t *map,
		const struct spa_dict *props, struct spa_log *log)
{
	const char *spec = props ? spa_dict_lookup(props, "api.iris.runtime-controls") : NULL;
	struct spa_json root, list;
	const char *value;
	int len, res;

	if (!spec)
		return 0;
	if (!map) {
		spa_log_error(log, "Iris runtime controls require a separate control producer");
		return -EINVAL;
	}
	controls->map = map;
	spa_json_init(&root, spec, strlen(spec));
	if (spa_json_enter_array(&root, &list) <= 0)
		return -EINVAL;
	while ((len = spa_json_next(&list, &value)) > 0) {
		if ((res = add_feature(controls, value, len, log)) < 0)
			return res;
	}
	return len < 0 || spa_json_next(&root, &value) != 0 ? -EINVAL : 0;
}

static bool readable(iris_feature_info_t info)
{
	return info.access == IRIS_FEATURE_READ_ONLY || info.access == IRIS_FEATURE_READ_WRITE;
}

static bool writable(iris_feature_info_t info)
{
	return info.access == IRIS_FEATURE_WRITE_ONLY || info.access == IRIS_FEATURE_READ_WRITE;
}

static int append_value(struct spa_pod_builder *builder, iris_node_map_t *map,
		const char *feature, iris_feature_info_t info)
{
	iris_status_t status;
	const bool read = readable(info);
	switch (info.type) {
	case IRIS_FEATURE_INTEGER: {
		int64_t value = 0;
		if (read && (status = iris_feature_get_integer(map, feature, &value)) != IRIS_OK)
			return iris_result(status);
		return spa_pod_builder_long(builder, value);
	}
	case IRIS_FEATURE_FLOAT: {
		double value = 0;
		if (read && (status = iris_feature_get_float(map, feature, &value)) != IRIS_OK)
			return iris_result(status);
		return spa_pod_builder_double(builder, value);
	}
	case IRIS_FEATURE_BOOLEAN: {
		uint32_t value = 0;
		if (read && (status = iris_feature_get_boolean(map, feature, &value)) != IRIS_OK)
			return iris_result(status);
		return spa_pod_builder_bool(builder, value != 0);
	}
	case IRIS_FEATURE_ENUMERATION:
	case IRIS_FEATURE_STRING: {
		char local[128], *value = local;
		size_t length = sizeof(local);
		int res;
		if (!read)
			return spa_pod_builder_string(builder, "");
		status = iris_feature_to_string(map, feature, value, &length);
		if (status == IRIS_BUFFER_TOO_SMALL) {
			value = malloc(length);
			if (!value)
				return -ENOMEM;
			status = iris_feature_to_string(map, feature, value, &length);
		}
		res = status == IRIS_OK ? spa_pod_builder_string(builder, value) : iris_result(status);
		if (value != local)
			free(value);
		return res;
	}
	default:
		return -ENOTSUP;
	}
}

static int build_propinfo(const struct iris_controls *controls, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **out)
{
	struct spa_pod_frame object;
	const char *name = controls->names[index];
	const char *feature = name + sizeof(prefix) - 1;
	iris_feature_info_t info;
	iris_status_t status = iris_feature_info(controls->map, feature, &info);
	if (status != IRIS_OK)
		return iris_result(status);
	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add(builder, SPA_PROP_INFO_name, SPA_POD_String(name),
			SPA_PROP_INFO_params, SPA_POD_Bool(writable(info)), 0);
	spa_pod_builder_prop(builder, SPA_PROP_INFO_type, 0);
	int res = append_value(builder, controls->map, feature, info);
	*out = spa_pod_builder_pop(builder, &object);
	return res;
}

static int build_props(const struct iris_controls *controls, struct spa_pod_builder *builder,
		struct spa_pod **out)
{
	struct spa_pod_frame object, params;
	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(builder, &params);
	for (uint32_t i = 0; i < controls->count; i++) {
		const char *name = controls->names[i];
		const char *feature = name + sizeof(prefix) - 1;
		iris_feature_info_t info;
		iris_status_t status = iris_feature_info(controls->map, feature, &info);
		if (status != IRIS_OK)
			return iris_result(status);
		if (readable(info)) {
			int res = spa_pod_builder_string(builder, name);
			if (res < 0 || (res = append_value(builder, controls->map, feature, info)) < 0)
				return res;
		}
	}
	spa_pod_builder_pop(builder, &params);
	*out = spa_pod_builder_pop(builder, &object);
	return 0;
}

int iris_controls_enum(const struct iris_controls *controls, struct spa_hook_list *hooks,
		int seq, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	if (!controls->count || (id != SPA_PARAM_PropInfo && id != SPA_PARAM_Props))
		return -ENOENT;
	const uint32_t end = id == SPA_PARAM_Props ? 1 : controls->count;
	for (uint32_t i = start; i < end && num; i++) {
		uint8_t storage[512];
		struct spa_pod_dynamic_builder dynamic;
		struct spa_result_node_params result = { .id = id, .index = i, .next = i + 1 };
		struct spa_pod *param = NULL;
		spa_pod_dynamic_builder_init(&dynamic, storage, sizeof(storage), 512);
		int res = id == SPA_PARAM_Props ? build_props(controls, &dynamic.b, &param) :
			build_propinfo(controls, i, &dynamic.b, &param);
		if (res == 0 && !param)
			res = -ENOSPC;
		uint8_t filtered[512];
		struct spa_pod_dynamic_builder match;
		spa_pod_dynamic_builder_init(&match, filtered, sizeof(filtered), 512);
		if (res == 0 && spa_pod_filter(&match.b, &result.param, param, filter) == 0) {
			spa_node_emit_result(hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
			num--;
		}
		spa_pod_dynamic_builder_clean(&match);
		spa_pod_dynamic_builder_clean(&dynamic);
		if (res < 0)
			return res;
	}
	return 0;
}

static int write_value(iris_node_map_t *map, const char *name, uint32_t type,
		const struct spa_pod *pod, bool test, struct spa_log *log)
{
	iris_status_t status = IRIS_OK;
	switch (type) {
	case IRIS_FEATURE_INTEGER: {
		int64_t value;
		int32_t integer;
		if (spa_pod_get_long(pod, &value) < 0) {
			if (spa_pod_get_int(pod, &integer) < 0)
				return -EINVAL;
			value = integer;
		}
		if (!test)
			status = iris_feature_set_integer(map, name, value);
		break;
	}
	case IRIS_FEATURE_FLOAT: {
		double value;
		float real;
		if (spa_pod_get_double(pod, &value) < 0) {
			if (spa_pod_get_float(pod, &real) < 0)
				return -EINVAL;
			value = real;
		}
		if (!test)
			status = iris_feature_set_float(map, name, value);
		break;
	}
	case IRIS_FEATURE_BOOLEAN: {
		bool value;
		if (spa_pod_get_bool(pod, &value) < 0)
			return -EINVAL;
		if (!test)
			status = iris_feature_set_boolean(map, name, value);
		break;
	}
	case IRIS_FEATURE_ENUMERATION:
	case IRIS_FEATURE_STRING: {
		const char *value;
		if (spa_pod_get_string(pod, &value) < 0)
			return -EINVAL;
		if (!test)
			status = iris_feature_from_string(map, name, value);
		break;
	}
	default:
		return -ENOTSUP;
	}
	return status == IRIS_OK ? 0 : iris_report_error(log, status);
}

int iris_controls_set(const struct iris_controls *controls, uint32_t id, uint32_t flags,
		const struct spa_pod *param, struct spa_log *log)
{
	const char *name = NULL;
	struct spa_pod *value = NULL;
	const struct spa_pod_prop *property;

	if (!controls->count || id != SPA_PARAM_Props)
		return -ENOENT;
	if (!param)
		return 0;
	if (!spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
		return -EINVAL;
	SPA_POD_OBJECT_FOREACH((struct spa_pod_object *)param, property) {
		if (property->key != SPA_PROP_params)
			continue;
		struct spa_pod_parser parser;
		struct spa_pod_frame frame;
		spa_pod_parser_pod(&parser, &property->value);
		if (name || spa_pod_parser_push_struct(&parser, &frame) < 0 ||
				spa_pod_parser_get_string(&parser, &name) < 0 ||
				spa_pod_parser_get_pod(&parser, &value) < 0 ||
				spa_pod_parser_current(&parser) != NULL)
			return -EINVAL;
	}
	if (!name)
		return 0;
	for (uint32_t i = 0; i < controls->count; i++) {
		if (!spa_streq(name, controls->names[i]))
			continue;
		const char *feature = name + sizeof(prefix) - 1;
		iris_feature_info_t info;
		iris_status_t status = iris_feature_info(controls->map, feature, &info);
		if (status != IRIS_OK)
			return iris_report_error(log, status);
		if (!writable(info))
			return -EACCES;
		int res = write_value(controls->map, feature, info.type, value,
				flags & SPA_NODE_PARAM_FLAG_TEST_ONLY, log);
		return res == 0 && !(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY) ? 1 : res;
	}
	return -ENOENT;
}
