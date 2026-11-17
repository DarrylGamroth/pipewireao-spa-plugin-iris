/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/parser.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>

#define CHECK(condition) do { 	if (!(condition)) { 		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); 		exit(1); 	} } while (0)

struct graph_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta meta;
	struct spa_meta_header header;
};

struct capture {
	struct spa_handle *handle;
	struct spa_node *node;
	struct spa_video_info_raw format;
	struct spa_io_buffers io;
	struct graph_buffer storage[2];
	struct spa_buffer *buffers[2];
	uint64_t sequence;
	uint32_t frames;
	struct spa_log log;
	char error[256];
};

static void log_error(void *object, enum spa_log_level level,
		const char *file SPA_UNUSED, int line SPA_UNUSED, const char *func SPA_UNUSED,
		const char *format, ...)
{
	struct capture *capture = object;
	if (level != SPA_LOG_LEVEL_ERROR)
		return;
	va_list args;
	va_start(args, format);
	vsnprintf(capture->error, sizeof(capture->error), format, args);
	va_end(args);
}

static void result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct capture *capture = data;
	const struct spa_result_node_params *param = result;
	CHECK(res == 0 && type == SPA_RESULT_TYPE_NODE_PARAMS);
	CHECK(param->id == SPA_PARAM_EnumFormat && param->index == 0);
	CHECK(spa_format_video_raw_parse(param->param, &capture->format) >= 0);
}

static void open_capture(struct capture *capture, const struct spa_handle_factory *factory,
		const struct spa_dict *props, enum spa_video_format expected_format)
{
	static const struct spa_node_events events = {
		SPA_VERSION_NODE_EVENTS,
		.result = result,
	};
	struct spa_hook listener;
	struct spa_node *node;
	void *iface;
	uint8_t pod_memory[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_memory, sizeof(pod_memory));

	static const struct spa_log_methods log_methods = { .version = 0, .log = log_error };
	capture->log = (struct spa_log) {
		.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &log_methods, capture),
		.level = SPA_LOG_LEVEL_ERROR,
	};
	const struct spa_support support[] = {
		{ SPA_TYPE_INTERFACE_Log, &capture->log },
	};
	capture->handle = calloc(1, spa_handle_factory_get_size(factory, props));
	CHECK(capture->handle != NULL);
	int res = spa_handle_factory_init(factory, capture->handle, props, support, SPA_N_ELEMENTS(support));
	if (res < 0)
		fprintf(stderr, "factory init: %s (%d)\n", strerror(-res), res);
	CHECK(res == 0);
	CHECK(spa_handle_get_interface(capture->handle, SPA_TYPE_INTERFACE_Node, &iface) == 0);
	node = capture->node = iface;
	CHECK(spa_node_add_listener(node, &listener, &events, capture) == 0);
	CHECK(spa_node_port_enum_params(node, 0, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_hook_remove(&listener);
	CHECK(capture->format.format == expected_format);
	CHECK(capture->format.size.width > 0 && capture->format.size.height > 0);
	struct spa_video_info_raw wrong = capture->format;
	wrong.size.width++;
	const struct spa_pod *param = spa_format_video_raw_build(&builder, SPA_PARAM_Format, &wrong);
	CHECK(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0, SPA_PARAM_Format,
			SPA_NODE_PARAM_FLAG_TEST_ONLY, param) == -EINVAL);
	spa_pod_builder_init(&builder, pod_memory, sizeof(pod_memory));
	param = spa_format_video_raw_build(&builder, SPA_PARAM_Format, &capture->format);
	CHECK(spa_node_port_set_param(node, SPA_DIRECTION_OUTPUT, 0, SPA_PARAM_Format, 0, param) == 0);

	uint32_t bpp = expected_format == SPA_VIDEO_FORMAT_GRAY8 ? 1 : 2;
	uint32_t size = capture->format.size.width * capture->format.size.height * bpp;
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(capture->storage); i++) {
		struct graph_buffer *buffer = &capture->storage[i];
		buffer->data = (struct spa_data) {
			.type = SPA_DATA_MemPtr,
			.data = malloc(size),
			.maxsize = size,
			.chunk = &buffer->chunk,
		};
		CHECK(buffer->data.data != NULL);
		buffer->meta = (struct spa_meta) { SPA_META_Header, sizeof(buffer->header), &buffer->header };
		buffer->buffer = (struct spa_buffer) { 1, 1, &buffer->meta, &buffer->data };
		capture->buffers[i] = &buffer->buffer;
	}
	capture->storage[0].data.maxsize--;
	CHECK(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			capture->buffers, SPA_N_ELEMENTS(capture->buffers)) == -EINVAL);
	capture->storage[0].data.maxsize++;
	CHECK(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0,
			capture->buffers, SPA_N_ELEMENTS(capture->buffers)) == 0);
	capture->io = (struct spa_io_buffers) { SPA_STATUS_NEED_DATA, SPA_ID_INVALID };
	CHECK(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0, SPA_IO_Buffers,
			&capture->io, sizeof(capture->io)) == 0);
}

static int command(struct capture *capture, uint32_t id)
{
	struct spa_command command = SPA_NODE_COMMAND_INIT(id);
	return spa_node_send_command(capture->node, &command);
}

static bool read_frame(struct capture *capture)
{
	int status = spa_node_process(capture->node);
	CHECK(status == SPA_STATUS_OK || status == SPA_STATUS_HAVE_DATA);
	if (status == SPA_STATUS_OK)
		return false;
	CHECK(capture->io.status == SPA_STATUS_HAVE_DATA);
	CHECK(capture->io.buffer_id < SPA_N_ELEMENTS(capture->storage));
	struct graph_buffer *frame = &capture->storage[capture->io.buffer_id];
	CHECK(frame->chunk.offset == 0 && frame->chunk.size == frame->data.maxsize);
	CHECK(frame->chunk.stride == (int32_t)(frame->chunk.size / capture->format.size.height));
	CHECK(frame->header.pts > 0);
	CHECK(capture->frames == 0 || frame->header.seq > capture->sequence);

	/* Keep the buffer checked out for another process call. */
	const uint32_t id = capture->io.buffer_id;
	const uint64_t seq = frame->header.seq;
	const uint8_t first = ((uint8_t *)frame->data.data)[0];
	CHECK(spa_node_process(capture->node) == SPA_STATUS_OK);
	CHECK(capture->io.buffer_id == id && frame->header.seq == seq);
	CHECK(((uint8_t *)frame->data.data)[0] == first);
	capture->sequence = seq;
	capture->frames++;
	capture->io.status = SPA_STATUS_NEED_DATA;
	return true;
}

static void close_capture(struct capture *capture)
{
	CHECK(command(capture, SPA_NODE_COMMAND_Suspend) == 0);
	CHECK(spa_node_port_set_param(capture->node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, NULL) == 0);
	CHECK(spa_handle_clear(capture->handle) == 0);
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(capture->storage); i++)
		free(capture->storage[i].data.data);
	free(capture->handle);
}

struct control_state {
	double fps;
	char trigger[8];
	char serial[32];
	int64_t width;
	uint32_t info_count;
};

static void control_result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct control_state *state = data;
	const struct spa_result_node_params *param = result;
	CHECK(res == 0 && type == SPA_RESULT_TYPE_NODE_PARAMS);
	if (param->id == SPA_PARAM_PropInfo) {
		const char *name;
		bool writable;
		CHECK(spa_pod_parse_object(param->param, SPA_TYPE_OBJECT_PropInfo, NULL,
				SPA_PROP_INFO_name, SPA_POD_String(&name),
				SPA_PROP_INFO_params, SPA_POD_Bool(&writable)) >= 0);
		CHECK(writable == (strcmp(name, "genicam.AcquisitionFrameRate") == 0 ||
				strcmp(name, "genicam.TriggerMode") == 0));
		state->info_count++;
		return;
	}
	CHECK(param->id == SPA_PARAM_Props && param->index == 0);
	const struct spa_pod_prop *pairs = spa_pod_find_prop(param->param, NULL, SPA_PROP_params);
	CHECK(pairs);
	struct spa_pod_parser parser;
	struct spa_pod_frame frame;
	spa_pod_parser_pod(&parser, &pairs->value);
	CHECK(spa_pod_parser_push_struct(&parser, &frame) == 0);
	while (spa_pod_parser_current(&parser)) {
		const char *name;
		struct spa_pod *value;
		CHECK(spa_pod_parser_get_string(&parser, &name) == 0);
		CHECK(spa_pod_parser_get_pod(&parser, &value) == 0);
		if (strcmp(name, "genicam.AcquisitionFrameRate") == 0)
			CHECK(spa_pod_get_double(value, &state->fps) == 0);
		else if (strcmp(name, "genicam.Width") == 0)
			CHECK(spa_pod_get_long(value, &state->width) == 0);
		else {
			const char *text;
			CHECK(spa_pod_get_string(value, &text) == 0);
			if (strcmp(name, "genicam.TriggerMode") == 0)
				snprintf(state->trigger, sizeof(state->trigger), "%s", text);
			else {
				CHECK(strcmp(name, "genicam.DeviceSerialNumber") == 0);
				snprintf(state->serial, sizeof(state->serial), "%s", text);
			}
		}
	}
}

static struct control_state read_controls(struct capture *capture, uint32_t id)
{
	static const struct spa_node_events events = {
		SPA_VERSION_NODE_EVENTS,
		.result = control_result,
	};
	struct spa_hook listener;
	struct control_state state = { 0 };
	CHECK(spa_node_add_listener(capture->node, &listener, &events, &state) == 0);
	CHECK(spa_node_enum_params(capture->node, 0, id, 0, 8, NULL) == 0);
	spa_hook_remove(&listener);
	return state;
}

static int write_control(struct capture *capture, const char *name,
		const struct spa_pod *value, uint32_t flags)
{
	uint8_t storage[256];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod_frame object, pairs;
	spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&builder, &pairs);
	spa_pod_builder_string(&builder, name);
	spa_pod_builder_primitive(&builder, value);
	spa_pod_builder_pop(&builder, &pairs);
	const struct spa_pod *param = spa_pod_builder_pop(&builder, &object);
	CHECK(param);
	return spa_node_set_param(capture->node, SPA_PARAM_Props, flags, param);
}

struct serial_gate {
	struct capture *capture;
	void (*hold)(const char *);
	int (*wait)(unsigned);
	void (*release)(void);
};

static void *poll_during_control(void *data)
{
	struct serial_gate *gate = data;
	CHECK(gate->wait(2000));
	for (unsigned frame = 0; frame < 4; frame++)
		CHECK(read_frame(gate->capture));
	gate->release();
	return NULL;
}

static void check_control_init_failure(const struct spa_handle_factory *factory,
		const struct spa_dict *props, int expected)
{
	struct spa_handle *handle = calloc(1, spa_handle_factory_get_size(factory, props));
	CHECK(handle);
	CHECK(spa_handle_factory_init(factory, handle, props, NULL, 0) == expected);
	free(handle);
}

static void test_control_pair(const struct spa_handle_factory *factory, const char *stream_cti,
		const char *control_cti, bool no_token)
{
	const char *serial = no_token ? "ALT-TEST-001" : "CRED2-TEST-001";
	struct spa_dict_item props[] = {
		{ "api.iris.producer", stream_cti },
		{ "api.iris.serial-number", "test-serial" },
		{ "api.iris.acquisition-control", "stream-only" },
		{ "api.iris.control-producer", control_cti },
		{ "api.iris.camera-serial", "OTHER-CAMERA" },
		{ "api.iris.runtime-controls", "[ DeviceSerialNumber AcquisitionFrameRate TriggerMode Width ]" },
		{ "api.iris.control-interface-id", "CameraLinkSerial" },
		{ "api.iris.control-device-id", "CLSerial-1" },
	};
	const struct spa_dict properties = SPA_DICT_INIT(props, SPA_N_ELEMENTS(props) - (no_token ? 0 : 2));
	CHECK(unsetenv("CLP_XML_PATH") == 0 && unsetenv("CRED2_SIM_HARDWARE_ID") == 0 &&
			unsetenv("CRED2_SIM_NO_CAMERA") == 0);
	CHECK(setenv("CRED2_SIM_CROP_COLUMNS", "0 1", 1) == 0);
	if (no_token) {
		const struct spa_dict incomplete = SPA_DICT_INIT(props, SPA_N_ELEMENTS(props) - 1);
		check_control_init_failure(factory, &incomplete, -EINVAL);
	}
	check_control_init_failure(factory, &properties, no_token ? -ENODEV : -ENOENT);
	props[4].value = serial;
	if (!no_token) {
		CHECK(setenv("CRED2_SIM_CROP_COLUMNS", "0 31", 1) == 0);
		check_control_init_failure(factory, &properties, -EINVAL);
		CHECK(setenv("CRED2_SIM_CROP_COLUMNS", "0 1", 1) == 0);
	}

	struct capture camera = { 0 };
	open_capture(&camera, factory, &properties, SPA_VIDEO_FORMAT_GRAY16_LE);
	CHECK(read_controls(&camera, SPA_PARAM_PropInfo).info_count == 4);
	CHECK(command(&camera, SPA_NODE_COMMAND_Start) == 0);
	CHECK(read_frame(&camera));
	const struct control_state before = read_controls(&camera, SPA_PARAM_Props);
	CHECK(before.fps == 120 && strcmp(before.trigger, "Off") == 0 &&
			strcmp(before.serial, serial) == 0 && before.width == 2);

	struct spa_pod_double rate = SPA_POD_INIT_Double(200.0);
	struct spa_pod_long width = SPA_POD_INIT_Long(32);
	CHECK(write_control(&camera, "genicam.AcquisitionFrameRate", &rate.pod,
			SPA_NODE_PARAM_FLAG_TEST_ONLY) == 0);
	CHECK(read_controls(&camera, SPA_PARAM_Props).fps == 120);
	CHECK(write_control(&camera, "genicam.Width", &width.pod, 0) == -EACCES);
	CHECK(write_control(&camera, "genicam.Height", &width.pod, 0) == -ENOENT);

	const char *serial_library = getenv("CLPROTOCOL_GENTL_CONFIG")
		? getenv("CLPROTOCOL_GENTL_SERIAL") : getenv("CRED2_CL_SERIAL");
	CHECK(serial_library != NULL);
	void *mock = dlopen(serial_library, RTLD_NOW | RTLD_LOCAL);
	CHECK(mock);
	struct serial_gate gate = {
		.capture = &camera,
		.hold = dlsym(mock, "cred2_sim_hold_command"),
		.wait = dlsym(mock, "cred2_sim_wait_for_command"),
		.release = dlsym(mock, "cred2_sim_release_command"),
	};
	CHECK(gate.hold && gate.wait && gate.release);
	pthread_t poller;
	gate.hold(no_token ? "RATE=200.000000" : "set fps 200.000000");
	CHECK(pthread_create(&poller, NULL, poll_during_control, &gate) == 0);
	CHECK(write_control(&camera, "genicam.AcquisitionFrameRate", &rate.pod, 0) == 0);
	CHECK(pthread_join(poller, NULL) == 0);

	struct spa_pod_string trigger = { { 3, SPA_TYPE_String } };
	struct {
		struct spa_pod_string pod;
		char text[8];
	} on = { trigger, "On" };
	CHECK(write_control(&camera, "genicam.TriggerMode", &on.pod.pod, 0) == 0);
	gate.hold(no_token ? "RATE?" : "fps raw");
	CHECK(pthread_create(&poller, NULL, poll_during_control, &gate) == 0);
	const struct control_state after = read_controls(&camera, SPA_PARAM_Props);
	CHECK(pthread_join(poller, NULL) == 0);
	CHECK(after.fps == 200 && strcmp(after.trigger, "On") == 0 && after.width == 2);
	CHECK(read_frame(&camera) && camera.frames == 10);
	CHECK(dlclose(mock) == 0);
	close_capture(&camera);
	printf("Paired %s: frame rate 120 to 200 Hz, TriggerMode Off to On, %u frames including 8 during blocked serial I/O\n",
			serial, camera.frames);
}

int main(int argc, char **argv)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	CHECK(argc >= 2 && argc <= 6);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!library)
		fprintf(stderr, "%s\n", dlerror());
	CHECK(library != NULL);
	spa_handle_factory_enum_func_t enumerate = (spa_handle_factory_enum_func_t)
		dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	CHECK(enumerate != NULL && enumerate(&factory, &index) == 1);
	CHECK(strcmp(factory->name, "api.iris.source") == 0 && enumerate(&factory, &index) == 0);
	struct spa_handle *empty = calloc(1, spa_handle_factory_get_size(factory, NULL));
	CHECK(empty != NULL);
	CHECK(spa_handle_factory_init(factory, empty, NULL, NULL, 0) == -EINVAL);
	free(empty);
	if (argc >= 5 && strcmp(argv[3], "--control") == 0) {
		CHECK(argc == 5 || strcmp(argv[5], "no-token") == 0);
		test_control_pair(factory, argv[2], argv[4], argc == 6);
	} else if (argc >= 3) {
		CHECK(argc <= 5);
		struct spa_dict_item props[] = {
			{ "api.iris.producer", argv[2] },
			{ "api.iris.acquisition-control", "stream-only" },
			{ "api.iris.buffer-count", argc >= 4 ? argv[3] : NULL },
		};
		static const uint8_t pixels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
		const struct spa_dict properties = SPA_DICT_INIT(props, argc >= 4 ? 3 : 2);
		if (argc == 5) {
			struct capture rejected = { 0 };
			props[2].value = argv[4];
			open_capture(&rejected, factory, &properties, SPA_VIDEO_FORMAT_GRAY16_LE);
			CHECK(command(&rejected, SPA_NODE_COMMAND_Start) == -ERANGE);
			CHECK(strstr(rejected.error,
					"Iris stream start: requested buffer count 1 is below producer minimum 2") != NULL);
			CHECK(spa_node_process(rejected.node) == SPA_STATUS_OK);
			CHECK(rejected.io.status == SPA_STATUS_NEED_DATA);
			close_capture(&rejected);
			props[2].value = argv[3];
		}
		struct capture mock = { 0 };
		open_capture(&mock, factory, &properties, SPA_VIDEO_FORMAT_GRAY16_LE);
		CHECK(mock.format.size.width == 2 && mock.format.size.height == 4);
		for (unsigned cycle = 0; cycle < 2; ++cycle) {
			CHECK(command(&mock, SPA_NODE_COMMAND_Start) == 0);
			for (unsigned frame = 0; frame < 12; ++frame) {
				CHECK(read_frame(&mock));
				struct graph_buffer *buffer = &mock.storage[mock.io.buffer_id];
				CHECK(buffer->chunk.size == sizeof(pixels));
				CHECK(memcmp(buffer->data.data, pixels, sizeof(pixels)) == 0);
			}
			/* Keep one SPA buffer leased while requesting another frame. */
			const uint32_t held_id = mock.io.buffer_id;
			const struct graph_buffer *held = &mock.storage[held_id];
			const uint64_t sequence = held->header.seq;
			mock.io.buffer_id = SPA_ID_INVALID;
			CHECK(read_frame(&mock));
			CHECK(mock.io.buffer_id != held_id && held->header.seq == sequence);
			CHECK(memcmp(held->data.data, pixels, sizeof(pixels)) == 0);
			CHECK(spa_node_port_reuse_buffer(mock.node, 0, held_id) == 0);
			CHECK(command(&mock, SPA_NODE_COMMAND_Pause) == 0);
			CHECK(spa_node_process(mock.node) == SPA_STATUS_OK);
		}
		printf("Iris mock: %u frames across two acquisition cycles\n", mock.frames);
		close_capture(&mock);
	}
	CHECK(dlclose(library) == 0);
	return 0;
}
