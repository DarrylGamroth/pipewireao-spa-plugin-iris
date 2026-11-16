/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/node/keys.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>

#include "camera.h"

#define MAX_BUFFERS 32

struct source {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_hook_list hooks;
	struct spa_log *log;
	struct iris_camera camera;
	pthread_mutex_t lock;
	struct spa_port_info port_info;
	struct spa_param_info params[5];
	struct spa_io_buffers *io;
	struct spa_buffer *buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t next_buffer;
	_Atomic uint32_t leased;
	uint64_t sequence;
	bool discontinuity;
	bool have_format;
	bool started;
	bool stop_pending;
};

static const struct spa_dict_item properties[] = {
	{ "media.class", "Video/Source" },
	{ SPA_KEY_NODE_DRIVER, "true" },
};
static struct spa_dict node_props = SPA_DICT_INIT_ARRAY(properties);

static bool valid_port(enum spa_direction direction, uint32_t port)
{
	return direction == SPA_DIRECTION_OUTPUT && port == 0;
}

static int add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct source *self = object;
	struct spa_hook_list save;
	const struct spa_node_info info = {
		.max_output_ports = 1,
		.change_mask = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS,
		.flags = SPA_NODE_FLAG_RT | SPA_NODE_FLAG_POLL_DRIVER,
		.props = &node_props,
	};
	struct spa_port_info port = self->port_info;

	port.change_mask = SPA_PORT_CHANGE_MASK_FLAGS | SPA_PORT_CHANGE_MASK_PARAMS;
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	spa_node_emit_info(&self->hooks, &info);
	spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_OUTPUT, 0, &port);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

static int set_callbacks(void *object SPA_UNUSED,
		const struct spa_node_callbacks *callbacks SPA_UNUSED, void *data SPA_UNUSED)
{
	/* A poll driver is clocked by the host graph. It never signals ready. */
	return 0;
}

static int stop(struct source *self)
{
	self->started = false;
	if (!self->stop_pending)
		return 0;
	int res = iris_result(iris_stream_stop(self->camera.stream));
	if (res == 0)
		self->stop_pending = false;
	return res;
}

static int send_command(void *object, const struct spa_command *command)
{
	struct source *self = object;
	int res = 0;

	pthread_mutex_lock(&self->lock);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (self->started)
			break;
		if (self->stop_pending) {
			res = -EBUSY;
			break;
		}
		if (!self->have_format || self->n_buffers == 0 || self->io == NULL) {
			res = -EIO;
			break;
		}
		const uint32_t requested_count = self->camera.options.buffer_count;
		iris_buffer_requirements_t requirements;
		if (requested_count &&
				iris_stream_buffer_requirements(self->camera.stream, &requirements) == IRIS_OK &&
				requested_count < requirements.minimum_buffer_count) {
			spa_log_error(self->log,
					"Iris stream start: requested buffer count %" PRIu32 " is below producer minimum %zu",
					requested_count, requirements.minimum_buffer_count);
			res = -ERANGE;
			break;
		}
		self->stop_pending = true;
		const iris_status_t status = iris_stream_start(self->camera.stream, &self->camera.options);
		if (status != IRIS_OK) {
			res = iris_report_error(self->log, status);
			(void)stop(self);
			break;
		}
		self->discontinuity = true;
		self->started = true;
		break;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		res = stop(self);
		break;
	default:
		res = -ENOTSUP;
	}
	pthread_mutex_unlock(&self->lock);
	return res;
}

static int port_enum_params(void *object, int seq, enum spa_direction direction,
		uint32_t port, uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct source *self = object;
	uint8_t storage[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	struct spa_pod *param = NULL;
	struct spa_result_node_params result = { .id = id, .index = start, .next = 1 };

	if (!valid_port(direction, port) || num == 0)
		return -EINVAL;
	if (start != 0)
		return 0;
	switch (id) {
	case SPA_PARAM_Format:
		if (!self->have_format)
			return 0;
		/* fall through */
	case SPA_PARAM_EnumFormat:
		param = spa_format_video_raw_build(&builder, id, &self->camera.format);
		break;
	case SPA_PARAM_Buffers:
		if (!self->have_format)
			return 0;
		param = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size, SPA_POD_Int(self->camera.size),
				SPA_PARAM_BUFFERS_stride, SPA_POD_Int(self->camera.stride),
				SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
					(1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd)));
		break;
	case SPA_PARAM_Meta:
		param = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
		break;
	case SPA_PARAM_IO:
		param = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
		break;
	default:
		return -ENOENT;
	}
	if (param == NULL)
		return -ENOSPC;
	if (spa_pod_filter(&builder, &result.param, param, filter) >= 0)
		spa_node_emit_result(&self->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
	return 0;
}

static void clear_buffers(struct source *self)
{
	self->n_buffers = 0;
	self->next_buffer = 0;
	atomic_store_explicit(&self->leased, 0, memory_order_relaxed);
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct source *self = object;
	struct spa_video_info_raw format = { 0 };
	uint32_t media, subtype;
	int res = 0;
	bool changed = false;

	if (!valid_port(direction, port))
		return -EINVAL;
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (param && (spa_format_parse(param, &media, &subtype) < 0 ||
			media != SPA_MEDIA_TYPE_video || subtype != SPA_MEDIA_SUBTYPE_raw ||
			spa_format_video_raw_parse(param, &format) < 0 ||
			format.format != self->camera.format.format ||
			format.size.width != self->camera.format.size.width ||
			format.size.height != self->camera.format.size.height ||
			SPA_FLAG_IS_SET(format.flags, SPA_VIDEO_FLAG_MODIFIER)))
		return -EINVAL;

	pthread_mutex_lock(&self->lock);
	if (self->stop_pending && (param || self->io))
		res = -EBUSY;
	else if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		/* The host may detach the IO area before unsetting the format. */
		if (!param && (res = stop(self)) < 0)
			goto done;
		if (!param)
			clear_buffers(self);
		if (self->have_format != (param != NULL)) {
			self->have_format = param != NULL;
			self->params[1].flags = SPA_PARAM_INFO_WRITE |
					(self->have_format ? SPA_PARAM_INFO_READ : 0);
			self->params[2].flags = self->have_format ? SPA_PARAM_INFO_READ : 0;
			changed = true;
		}
	}
done:
	pthread_mutex_unlock(&self->lock);
	if (changed) {
		struct spa_port_info info = self->port_info;
		info.change_mask = SPA_PORT_CHANGE_MASK_PARAMS;
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_OUTPUT, 0, &info);
	}
	return res;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t port, uint32_t flags, struct spa_buffer **buffers, uint32_t count)
{
	struct source *self = object;
	int res = 0;

	if (!valid_port(direction, port) || count > MAX_BUFFERS || (count && !buffers))
		return -EINVAL;
	if (count && (flags & SPA_NODE_BUFFERS_FLAG_ALLOC))
		return -ENOTSUP;
	pthread_mutex_lock(&self->lock);
	if ((self->stop_pending && (count || self->io)) || (count && self->n_buffers)) {
		res = -EBUSY;
		goto done;
	}
	if (count && (!self->have_format || count < 2)) {
		res = -EINVAL;
		goto done;
	}
	if (!count && (res = stop(self)) < 0)
		goto done;
	for (uint32_t i = 0; i < count; i++) {
		struct spa_buffer *buffer = buffers[i];
		struct spa_data *data;
		if (!buffer || buffer->n_datas != 1 || !buffer->datas ||
				!(data = buffer->datas)->data || !data->chunk ||
				data->maxsize < self->camera.size ||
				(data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_MemFd)) {
			res = -EINVAL;
			goto done;
		}
	}
	clear_buffers(self);
	if (count) {
		memcpy(self->buffers, buffers, count * sizeof(buffers[0]));
		self->n_buffers = count;
	}
done:
	pthread_mutex_unlock(&self->lock);
	return res;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port, uint32_t id, void *data, size_t size)
{
	struct source *self = object;

	if (!valid_port(direction, port) || (data && size < sizeof(struct spa_io_buffers)))
		return -EINVAL;
	if (id != SPA_IO_Buffers)
		return -ENOENT;
	pthread_mutex_lock(&self->lock);
	self->io = data;
	pthread_mutex_unlock(&self->lock);
	return 0;
}

static int port_reuse_buffer(void *object, uint32_t port, uint32_t id)
{
	struct source *self = object;
	if (port != 0 || id >= MAX_BUFFERS)
		return -EINVAL;
	atomic_fetch_and_explicit(&self->leased, ~(UINT32_C(1) << id), memory_order_release);
	return 0;
}

static bool copy_image(const struct iris_camera *camera, struct spa_data *dst,
		const iris_frame_t *frame)
{
	const uint32_t required = IRIS_FRAME_HAS_PIXEL_FORMAT | IRIS_FRAME_HAS_STRIDE;
	/* GenTL's PIXELENDIANNESS_BIG value. */
	const int32_t big_endian = 2;
	const uint32_t height = camera->format.size.height;

	if (frame->status != IRIS_FRAME_COMPLETE || !frame->data ||
			(frame->valid & required) != required ||
			frame->pixel_format != camera->pixel_format ||
			frame->width != camera->format.size.width || frame->height != height ||
			(camera->format.format == SPA_VIDEO_FORMAT_GRAY16_LE &&
			 (frame->valid & IRIS_FRAME_HAS_PIXEL_ENDIANNESS) &&
			 frame->pixel_endianness == big_endian) ||
			frame->stride_bytes < camera->stride ||
			frame->image_size / frame->stride_bytes < height ||
			dst->maxsize < camera->size)
		return false;

	if (frame->stride_bytes == camera->stride)
		memcpy(dst->data, frame->data, camera->size);
	else {
		const uint8_t *pixels = frame->data;
		uint8_t *output = dst->data;
		for (uint32_t row = 0; row < height; row++)
			memcpy(output + (size_t)row * camera->stride,
					pixels + row * frame->stride_bytes, camera->stride);
	}
	return true;
}

static int process_frame(struct source *self)
{
	struct spa_io_buffers *io = self->io;
	iris_frame_t frame;
	iris_status_t status;

	if (!self->started || io == NULL)
		return SPA_STATUS_OK;
	if (io->status < 0 || (io->status & SPA_STATUS_HAVE_DATA))
		return SPA_STATUS_OK;
	if (io->buffer_id < self->n_buffers)
		port_reuse_buffer(self, 0, io->buffer_id);
	io->buffer_id = SPA_ID_INVALID;

	const uint32_t leased = atomic_load_explicit(&self->leased, memory_order_acquire);
	uint32_t id = self->next_buffer;
	for (uint32_t available = self->n_buffers; leased & (UINT32_C(1) << id); available--) {
		if (available == 1)
			return SPA_STATUS_OK;
		id = (id + 1) % self->n_buffers;
	}

	status = iris_stream_try_get_frame(self->camera.stream, &frame);
	if (status == IRIS_NO_FRAME || status == IRIS_BUSY)
		return SPA_STATUS_OK;
	if (status != IRIS_OK) {
		self->started = false;
		return iris_result(status);
	}

	struct spa_buffer *output = self->buffers[id];
	bool copied = copy_image(&self->camera, &output->datas[0], &frame);
	int64_t pts = (frame.valid & IRIS_FRAME_HAS_HOST_TIMESTAMP_NS) &&
			frame.host_timestamp_ns <= INT64_MAX ?
			(int64_t)frame.host_timestamp_ns : SPA_TIME_INVALID;
	uint64_t sequence = self->sequence++;
	status = iris_stream_release_frame(self->camera.stream, &frame);
	if (status != IRIS_OK) {
		self->started = false;
		return iris_result(status);
	}
	if (!copied) {
		self->discontinuity = true;
		return SPA_STATUS_OK;
	}

	*output->datas[0].chunk = (struct spa_chunk) {
		.size = self->camera.size,
		.stride = (int32_t)self->camera.stride,
	};
	struct spa_meta_header *header = spa_buffer_find_meta_data(output,
			SPA_META_Header, sizeof(*header));
	if (header)
		*header = (struct spa_meta_header) {
			.flags = self->discontinuity ? SPA_META_HEADER_FLAG_DISCONT : 0,
			.pts = pts,
			.seq = sequence,
		};
	self->discontinuity = false;
	self->next_buffer = (id + 1) % self->n_buffers;
	atomic_fetch_or_explicit(&self->leased, UINT32_C(1) << id, memory_order_relaxed);
	io->buffer_id = id;
	io->status = SPA_STATUS_HAVE_DATA;
	return SPA_STATUS_HAVE_DATA;
}

static int process(void *object)
{
	struct source *self = object;
	if (pthread_mutex_trylock(&self->lock) != 0)
		return SPA_STATUS_OK;
	int res = process_frame(self);
	pthread_mutex_unlock(&self->lock);
	return res;
}

static const struct spa_node_methods methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.set_callbacks = set_callbacks,
	.send_command = send_command,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = port_reuse_buffer,
	.process = process,
};

static int get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct source *self = (struct source *)handle;
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct source *self = (struct source *)handle;
	pthread_mutex_lock(&self->lock);
	int res = stop(self);
	int close_res = iris_camera_close(&self->camera, self->log);
	pthread_mutex_unlock(&self->lock);
	pthread_mutex_destroy(&self->lock);
	return close_res ? close_res : res;
}

static size_t get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct source);
}

static int init(const struct spa_handle_factory *factory SPA_UNUSED,
		struct spa_handle *handle, const struct spa_dict *props,
		const struct spa_support *support, uint32_t n_support)
{
	struct source *self = (struct source *)handle;
	int res;

	*self = (struct source) { 0 };
	self->handle = (struct spa_handle) { SPA_VERSION_HANDLE, get_interface, clear };
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &methods, self);
	self->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_hook_list_init(&self->hooks);
	self->port_info = SPA_PORT_INFO_INIT();
	self->port_info.flags = SPA_PORT_FLAG_LIVE;
	self->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	self->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	self->params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers, 0);
	self->params[3] = SPA_PARAM_INFO(SPA_PARAM_Meta, SPA_PARAM_INFO_READ);
	self->params[4] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	self->port_info.params = self->params;
	self->port_info.n_params = SPA_N_ELEMENTS(self->params);
	atomic_init(&self->leased, 0);
	if ((res = pthread_mutex_init(&self->lock, NULL)) != 0)
		return -res;
	if ((res = iris_camera_open(&self->camera, props, self->log)) < 0)
		(void)clear(handle);
	return res;
}

static int enum_interface_info(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	static const struct spa_interface_info node = { SPA_TYPE_INTERFACE_Node };
	if (*index != 0)
		return 0;
	*info = &node;
	++*index;
	return 1;
}

static const struct spa_handle_factory factory = {
	SPA_VERSION_HANDLE_FACTORY, "api.iris.source", NULL, get_size, init,
	enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **result, uint32_t *index)
{
	if (!result || !index)
		return -EINVAL;
	if (*index != 0)
		return 0;
	*result = &factory;
	++*index;
	return 1;
}
