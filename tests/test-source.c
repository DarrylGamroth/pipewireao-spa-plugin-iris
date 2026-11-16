/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/video/format-utils.h>
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
};

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

	capture->handle = calloc(1, spa_handle_factory_get_size(factory, props));
	CHECK(capture->handle != NULL);
	int res = spa_handle_factory_init(factory, capture->handle, props, NULL, 0);
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

static void command(struct capture *capture, uint32_t id)
{
	struct spa_command command = SPA_NODE_COMMAND_INIT(id);
	CHECK(spa_node_send_command(capture->node, &command) == 0);
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
	command(capture, SPA_NODE_COMMAND_Suspend);
	CHECK(spa_node_port_set_param(capture->node, SPA_DIRECTION_OUTPUT, 0,
			SPA_PARAM_Format, 0, NULL) == 0);
	CHECK(spa_handle_clear(capture->handle) == 0);
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(capture->storage); i++)
		free(capture->storage[i].data.data);
	free(capture->handle);
}

int main(int argc, char **argv)
{
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	CHECK(argc >= 2 && argc <= 4);
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
	if (argc >= 3) {
		const struct spa_dict_item props[] = {
			{ "api.iris.producer", argv[2] },
			{ "api.iris.acquisition-control", "stream-only" },
			{ "api.iris.buffer-count", argc == 4 ? argv[3] : NULL },
		};
		static const uint8_t pixels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
		struct capture mock = { 0 };
		const struct spa_dict properties = SPA_DICT_INIT(props, argc == 4 ? 3 : 2);
		open_capture(&mock, factory, &properties, SPA_VIDEO_FORMAT_GRAY16_LE);
		CHECK(mock.format.size.width == 2 && mock.format.size.height == 4);
		for (unsigned cycle = 0; cycle < 2; ++cycle) {
			command(&mock, SPA_NODE_COMMAND_Start);
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
			command(&mock, SPA_NODE_COMMAND_Pause);
			CHECK(spa_node_process(mock.node) == SPA_STATUS_OK);
		}
		printf("Iris mock: %u frames across two acquisition cycles\n", mock.frames);
		close_capture(&mock);
	}
	CHECK(dlclose(library) == 0);
	return 0;
}
