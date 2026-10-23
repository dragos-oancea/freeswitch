/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2026, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * switch_core_stt_bucket.c -- many calls -> channels of one Ogg/Opus stream -> one STT connection
 *
 * Data path:
 *
 *   RTP read (switch_core_media_read_frame, per call thread)
 *      -> private decoder + resampler (per slot)  -> slot PCM ring buffer
 *   bucket clock thread (one per bucket, every ptime)
 *      -> pull one frame from every slot (silence if none) -> interleave N channels
 *      -> libopusenc pull encoder, channel mapping family 255 (N independent mono streams)
 *      -> Ogg pages -> sink (file / tcp / module-provided, e.g. provider websocket)
 *
 * A call keeps its channel index for its whole lifetime. A freed channel is kept
 * silent for quarantine_ms before reuse, and every occupancy is recorded as
 * (stream id, channel, start ms, end ms, uuid) so results coming back from the
 * provider per channel can be routed to the right call.
 */

#include <switch.h>
#include "switch_stt_bucket.h"

#ifdef HAVE_LIBOPUSENC
#include <opus/opusenc.h>
#endif

#define STT_BUCKET_MAX_BUCKETS 64
#define STT_BUCKET_HISTORY 8
#define STT_BUCKET_STREAM_ID_LEN 64
#define STT_BUCKET_DEC_BUF (SWITCH_RECOMMENDED_BUFFER_SIZE * 2)
#define STT_BUCKET_RETRY_US (1000 * 1000)

typedef enum {
	SLOT_FREE = 0,
	SLOT_ACTIVE,     /* a call is feeding audio */
	SLOT_DRAINING,   /* call detached, buffered audio still being sent */
	SLOT_QUARANTINE  /* silent, not reusable until free_at */
} slot_state_t;

typedef struct {
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char peer_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char stream_id[STT_BUCKET_STREAM_ID_LEN];
	uint64_t start_ms;
	uint64_t end_ms; /* UINT64_MAX while open */
} slot_history_t;

struct switch_stt_bucket_slot_s {
	switch_stt_bucket_t *bucket;
	int index;
	switch_mutex_t *mutex;
	slot_state_t state;
	uint32_t generation;
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char peer_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	switch_buffer_t *buf;
	switch_bool_t primed;
	switch_time_t free_at;
	uint64_t dropped_ms;
	uint64_t underruns;

	/* decoding, only touched by the feeding call thread (under mutex) */
	switch_codec_t codec;
	switch_bool_t codec_ready;
	char codec_key[128];
	switch_audio_resampler_t *resampler;
	uint32_t resampler_from;
	int16_t dec[STT_BUCKET_DEC_BUF];

	/* which stream epoch the current occupant was announced in */
	uint32_t announced_epoch;
	slot_history_t history[STT_BUCKET_HISTORY];
	int history_pos;
};

struct switch_stt_bucket_s {
	char name[16];
	int idx;
	int channels;
	uint32_t rate;
	switch_memory_pool_t *pool;
	switch_mutex_t *mutex; /* thread lifecycle + slot allocation */
	switch_stt_bucket_slot_t *slots;
	switch_thread_t *thread;
	volatile switch_bool_t running;
	volatile switch_bool_t stop;

	/* clock thread only */
	uint32_t epoch;
	char stream_id[STT_BUCKET_STREAM_ID_LEN];
	switch_bool_t stream_open;
	uint64_t samples_out;
	switch_time_t retry_at;
	int16_t *mix;
	int16_t *tmp;
#ifdef HAVE_LIBOPUSENC
	OggOpusEnc *enc;
	OggOpusComments *comments;
#endif
	const switch_stt_bucket_sink_t *sink;
	void *sink_data;
	uint64_t pages, bytes;
};

static struct {
	switch_memory_pool_t *pool;
	/* lock order: mutex -> bucket->mutex -> slot->mutex. The clock threads never take `mutex`,
	   so it may be held while joining them; config and sinks live under cfg_mutex. */
	switch_mutex_t *mutex;
	switch_mutex_t *cfg_mutex;
	switch_stt_bucket_config_t cfg;
	switch_stt_bucket_t *buckets[STT_BUCKET_MAX_BUCKETS];
	int nbuckets;
	switch_hash_t *sinks;
	switch_bool_t ready;
	char node[16];
} globals;

static uint64_t stream_ms(switch_stt_bucket_t *b)
{
	return b->rate ? (b->samples_out * 1000) / b->rate : 0;
}

static void fire_slot_event(const char *subclass, switch_stt_bucket_t *b, switch_stt_bucket_slot_t *slot, slot_history_t *h)
{
	switch_event_t *event;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, subclass) != SWITCH_STATUS_SUCCESS) {
		return;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", h->uuid);
	if (*h->peer_uuid) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Peer-UUID", h->peer_uuid);
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Bucket", b->name);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Stream-ID", h->stream_id);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-Channel", "%d", slot->index);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-Start-Ms", "%" SWITCH_UINT64_T_FMT, h->start_ms);
	if (h->end_ms != UINT64_MAX) {
		switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-End-Ms", "%" SWITCH_UINT64_T_FMT, h->end_ms);
	}
	switch_event_fire(&event);
}

static void fire_stream_event(switch_stt_bucket_t *b, const char *action)
{
	switch_event_t *event;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, SWITCH_STT_BUCKET_EVENT_STREAM) != SWITCH_STATUS_SUCCESS) {
		return;
	}
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Bucket", b->name);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Stream-ID", b->stream_id);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "STT-Action", action);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-Channels", "%d", b->channels);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-Rate", "%u", b->rate);
	switch_event_add_header(event, SWITCH_STACK_BOTTOM, "STT-Duration-Ms", "%" SWITCH_UINT64_T_FMT, stream_ms(b));
	switch_event_fire(&event);
}

/* ---- history (slot mutex held) ---- */

static slot_history_t *history_open(switch_stt_bucket_slot_t *slot, const char *stream_id, uint64_t start_ms)
{
	slot_history_t *h;

	slot->history_pos = (slot->history_pos + 1) % STT_BUCKET_HISTORY;
	h = &slot->history[slot->history_pos];
	switch_copy_string(h->uuid, slot->uuid, sizeof(h->uuid));
	switch_copy_string(h->peer_uuid, slot->peer_uuid, sizeof(h->peer_uuid));
	switch_copy_string(h->stream_id, stream_id, sizeof(h->stream_id));
	h->start_ms = start_ms;
	h->end_ms = UINT64_MAX;
	return h;
}

static slot_history_t *history_close(switch_stt_bucket_slot_t *slot, uint64_t end_ms)
{
	slot_history_t *h = &slot->history[slot->history_pos];

	if (*h->uuid && h->end_ms == UINT64_MAX) {
		h->end_ms = end_ms;
		return h;
	}
	return NULL;
}

/* ---- built-in sinks ---- */

typedef struct {
	FILE *fp;
} file_sink_t;

static switch_status_t file_sink_open(switch_stt_bucket_t *b, const char *stream_id, int channels, int rate, void **sink_data)
{
	char path[1024];
	char tplbuf[512];
	const char *tpl = tplbuf;
	const char *p;
	file_sink_t *fs;

	switch_mutex_lock(globals.cfg_mutex);
	switch_copy_string(tplbuf, globals.cfg.file_path, sizeof(tplbuf));
	switch_mutex_unlock(globals.cfg_mutex);

	if (zstr(tpl)) {
		tpl = "/tmp/stt_bucket_%s.opus";
	}
	if ((p = strstr(tpl, "%s"))) {
		switch_snprintf(path, sizeof(path), "%.*s%s%s", (int)(p - tpl), tpl, stream_id, p + 2);
	} else {
		switch_snprintf(path, sizeof(path), "%s", tpl);
	}
	if (!(fs = calloc(1, sizeof(*fs))) || !(fs->fp = fopen(path, "wb"))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] %s: cannot open %s\n", b->name, path);
		switch_safe_free(fs);
		return SWITCH_STATUS_FALSE;
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] %s: writing stream %s to %s\n", b->name, stream_id, path);
	*sink_data = fs;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t file_sink_write(switch_stt_bucket_t *b, void *sink_data, const uint8_t *page, switch_size_t len)
{
	file_sink_t *fs = sink_data;

	if (fwrite(page, 1, len, fs->fp) != len) {
		return SWITCH_STATUS_FALSE;
	}
	fflush(fs->fp);
	return SWITCH_STATUS_SUCCESS;
}

static void file_sink_close(switch_stt_bucket_t *b, void *sink_data)
{
	file_sink_t *fs = sink_data;

	if (fs) {
		if (fs->fp) fclose(fs->fp);
		free(fs);
	}
}

static const switch_stt_bucket_sink_t file_sink = { "file", file_sink_open, file_sink_write, file_sink_close };

typedef struct {
	switch_memory_pool_t *pool;
	switch_socket_t *sock;
} tcp_sink_t;

static switch_status_t tcp_sink_open(switch_stt_bucket_t *b, const char *stream_id, int channels, int rate, void **sink_data)
{
	tcp_sink_t *ts;
	switch_sockaddr_t *sa;
	char host[256];
	char *port_s;
	switch_port_t port;

	switch_mutex_lock(globals.cfg_mutex);
	switch_copy_string(host, globals.cfg.tcp_target, sizeof(host));
	switch_mutex_unlock(globals.cfg_mutex);
	if (!(port_s = strrchr(host, ':')) || !(port = (switch_port_t)atoi(port_s + 1))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] tcp-target must be host:port\n");
		return SWITCH_STATUS_FALSE;
	}
	*port_s = '\0';

	if (!(ts = calloc(1, sizeof(*ts)))) {
		return SWITCH_STATUS_MEMERR;
	}
	switch_core_new_memory_pool(&ts->pool);
	if (switch_sockaddr_info_get(&sa, host, SWITCH_UNSPEC, port, 0, ts->pool) != SWITCH_STATUS_SUCCESS ||
		switch_socket_create(&ts->sock, switch_sockaddr_get_family(sa), SOCK_STREAM, SWITCH_PROTO_TCP, ts->pool) != SWITCH_STATUS_SUCCESS) {
		goto fail;
	}
	switch_socket_timeout_set(ts->sock, 2000000);
	if (switch_socket_connect(ts->sock, sa) != SWITCH_STATUS_SUCCESS) {
		goto fail;
	}
	/* sends must not stall the mixer clock for long */
	switch_socket_timeout_set(ts->sock, 100000);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] %s: stream %s connected to %s:%u\n", b->name, stream_id, host, port);
	*sink_data = ts;
	return SWITCH_STATUS_SUCCESS;

  fail:
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] %s: cannot connect to %s:%u\n", b->name, host, port);
	if (ts->sock) switch_socket_close(ts->sock);
	switch_core_destroy_memory_pool(&ts->pool);
	free(ts);
	return SWITCH_STATUS_FALSE;
}

static switch_status_t tcp_sink_write(switch_stt_bucket_t *b, void *sink_data, const uint8_t *page, switch_size_t len)
{
	tcp_sink_t *ts = sink_data;
	switch_size_t off = 0;

	while (off < len) {
		switch_size_t n = len - off;
		if (switch_socket_send(ts->sock, (const char *)page + off, &n) != SWITCH_STATUS_SUCCESS || n == 0) {
			return SWITCH_STATUS_FALSE;
		}
		off += n;
	}
	return SWITCH_STATUS_SUCCESS;
}

static void tcp_sink_close(switch_stt_bucket_t *b, void *sink_data)
{
	tcp_sink_t *ts = sink_data;

	if (ts) {
		if (ts->sock) {
			switch_socket_shutdown(ts->sock, SWITCH_SHUTDOWN_WRITE);
			switch_socket_close(ts->sock);
		}
		switch_core_destroy_memory_pool(&ts->pool);
		free(ts);
	}
}

static const switch_stt_bucket_sink_t tcp_sink = { "tcp", tcp_sink_open, tcp_sink_write, tcp_sink_close };

/* ---- stream (clock thread only) ---- */

#ifdef HAVE_LIBOPUSENC
static void flush_pages(switch_stt_bucket_t *b, int flush)
{
	unsigned char *page;
	opus_int32 len;

	while (b->enc && ope_encoder_get_page(b->enc, &page, &len, flush)) {
		if (b->sink && b->sink_data) {
			if (b->sink->write(b, b->sink_data, page, (switch_size_t)len) != SWITCH_STATUS_SUCCESS) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[stt_bucket] %s: sink %s write failed, restarting stream\n",
								  b->name, b->sink->name);
				b->sink->close(b, b->sink_data);
				b->sink_data = NULL;
				break;
			}
		}
		b->pages++;
		b->bytes += (uint64_t)len;
	}
}
#endif

static void stream_close(switch_stt_bucket_t *b, switch_bool_t drain)
{
	int i;
	uint64_t end = stream_ms(b);

	if (!b->stream_open) {
		return;
	}

#ifdef HAVE_LIBOPUSENC
	if (b->enc) {
		if (drain && b->sink_data) {
			ope_encoder_drain(b->enc);
			flush_pages(b, 1);
		}
		ope_encoder_destroy(b->enc);
		b->enc = NULL;
	}
	if (b->comments) {
		ope_comments_destroy(b->comments);
		b->comments = NULL;
	}
#endif
	if (b->sink && b->sink_data) {
		b->sink->close(b, b->sink_data);
	}
	b->sink_data = NULL;
	b->stream_open = SWITCH_FALSE;

	/* close every open occupancy record of this stream; quarantine is per stream, so free those slots */
	for (i = 0; i < b->channels; i++) {
		switch_stt_bucket_slot_t *slot = &b->slots[i];
		slot_history_t *h;

		switch_mutex_lock(slot->mutex);
		if ((h = history_close(slot, end))) {
			fire_slot_event(SWITCH_STT_BUCKET_EVENT_DETACH, b, slot, h);
		}
		slot->announced_epoch = 0;
		if (slot->state == SLOT_QUARANTINE) {
			slot->state = SLOT_FREE;
		}
		switch_mutex_unlock(slot->mutex);
	}

	fire_stream_event(b, "close");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] %s: stream %s closed after %" SWITCH_UINT64_T_FMT " ms, %"
					  SWITCH_UINT64_T_FMT " pages, %" SWITCH_UINT64_T_FMT " bytes\n", b->name, b->stream_id, end, b->pages, b->bytes);
}

static switch_status_t stream_open(switch_stt_bucket_t *b)
{
#ifdef HAVE_LIBOPUSENC
	switch_stt_bucket_config_t cfg;
	int err = 0, family;

	switch_stt_bucket_get_config(&cfg);

	switch_mutex_lock(globals.cfg_mutex);
	b->sink = switch_core_hash_find(globals.sinks, cfg.sink);
	switch_mutex_unlock(globals.cfg_mutex);

	if (!b->sink) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] %s: no sink named [%s]\n", b->name, cfg.sink);
		return SWITCH_STATUS_FALSE;
	}

	if (!*globals.node || !strcmp(globals.node, "fs")) {
		const char *core_uuid = switch_core_get_uuid();
		switch_copy_string(globals.node, zstr(core_uuid) ? "fs" : core_uuid, 9);
	}

	b->epoch++;
	b->samples_out = 0;
	b->pages = b->bytes = 0;
	switch_snprintf(b->stream_id, sizeof(b->stream_id), "%s-%s-%" SWITCH_TIME_T_FMT "-%u", globals.node, b->name,
					switch_epoch_time_now(NULL), b->epoch);

	b->comments = ope_comments_create();
	ope_comments_add(b->comments, "ENCODER", "FreeSWITCH stt_bucket");
	ope_comments_add(b->comments, "STT_STREAM_ID", b->stream_id);

	/* family 255: N uncoupled mono streams, channel i == call i, no cross-channel coding */
	family = b->channels > 1 ? 255 : 0;
	if (!(b->enc = ope_encoder_create_pull(b->comments, (opus_int32)b->rate, b->channels, family, &err))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] %s: encoder: %s\n", b->name, ope_strerror(err));
		ope_comments_destroy(b->comments);
		b->comments = NULL;
		return SWITCH_STATUS_FALSE;
	}
	ope_encoder_ctl(b->enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP));
	ope_encoder_ctl(b->enc, OPUS_SET_COMPLEXITY(5));
	ope_encoder_ctl(b->enc, OPUS_SET_BITRATE(cfg.bitrate_per_channel * b->channels));
	/* DTX makes idle channels nearly free on the wire, but ffmpeg's family-255 decoder (used by many
	   STT backends) mixes audio between channels when streams are in DTX: off unless asked for */
	ope_encoder_ctl(b->enc, OPUS_SET_DTX(cfg.dtx ? 1 : 0));
	/* libopusenc defaults to a 2 s decision delay (it holds audio back to pick frame sizes):
	   far too late for streaming STT, so encode as soon as a frame is there */
	ope_encoder_ctl(b->enc, OPE_SET_DECISION_DELAY(0));
	/* the muxing delay is counted in 48 kHz samples */
	ope_encoder_ctl(b->enc, OPE_SET_MUXING_DELAY(48 * cfg.page_ms));

	if (b->sink->open(b, b->stream_id, b->channels, (int)b->rate, &b->sink_data) != SWITCH_STATUS_SUCCESS) {
		ope_encoder_destroy(b->enc);
		b->enc = NULL;
		ope_comments_destroy(b->comments);
		b->comments = NULL;
		b->sink_data = NULL;
		return SWITCH_STATUS_FALSE;
	}

	b->stream_open = SWITCH_TRUE;
	fire_stream_event(b, "open");
	return SWITCH_STATUS_SUCCESS;
#else
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] built without libopusenc\n");
	return SWITCH_STATUS_FALSE;
#endif
}

/* one mixer tick: returns number of slots holding a call (active or draining) */
static int bucket_tick(switch_stt_bucket_t *b, uint32_t frame, int page_due, int quarantine_ms)
{
	int i, busy = 0;
	switch_size_t frame_bytes = frame * sizeof(int16_t);
	uint64_t now_ms = stream_ms(b);
	switch_time_t now = switch_micro_time_now();

	memset(b->mix, 0, frame_bytes * b->channels);

	for (i = 0; i < b->channels; i++) {
		switch_stt_bucket_slot_t *slot = &b->slots[i];
		switch_size_t got = 0;
		uint32_t s;

		switch_mutex_lock(slot->mutex);

		if (slot->state == SLOT_ACTIVE || slot->state == SLOT_DRAINING) {
			busy++;

			if (b->stream_open && slot->announced_epoch != b->epoch) {
				/* first time this occupant appears in this stream */
				slot_history_t *h = history_open(slot, b->stream_id, now_ms);
				slot->announced_epoch = b->epoch;
				fire_slot_event(SWITCH_STT_BUCKET_EVENT_ATTACH, b, slot, h);
			}

			/* 2 frames of prebuffer absorb the jitter between the call clock and ours */
			if (!slot->primed && switch_buffer_inuse(slot->buf) >= frame_bytes * 2) {
				slot->primed = SWITCH_TRUE;
			}

			if (slot->primed || slot->state == SLOT_DRAINING) {
				got = switch_buffer_read(slot->buf, b->tmp, frame_bytes);
				if (got < frame_bytes && slot->state == SLOT_ACTIVE) {
					slot->underruns++;
					slot->primed = SWITCH_FALSE;
				}
			}

			if (slot->state == SLOT_DRAINING && !switch_buffer_inuse(slot->buf)) {
				slot_history_t *h = history_close(slot, now_ms + (got / sizeof(int16_t)) * 1000 / b->rate);

				if (h) {
					fire_slot_event(SWITCH_STT_BUCKET_EVENT_DETACH, b, slot, h);
				}
				slot->state = SLOT_QUARANTINE;
				slot->free_at = now + (switch_time_t)quarantine_ms * 1000;
				slot->announced_epoch = 0;
				*slot->uuid = '\0';
				*slot->peer_uuid = '\0';
			}
		} else if (slot->state == SLOT_QUARANTINE && now >= slot->free_at) {
			slot->state = SLOT_FREE;
		}

		switch_mutex_unlock(slot->mutex);

		/* interleave: sample s of channel i lives at mix[s * channels + i] */
		for (s = 0; s < got / sizeof(int16_t); s++) {
			b->mix[s * b->channels + i] = b->tmp[s];
		}
	}

#ifdef HAVE_LIBOPUSENC
	if (b->stream_open && b->enc) {
		int err = ope_encoder_write(b->enc, b->mix, (int)frame);

		if (err != OPE_OK) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] %s: encode: %s\n", b->name, ope_strerror(err));
		}
		b->samples_out += frame;
		flush_pages(b, page_due);
		if (!b->sink_data) {
			/* sink died: the receiver needs fresh headers, so start a new stream */
			stream_close(b, SWITCH_FALSE);
			b->retry_at = now;
		}
	}
#endif

	return busy;
}

static void *SWITCH_THREAD_FUNC bucket_thread(switch_thread_t *thread, void *obj)
{
	switch_stt_bucket_t *b = (switch_stt_bucket_t *) obj;
	switch_stt_bucket_config_t cfg;
	switch_time_t next;
	uint32_t frame;
	int idle_ms = 0, since_page = 0;

	switch_stt_bucket_get_config(&cfg);
	frame = b->rate * (uint32_t)cfg.ptime / 1000;
	b->retry_at = 0;
	next = switch_micro_time_now();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] %s: clock started (%d ch, %u Hz, %d ms)\n",
					  b->name, b->channels, b->rate, cfg.ptime);

	while (!b->stop) {
		int busy, page_due;
		switch_time_t now;

		next += (switch_time_t)cfg.ptime * 1000;
		now = switch_micro_time_now();
		if (next > now) {
			switch_sleep(next - now);
		} else if (now - next > 1000 * 1000) {
			/* we fell far behind (suspend, overload): resync instead of bursting */
			next = now;
		}

		if (!b->stream_open && switch_micro_time_now() >= b->retry_at) {
			if (stream_open(b) != SWITCH_STATUS_SUCCESS) {
				b->retry_at = switch_micro_time_now() + STT_BUCKET_RETRY_US;
			}
			since_page = 0;
		}

		since_page += cfg.ptime;
		page_due = since_page >= cfg.page_ms;
		if (page_due) {
			since_page = 0;
		}

		busy = bucket_tick(b, frame, page_due, cfg.quarantine_ms);

		if (busy) {
			idle_ms = 0;
			continue;
		}

		if ((idle_ms += cfg.ptime) >= cfg.idle_timeout_ms) {
			int i, still = 0;

			/* re-check under the bucket lock so a concurrent attach either sees running == FALSE or is counted here */
			switch_mutex_lock(b->mutex);
			for (i = 0; i < b->channels; i++) {
				if (b->slots[i].state == SLOT_ACTIVE || b->slots[i].state == SLOT_DRAINING) {
					still++;
				}
			}
			if (!still) {
				b->running = SWITCH_FALSE;
			}
			switch_mutex_unlock(b->mutex);

			if (!still) {
				break;
			}
			idle_ms = 0;
		}
	}

	stream_close(b, SWITCH_TRUE);
	b->running = SWITCH_FALSE;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] %s: clock stopped\n", b->name);
	return NULL;
}

/* ---- buckets ---- */

static switch_stt_bucket_t *bucket_create(int idx, const switch_stt_bucket_config_t *cfg)
{
	switch_stt_bucket_t *b;
	int i;

	b = switch_core_alloc(globals.pool, sizeof(*b));
	b->idx = idx;
	switch_snprintf(b->name, sizeof(b->name), "b%d", idx);
	b->channels = cfg->channels;
	b->rate = cfg->rate;
	b->pool = globals.pool;
	switch_mutex_init(&b->mutex, SWITCH_MUTEX_NESTED, globals.pool);
	b->slots = switch_core_alloc(globals.pool, sizeof(switch_stt_bucket_slot_t) * b->channels);
	/* worst case frame: 60 ms at 48 kHz */
	b->mix = switch_core_alloc(globals.pool, sizeof(int16_t) * 2880 * b->channels);
	b->tmp = switch_core_alloc(globals.pool, sizeof(int16_t) * 2880);

	for (i = 0; i < b->channels; i++) {
		switch_stt_bucket_slot_t *slot = &b->slots[i];

		slot->bucket = b;
		slot->index = i;
		slot->history_pos = STT_BUCKET_HISTORY - 1;
		switch_mutex_init(&slot->mutex, SWITCH_MUTEX_NESTED, globals.pool);
		switch_buffer_create_dynamic(&slot->buf, 4096, 8192, 0);
	}
	return b;
}

static void bucket_join(switch_stt_bucket_t *b)
{
	if (b->thread) {
		switch_status_t st;
		switch_thread_join(&st, b->thread);
		b->thread = NULL;
	}
	b->sink = NULL;
}

static void bucket_start(switch_stt_bucket_t *b)
{
	switch_threadattr_t *attr = NULL;

	/* bucket mutex held */
	if (b->running) {
		return;
	}
	/* the previous clock has decided to exit; its exit path takes no lock we hold */
	bucket_join(b);
	b->stop = SWITCH_FALSE;
	b->running = SWITCH_TRUE;
	switch_threadattr_create(&attr, b->pool);
	switch_threadattr_stacksize_set(attr, SWITCH_THREAD_STACKSIZE);
	switch_threadattr_priority_set(attr, SWITCH_PRI_REALTIME);
	if (switch_thread_create(&b->thread, attr, bucket_thread, b, b->pool) != SWITCH_STATUS_SUCCESS) {
		b->running = SWITCH_FALSE;
		b->thread = NULL;
	}
}

SWITCH_DECLARE(switch_bool_t) switch_stt_bucket_enabled(void)
{
	return globals.ready && globals.cfg.enabled;
}

SWITCH_DECLARE(switch_bool_t) switch_stt_bucket_attach_all(void)
{
	return globals.cfg.attach_all;
}

SWITCH_DECLARE(switch_bool_t) switch_stt_bucket_follow_bridge(void)
{
	return globals.cfg.follow_bridge;
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_attach(const char *uuid, switch_stt_bucket_slot_t **slotp, uint32_t *generation)
{
	return switch_stt_bucket_attach_peer(uuid, NULL, slotp, generation);
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_attach_peer(const char *uuid, const char *peer_uuid, switch_stt_bucket_slot_t **slotp, uint32_t *generation)
{
	switch_stt_bucket_config_t cfg;
	switch_stt_bucket_slot_t *found = NULL;
	switch_time_t now = switch_micro_time_now();
	int bi, i;

	*slotp = NULL;
	if (!switch_stt_bucket_enabled() || zstr(uuid)) {
		return SWITCH_STATUS_FALSE;
	}
	switch_stt_bucket_get_config(&cfg);

	switch_mutex_lock(globals.mutex);

	/* pack calls into the lowest buckets: fewer provider connections */
	for (bi = 0; !found && bi <= globals.nbuckets && bi < cfg.max_buckets && bi < STT_BUCKET_MAX_BUCKETS; bi++) {
		switch_stt_bucket_t *b;

		if (bi == globals.nbuckets) {
			globals.buckets[bi] = bucket_create(bi, &cfg);
			globals.nbuckets++;
		}
		b = globals.buckets[bi];

		switch_mutex_lock(b->mutex);
		for (i = 0; i < b->channels; i++) {
			switch_stt_bucket_slot_t *slot = &b->slots[i];

			switch_mutex_lock(slot->mutex);
			if (slot->state == SLOT_FREE || (slot->state == SLOT_QUARANTINE && (now >= slot->free_at || !b->running))) {
				slot->state = SLOT_ACTIVE;
				slot->generation++;
				slot->primed = SWITCH_FALSE;
				slot->underruns = slot->dropped_ms = 0;
				slot->announced_epoch = 0;
				switch_copy_string(slot->uuid, uuid, sizeof(slot->uuid));
				switch_copy_string(slot->peer_uuid, switch_str_nil(peer_uuid), sizeof(slot->peer_uuid));
				switch_buffer_zero(slot->buf);
				*generation = slot->generation;
				found = slot;
			}
			switch_mutex_unlock(slot->mutex);
			if (found) {
				break;
			}
		}
		if (found) {
			bucket_start(b);
		}
		switch_mutex_unlock(b->mutex);
	}

	switch_mutex_unlock(globals.mutex);

	if (!found) {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(uuid), SWITCH_LOG_WARNING, "[stt_bucket] no free channel (%d buckets x %d channels)\n",
						  cfg.max_buckets, cfg.channels);
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(uuid), SWITCH_LOG_INFO, "[stt_bucket] attached to %s channel %d\n", found->bucket->name, found->index);
	*slotp = found;
	return SWITCH_STATUS_SUCCESS;
}

static void slot_reset_decoder(switch_stt_bucket_slot_t *slot)
{
	if (slot->codec_ready) {
		switch_core_codec_destroy(&slot->codec);
		slot->codec_ready = SWITCH_FALSE;
	}
	*slot->codec_key = '\0';
	if (slot->resampler) {
		switch_resample_destroy(&slot->resampler);
	}
	slot->resampler_from = 0;
}

SWITCH_DECLARE(void) switch_stt_bucket_detach(switch_stt_bucket_slot_t *slot, uint32_t generation)
{
	if (!slot) {
		return;
	}
	switch_mutex_lock(slot->mutex);
	if (slot->generation == generation && slot->state == SLOT_ACTIVE) {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(slot->uuid), SWITCH_LOG_INFO,
						  "[stt_bucket] detached from %s channel %d (underruns %" SWITCH_UINT64_T_FMT ", dropped %" SWITCH_UINT64_T_FMT " ms)\n",
						  slot->bucket->name, slot->index, slot->underruns, slot->dropped_ms);
		slot->state = SLOT_DRAINING;
		slot_reset_decoder(slot);
	}
	switch_mutex_unlock(slot->mutex);
}

SWITCH_DECLARE(void) switch_stt_bucket_slot_info(switch_stt_bucket_slot_t *slot, const char **bucket_name, int *channel)
{
	if (bucket_name) *bucket_name = slot ? slot->bucket->name : NULL;
	if (channel) *channel = slot ? slot->index : -1;
}

/* slot mutex held */
static void slot_push(switch_stt_bucket_slot_t *slot, int16_t *pcm, uint32_t samples, uint32_t rate)
{
	switch_stt_bucket_t *b = slot->bucket;
	switch_size_t bytes, max;

	if (!samples) {
		return;
	}

	if (rate != b->rate) {
		if (!slot->resampler || slot->resampler_from != rate) {
			if (slot->resampler) {
				switch_resample_destroy(&slot->resampler);
			}
			if (switch_resample_create(&slot->resampler, rate, b->rate, STT_BUCKET_DEC_BUF, SWITCH_RESAMPLE_QUALITY, 1) != SWITCH_STATUS_SUCCESS) {
				return;
			}
			slot->resampler_from = rate;
		}
		switch_resample_process(slot->resampler, pcm, samples);
		pcm = slot->resampler->to;
		samples = slot->resampler->to_len;
	}

	bytes = samples * sizeof(int16_t);
	max = (switch_size_t)b->rate * globals.cfg.max_buffer_ms / 1000 * sizeof(int16_t);
	if (switch_buffer_inuse(slot->buf) + bytes > max) {
		/* our clock is slower than the call's (or the bucket stalled): drop the oldest audio */
		switch_size_t over = switch_buffer_inuse(slot->buf) + bytes - max;
		switch_buffer_toss(slot->buf, over);
		slot->dropped_ms += (over / sizeof(int16_t)) * 1000 / b->rate;
	}
	switch_buffer_write(slot->buf, pcm, bytes);
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_feed_pcm(switch_stt_bucket_slot_t *slot, uint32_t generation,
														   const int16_t *pcm, uint32_t samples, uint32_t rate)
{
	if (!globals.ready || !slot || !pcm || !rate) {
		return SWITCH_STATUS_FALSE;
	}
	switch_mutex_lock(slot->mutex);
	if (slot->generation != generation || slot->state != SLOT_ACTIVE) {
		switch_mutex_unlock(slot->mutex);
		return SWITCH_STATUS_FALSE;
	}
	if (samples > STT_BUCKET_DEC_BUF) {
		samples = STT_BUCKET_DEC_BUF;
	}
	memcpy(slot->dec, pcm, samples * sizeof(int16_t));
	slot_push(slot, slot->dec, samples, rate);
	switch_mutex_unlock(slot->mutex);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_feed_frame(switch_stt_bucket_slot_t *slot, uint32_t generation,
															 const switch_codec_implementation_t *impl, const char *fmtp, switch_frame_t *frame)
{
	char key[sizeof(slot->codec_key)];
	uint32_t rate, samples, len, channels;
	unsigned int flag = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;

	if (!globals.ready || !slot || !impl || !frame || !impl->iananame) {
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(slot->mutex);

	if (slot->generation != generation || slot->state != SLOT_ACTIVE) {
		goto end;
	}

	channels = impl->number_of_channels ? impl->number_of_channels : 1;

	if ((frame->flags & (SFF_CNG | SFF_PLC)) || !frame->datalen) {
		/* comfort noise / lost packet: keep the channel timeline intact with silence */
		samples = impl->samples_per_packet;
		if (samples > STT_BUCKET_DEC_BUF) samples = STT_BUCKET_DEC_BUF;
		memset(slot->dec, 0, samples * sizeof(int16_t));
		slot_push(slot, slot->dec, samples, impl->actual_samples_per_second);
		status = SWITCH_STATUS_SUCCESS;
		goto end;
	}

	switch_snprintf(key, sizeof(key), "%s/%u/%d/%u", impl->iananame, impl->actual_samples_per_second,
					impl->microseconds_per_packet, channels);

	if (!slot->codec_ready || strcmp(key, slot->codec_key)) {
		slot_reset_decoder(slot);
		/* a private decoder: decoding state of the call's own read codec is never touched */
		if (switch_core_codec_init(&slot->codec, impl->iananame, impl->modname, fmtp, impl->actual_samples_per_second,
								   impl->microseconds_per_packet / 1000, channels,
								   SWITCH_CODEC_FLAG_DECODE, NULL, NULL) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(slot->uuid), SWITCH_LOG_WARNING, "[stt_bucket] cannot decode %s\n", key);
			goto end;
		}
		slot->codec_ready = SWITCH_TRUE;
		switch_copy_string(slot->codec_key, key, sizeof(slot->codec_key));
	}

	len = sizeof(slot->dec);
	rate = impl->actual_samples_per_second;
	if (switch_core_codec_decode(&slot->codec, NULL, frame->data, frame->datalen, impl->actual_samples_per_second,
								 slot->dec, &len, &rate, &flag) != SWITCH_STATUS_SUCCESS) {
		goto end;
	}

	samples = len / sizeof(int16_t) / channels;
	if (channels > 1) {
		/* downmix to mono: one call == one channel of the bucket */
		uint32_t s, c;
		for (s = 0; s < samples; s++) {
			int32_t acc = 0;
			for (c = 0; c < channels; c++) {
				acc += slot->dec[s * channels + c];
			}
			slot->dec[s] = (int16_t)(acc / (int32_t)channels);
		}
	}

	slot_push(slot, slot->dec, samples, rate ? rate : impl->actual_samples_per_second);
	status = SWITCH_STATUS_SUCCESS;

  end:
	switch_mutex_unlock(slot->mutex);
	return status;
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_resolve(const char *stream_id, int channel, uint64_t offset_ms, char *uuid, switch_size_t uuid_len)
{
	int bi, h;
	switch_status_t status = SWITCH_STATUS_NOTFOUND;

	if (!globals.ready || zstr(stream_id) || !uuid || !uuid_len) {
		return SWITCH_STATUS_FALSE;
	}
	*uuid = '\0';

	switch_mutex_lock(globals.mutex);
	for (bi = 0; bi < globals.nbuckets && status != SWITCH_STATUS_SUCCESS; bi++) {
		switch_stt_bucket_t *b = globals.buckets[bi];
		switch_stt_bucket_slot_t *slot;

		if (channel < 0 || channel >= b->channels) {
			continue;
		}
		slot = &b->slots[channel];
		switch_mutex_lock(slot->mutex);
		for (h = 0; h < STT_BUCKET_HISTORY; h++) {
			slot_history_t *e = &slot->history[h];
			if (*e->uuid && !strcmp(e->stream_id, stream_id) && offset_ms >= e->start_ms && offset_ms < e->end_ms) {
				switch_copy_string(uuid, e->uuid, uuid_len);
				status = SWITCH_STATUS_SUCCESS;
				break;
			}
		}
		switch_mutex_unlock(slot->mutex);
	}
	switch_mutex_unlock(globals.mutex);

	return status;
}

SWITCH_DECLARE(void) switch_stt_bucket_status(switch_stream_handle_t *stream)
{
	int bi, i;
	static const char *names[] = { "free", "active", "draining", "quarantine" };
	switch_stt_bucket_config_t cfg;

	if (!globals.ready) {
		stream->write_function(stream, "stt_bucket: not initialized\n");
		return;
	}
	switch_stt_bucket_get_config(&cfg);
	switch_mutex_lock(globals.mutex);
	stream->write_function(stream, "stt_bucket: %s, sink %s, %d ch x %d buckets max, %u Hz\n",
						   cfg.enabled ? "enabled" : "disabled", cfg.sink, cfg.channels, cfg.max_buckets, cfg.rate);
	for (bi = 0; bi < globals.nbuckets; bi++) {
		switch_stt_bucket_t *b = globals.buckets[bi];
		int used = 0;

		for (i = 0; i < b->channels; i++) {
			if (b->slots[i].state != SLOT_FREE) used++;
		}
		stream->write_function(stream, "%s: %s stream=%s %d/%d channels in use, %" SWITCH_UINT64_T_FMT " ms, %" SWITCH_UINT64_T_FMT " bytes\n",
							   b->name, b->running ? "running" : "stopped", b->stream_open ? b->stream_id : "-", used, b->channels,
							   stream_ms(b), b->bytes);
		for (i = 0; i < b->channels; i++) {
			switch_stt_bucket_slot_t *slot = &b->slots[i];
			switch_mutex_lock(slot->mutex);
			if (slot->state != SLOT_FREE) {
				stream->write_function(stream, "  ch %3d %-10s %s buffered=%ums underruns=%" SWITCH_UINT64_T_FMT "\n", i, names[slot->state],
									   *slot->uuid ? slot->uuid : "-",
									   (unsigned)(switch_buffer_inuse(slot->buf) / sizeof(int16_t) * 1000 / b->rate), slot->underruns);
			}
			switch_mutex_unlock(slot->mutex);
		}
	}
	switch_mutex_unlock(globals.mutex);
}

/* ---- sinks registry ---- */

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_register_sink(const switch_stt_bucket_sink_t *sink)
{
	if (!globals.ready || !sink || zstr(sink->name) || !sink->open || !sink->write || !sink->close) {
		return SWITCH_STATUS_FALSE;
	}
	switch_mutex_lock(globals.cfg_mutex);
	switch_core_hash_insert(globals.sinks, sink->name, (void *) sink);
	switch_mutex_unlock(globals.cfg_mutex);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] registered sink [%s]\n", sink->name);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void) switch_stt_bucket_unregister_sink(const char *name)
{
	int bi;

	if (!globals.ready || zstr(name)) {
		return;
	}

	switch_mutex_lock(globals.cfg_mutex);
	switch_core_hash_delete(globals.sinks, name);
	switch_mutex_unlock(globals.cfg_mutex);

	/* a module unloading its sink must not leave a clock thread calling into it:
	   stop those buckets, then restart the ones still carrying calls (they retry until a sink appears) */
	switch_mutex_lock(globals.mutex);
	for (bi = 0; bi < globals.nbuckets; bi++) {
		switch_stt_bucket_t *b = globals.buckets[bi];
		int i, busy = 0;

		if (!b->thread || !b->sink || strcmp(b->sink->name, name)) {
			continue;
		}
		b->stop = SWITCH_TRUE;
		bucket_join(b);

		switch_mutex_lock(b->mutex);
		for (i = 0; i < b->channels; i++) {
			if (b->slots[i].state == SLOT_ACTIVE || b->slots[i].state == SLOT_DRAINING) busy++;
		}
		if (busy) {
			bucket_start(b);
		}
		switch_mutex_unlock(b->mutex);
	}
	switch_mutex_unlock(globals.mutex);
}

/* ---- configuration ---- */

static void config_defaults(switch_stt_bucket_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = SWITCH_FALSE;
	cfg->rate = 16000;
	cfg->channels = 255;
	cfg->max_buckets = 4;
	cfg->ptime = 20;
	cfg->bitrate_per_channel = 16000;
	cfg->page_ms = 250;
	cfg->quarantine_ms = 2000;
	cfg->idle_timeout_ms = 5000;
	cfg->max_buffer_ms = 500;
	cfg->attach_all = SWITCH_FALSE;
	cfg->follow_bridge = SWITCH_TRUE;
	switch_copy_string(cfg->sink, "file", sizeof(cfg->sink));
}

static void config_sanitize(switch_stt_bucket_config_t *cfg)
{
	if (cfg->rate != 8000 && cfg->rate != 12000 && cfg->rate != 16000 && cfg->rate != 24000 && cfg->rate != 48000) {
		cfg->rate = 16000;
	}
	if (cfg->channels < 1) cfg->channels = 1;
	if (cfg->channels > SWITCH_STT_BUCKET_MAX_CHANNELS) cfg->channels = SWITCH_STT_BUCKET_MAX_CHANNELS;
	if (cfg->max_buckets < 1) cfg->max_buckets = 1;
	if (cfg->max_buckets > STT_BUCKET_MAX_BUCKETS) cfg->max_buckets = STT_BUCKET_MAX_BUCKETS;
	if (cfg->ptime != 10 && cfg->ptime != 20 && cfg->ptime != 40 && cfg->ptime != 60) cfg->ptime = 20;
	if (cfg->bitrate_per_channel < 6000) cfg->bitrate_per_channel = 6000;
	if (cfg->page_ms < cfg->ptime) cfg->page_ms = cfg->ptime;
	if (cfg->quarantine_ms < 0) cfg->quarantine_ms = 0;
	if (cfg->idle_timeout_ms < cfg->ptime) cfg->idle_timeout_ms = cfg->ptime;
	if (cfg->max_buffer_ms < 4 * cfg->ptime) cfg->max_buffer_ms = 4 * cfg->ptime;
	if (zstr(cfg->sink)) switch_copy_string(cfg->sink, "file", sizeof(cfg->sink));
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_configure(const switch_stt_bucket_config_t *cfg)
{
	switch_stt_bucket_config_t c = *cfg;

	if (!globals.ready) {
		return SWITCH_STATUS_FALSE;
	}
	config_sanitize(&c);
	switch_mutex_lock(globals.cfg_mutex);
	if (globals.nbuckets && (c.rate != globals.cfg.rate)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[stt_bucket] rate change applies to new buckets only\n");
	}
	globals.cfg = c;
	switch_mutex_unlock(globals.cfg_mutex);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void) switch_stt_bucket_get_config(switch_stt_bucket_config_t *cfg)
{
	if (!globals.cfg_mutex) {
		config_defaults(cfg);
		return;
	}
	switch_mutex_lock(globals.cfg_mutex);
	*cfg = globals.cfg;
	switch_mutex_unlock(globals.cfg_mutex);
}

static void config_load(switch_stt_bucket_config_t *cfg)
{
	switch_xml_t xml, x_cfg, settings, param;

	if (!(xml = switch_xml_open_cfg("stt_bucket.conf", &x_cfg, NULL))) {
		return;
	}
	if ((settings = switch_xml_child(x_cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");

			if (!strcasecmp(var, "enabled")) cfg->enabled = switch_true(val);
			else if (!strcasecmp(var, "rate")) cfg->rate = (uint32_t)atoi(val);
			else if (!strcasecmp(var, "channels")) cfg->channels = atoi(val);
			else if (!strcasecmp(var, "max-buckets")) cfg->max_buckets = atoi(val);
			else if (!strcasecmp(var, "ptime")) cfg->ptime = atoi(val);
			else if (!strcasecmp(var, "bitrate-per-channel")) cfg->bitrate_per_channel = atoi(val);
			else if (!strcasecmp(var, "page-ms")) cfg->page_ms = atoi(val);
			else if (!strcasecmp(var, "quarantine-ms")) cfg->quarantine_ms = atoi(val);
			else if (!strcasecmp(var, "idle-timeout-ms")) cfg->idle_timeout_ms = atoi(val);
			else if (!strcasecmp(var, "max-buffer-ms")) cfg->max_buffer_ms = atoi(val);
			else if (!strcasecmp(var, "attach-all")) cfg->attach_all = switch_true(val);
			else if (!strcasecmp(var, "bridge-legs")) cfg->follow_bridge = switch_true(val);
			else if (!strcasecmp(var, "dtx")) cfg->dtx = switch_true(val);
			else if (!strcasecmp(var, "sink")) switch_copy_string(cfg->sink, val, sizeof(cfg->sink));
			else if (!strcasecmp(var, "file-path")) switch_copy_string(cfg->file_path, val, sizeof(cfg->file_path));
			else if (!strcasecmp(var, "tcp-target")) switch_copy_string(cfg->tcp_target, val, sizeof(cfg->tcp_target));
			else switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "[stt_bucket] unknown param [%s]\n", var);
		}
	}
	switch_xml_free(xml);
}

SWITCH_DECLARE(switch_status_t) switch_stt_bucket_init(switch_memory_pool_t *pool)
{
	switch_stt_bucket_config_t cfg;
	const char *core_uuid;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&globals.cfg_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&globals.sinks);

	core_uuid = switch_core_get_uuid();
	switch_copy_string(globals.node, zstr(core_uuid) ? "fs" : core_uuid, 9);

	switch_event_reserve_subclass(SWITCH_STT_BUCKET_EVENT_ATTACH);
	switch_event_reserve_subclass(SWITCH_STT_BUCKET_EVENT_DETACH);
	switch_event_reserve_subclass(SWITCH_STT_BUCKET_EVENT_STREAM);

	config_defaults(&cfg);
	config_load(&cfg);
	config_sanitize(&cfg);
	globals.cfg = cfg;
	globals.ready = SWITCH_TRUE;

	switch_stt_bucket_register_sink(&file_sink);
	switch_stt_bucket_register_sink(&tcp_sink);

#ifndef HAVE_LIBOPUSENC
	if (cfg.enabled) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "[stt_bucket] enabled in config but FreeSWITCH was built without libopusenc\n");
		globals.cfg.enabled = SWITCH_FALSE;
	}
#endif

	if (globals.cfg.enabled) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "[stt_bucket] enabled: %d ch/bucket, %d buckets max, %u Hz, sink [%s]\n",
						  globals.cfg.channels, globals.cfg.max_buckets, globals.cfg.rate, globals.cfg.sink);
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_DECLARE(void) switch_stt_bucket_shutdown(void)
{
	int bi, i;

	if (!globals.ready) {
		return;
	}

	switch_mutex_lock(globals.mutex);
	for (bi = 0; bi < globals.nbuckets; bi++) {
		switch_stt_bucket_t *b = globals.buckets[bi];

		b->stop = SWITCH_TRUE;
		bucket_join(b);
		/* codec modules unload right after us: drop the private decoders now.
		   Slots and their buffers stay allocated, late detach() calls from dying sessions are harmless. */
		for (i = 0; i < b->channels; i++) {
			switch_mutex_lock(b->slots[i].mutex);
			slot_reset_decoder(&b->slots[i]);
			if (b->slots[i].state == SLOT_ACTIVE) {
				b->slots[i].state = SLOT_DRAINING;
			}
			switch_mutex_unlock(b->slots[i].mutex);
		}
	}
	globals.nbuckets = 0;
	globals.ready = SWITCH_FALSE;
	switch_mutex_unlock(globals.mutex);

	switch_mutex_lock(globals.cfg_mutex);
	switch_core_hash_destroy(&globals.sinks);
	switch_mutex_unlock(globals.cfg_mutex);

	switch_event_free_subclass(SWITCH_STT_BUCKET_EVENT_ATTACH);
	switch_event_free_subclass(SWITCH_STT_BUCKET_EVENT_DETACH);
	switch_event_free_subclass(SWITCH_STT_BUCKET_EVENT_STREAM);
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
