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
 * switch_stt_bucket.h -- multiplex the inbound audio of many calls into the
 * channels of one Ogg/Opus stream (channel mapping family 255) so that a whole
 * "bucket" of calls shares a single long-lived connection to an STT provider.
 *
 * Audio is tapped directly from the RTP read path in switch_core_media.c
 * (no media bug). Each call owns one channel index in a bucket for its
 * lifetime; the mapping channel index <-> call UUID is published with events
 * and can be resolved later with switch_stt_bucket_resolve().
 */

#ifndef SWITCH_STT_BUCKET_H
#define SWITCH_STT_BUCKET_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

#define SWITCH_STT_BUCKET_MAX_CHANNELS 255
#define SWITCH_STT_BUCKET_EVENT_ATTACH "stt_bucket::attach"
#define SWITCH_STT_BUCKET_EVENT_DETACH "stt_bucket::detach"
#define SWITCH_STT_BUCKET_EVENT_STREAM "stt_bucket::stream"

typedef struct switch_stt_bucket_s switch_stt_bucket_t;
typedef struct switch_stt_bucket_slot_s switch_stt_bucket_slot_t;

/*! A sink receives the Ogg pages of one bucket stream, e.g. a websocket to the STT provider.
    All callbacks run on the bucket's clock thread: write() must not block for long. */
typedef struct switch_stt_bucket_sink_s {
	const char *name;
	/*! a new Ogg stream starts (headers follow in write()); *sink_data is private to the sink */
	switch_status_t (*open)(switch_stt_bucket_t *bucket, const char *stream_id, int channels, int rate, void **sink_data);
	/*! one complete Ogg page; returning != SUCCESS closes the sink and restarts the stream (new headers, new stream id) */
	switch_status_t (*write)(switch_stt_bucket_t *bucket, void *sink_data, const uint8_t *page, switch_size_t len);
	/*! end of stream (EOS page already written) */
	void (*close)(switch_stt_bucket_t *bucket, void *sink_data);
} switch_stt_bucket_sink_t;

typedef struct switch_stt_bucket_config_s {
	switch_bool_t enabled;
	/*! sample rate of the multiplexed stream: 8000, 12000, 16000, 24000 or 48000 */
	uint32_t rate;
	/*! channels (= concurrent calls) per bucket, 1..255 */
	int channels;
	/*! upper bound of buckets (= provider connections) */
	int max_buckets;
	/*! clock of the mixer, ms (10, 20, 40 or 60) */
	int ptime;
	/*! Opus bitrate per channel, bps */
	int bitrate_per_channel;
	/*! how often an Ogg page is forced out, ms (latency vs. overhead) */
	int page_ms;
	/*! a freed channel stays silent this long before another call may use it */
	int quarantine_ms;
	/*! close the provider stream when a bucket has had no call for this long */
	int idle_timeout_ms;
	/*! cap of buffered audio per call, ms (older audio is dropped on overflow) */
	int max_buffer_ms;
	/*! Opus DTX on idle channels (smaller stream; breaks ffmpeg's family-255 decoder) */
	switch_bool_t dtx;
	/*! attach every call, not only those with stt_bucket=true */
	switch_bool_t attach_all;
	/*! built-in sink: "file", "tcp", or the name of a sink registered by a module */
	char sink[64];
	/*! file sink: path template, %s is replaced by the stream id */
	char file_path[512];
	/*! tcp sink: host:port */
	char tcp_target[256];
} switch_stt_bucket_config_t;

/*! load stt_bucket.conf (called from switch_core_init) */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_init(switch_memory_pool_t *pool);
/*! (re)configure explicitly; takes effect for streams opened afterwards */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_configure(const switch_stt_bucket_config_t *cfg);
SWITCH_DECLARE(void) switch_stt_bucket_get_config(switch_stt_bucket_config_t *cfg);
SWITCH_DECLARE(void) switch_stt_bucket_shutdown(void);

/*! sinks provided by modules (e.g. a websocket client for a given provider) */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_register_sink(const switch_stt_bucket_sink_t *sink);
SWITCH_DECLARE(void) switch_stt_bucket_unregister_sink(const char *name);

/*! claim a channel for a call; fires SWITCH_STT_BUCKET_EVENT_ATTACH */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_attach(const char *uuid, switch_stt_bucket_slot_t **slot, uint32_t *generation);
/*! release it; buffered audio is still flushed; fires SWITCH_STT_BUCKET_EVENT_DETACH */
SWITCH_DECLARE(void) switch_stt_bucket_detach(switch_stt_bucket_slot_t *slot, uint32_t generation);

/*! feed one encoded RTP payload; it is decoded with a private decoder matching impl (the session codec is not touched) */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_feed_frame(switch_stt_bucket_slot_t *slot, uint32_t generation,
															 const switch_codec_implementation_t *impl, const char *fmtp, switch_frame_t *frame);
/*! feed signed linear mono audio at any rate */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_feed_pcm(switch_stt_bucket_slot_t *slot, uint32_t generation,
														   const int16_t *pcm, uint32_t samples, uint32_t rate);

/*! bucket name ("b0", "b1"...) and channel index of a slot */
SWITCH_DECLARE(void) switch_stt_bucket_slot_info(switch_stt_bucket_slot_t *slot, const char **bucket_name, int *channel);
SWITCH_DECLARE(switch_bool_t) switch_stt_bucket_enabled(void);
SWITCH_DECLARE(switch_bool_t) switch_stt_bucket_attach_all(void);

/*! which call was on channel `channel` of stream `stream_id` at `offset_ms` (stream time)? */
SWITCH_DECLARE(switch_status_t) switch_stt_bucket_resolve(const char *stream_id, int channel, uint64_t offset_ms, char *uuid, switch_size_t uuid_len);
SWITCH_DECLARE(void) switch_stt_bucket_status(switch_stream_handle_t *stream);

SWITCH_END_EXTERN_C
#endif
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
