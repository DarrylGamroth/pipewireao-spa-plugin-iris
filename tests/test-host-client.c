/* SPDX-License-Identifier: MIT */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

struct capture {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	const char *name;
	uint32_t frames;
	uint64_t sequence;
	int64_t first_pts;
	int64_t last_pts;
	bool failed;
	bool reported;
};

static void done(struct capture *capture, const char *error)
{
	capture->failed = error != NULL;
	if (error)
		fprintf(stderr, "%s\n", error);
	pw_main_loop_quit(capture->loop);
}

static void state_changed(void *data, enum pw_stream_state old SPA_UNUSED,
		enum pw_stream_state state, const char *error)
{
	if (state == PW_STREAM_STATE_ERROR)
		done(data, error ? error : "stream error");
}

static void process(void *data)
{
	struct capture *capture = data;
	struct pw_buffer *received;

	while ((received = pw_stream_dequeue_buffer(capture->stream)) != NULL) {
		struct spa_buffer *buffer = received->buffer;
		struct spa_meta_header *header = spa_buffer_find_meta_data(buffer,
				SPA_META_Header, sizeof(*header));
		const struct spa_data *image = buffer->n_datas == 1 ? buffer->datas : NULL;
		const struct spa_chunk *chunk = image ? image->chunk : NULL;
		bool valid = image && image->data && chunk && chunk->offset == 0 &&
				chunk->size > 0 && chunk->size <= image->maxsize && chunk->stride > 0 &&
				header && header->pts > 0 &&
				(capture->frames == 0 || (header->seq > capture->sequence &&
				 header->pts >= capture->last_pts));
		if (valid) {
			if (capture->frames == 0)
				capture->first_pts = header->pts;
			capture->last_pts = header->pts;
			capture->sequence = header->seq;
			capture->frames++;
		}
		if (pw_stream_queue_buffer(capture->stream, received) < 0 || !valid) {
			done(capture, "invalid image or buffer return");
			return;
		}
	}
	if (!capture->reported && capture->frames >= 16 &&
			capture->last_pts - capture->first_pts >= 250 * SPA_NSEC_PER_MSEC) {
		printf("%s %u %" PRId64 " %" PRId64 "\n", capture->name,
				capture->frames, capture->first_pts, capture->last_pts);
		fflush(stdout);
		capture->reported = true;
	}
}

static void stop(void *data, int signum SPA_UNUSED)
{
	struct capture *capture = data;
	if (!capture->reported) {
		fprintf(stderr, "%s: stopped after %u frames\n", capture->name, capture->frames);
		capture->failed = true;
	}
	pw_main_loop_quit(capture->loop);
}

int main(int argc, char **argv)
{
	static const struct pw_stream_events events = {
		PW_VERSION_STREAM_EVENTS,
		.state_changed = state_changed,
		.process = process,
	};
	struct capture capture = { 0 };
	uint8_t storage[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
	const struct spa_pod *params[] = {
		spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat,
			&SPA_VIDEO_INFO_RAW_INIT()),
		spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))),
	};

	if (argc != 2)
		return 2;
	pw_init(&argc, &argv);
	capture.loop = pw_main_loop_new(NULL);
	if (!capture.loop)
		return 1;
	capture.name = argv[1];
	if (!pw_loop_add_signal(pw_main_loop_get_loop(capture.loop), SIGTERM, stop, &capture)) {
		pw_main_loop_destroy(capture.loop);
		return 1;
	}
	capture.stream = pw_stream_new_simple(pw_main_loop_get_loop(capture.loop), argv[1],
			pw_properties_new(PW_KEY_NODE_NAME, argv[1],
				PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", NULL),
			&events, &capture);
	if (!capture.stream) {
		pw_main_loop_destroy(capture.loop);
		return 1;
	}
	int res = pw_stream_connect(capture.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_NO_CONVERT,
			params, SPA_N_ELEMENTS(params));
	if (res == 0)
		res = pw_main_loop_run(capture.loop);
	pw_stream_destroy(capture.stream);
	pw_main_loop_destroy(capture.loop);
	pw_deinit();
	return res < 0 || capture.failed || !capture.reported;
}
