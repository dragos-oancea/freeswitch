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
 * stt_bucket_test_util.h -- helpers shared by the STT bucket tests:
 *   - an in-memory sink that keeps every stream it is handed (optionally failing on purpose)
 *   - a decoder for the multichannel Ogg/Opus stream (mapping family 255) using libopus directly
 *     (libopusfile refuses family 255; ffmpeg mixes channels when DTX is on)
 *   - tone detection per channel
 */
#ifndef STT_BUCKET_TEST_UTIL_H
#define STT_BUCKET_TEST_UTIL_H

#include <switch.h>
#include <math.h>
#include <opus_multistream.h>

#define MOCK_MAX_STREAMS 16

typedef struct {
	char id[128];
	int channels;
	int rate;
	switch_buffer_t *data;
	int closed;
} mock_stream_t;

static struct {
	switch_mutex_t *mutex;
	mock_stream_t streams[MOCK_MAX_STREAMS];
	int nstreams;
	int writes;
	int fail_after_writes; /* 0 = never fail; N = the N-th write of each stream fails */
	int writes_this_stream;
} mock;

static switch_status_t mock_open(switch_stt_bucket_t *b, const char *stream_id, int channels, int rate, void **sink_data)
{
	mock_stream_t *s;

	switch_mutex_lock(mock.mutex);
	if (mock.nstreams >= MOCK_MAX_STREAMS) {
		switch_mutex_unlock(mock.mutex);
		return SWITCH_STATUS_FALSE;
	}
	s = &mock.streams[mock.nstreams++];
	switch_copy_string(s->id, stream_id, sizeof(s->id));
	s->channels = channels;
	s->rate = rate;
	switch_buffer_create_dynamic(&s->data, 64 * 1024, 64 * 1024, 0);
	mock.writes_this_stream = 0;
	switch_mutex_unlock(mock.mutex);
	*sink_data = s;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mock_write(switch_stt_bucket_t *b, void *sink_data, const uint8_t *page, switch_size_t len)
{
	mock_stream_t *s = sink_data;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_mutex_lock(mock.mutex);
	mock.writes++;
	if (mock.fail_after_writes && ++mock.writes_this_stream >= mock.fail_after_writes) {
		status = SWITCH_STATUS_FALSE; /* like a provider dropping the websocket */
	} else {
		switch_buffer_write(s->data, page, len);
	}
	switch_mutex_unlock(mock.mutex);
	return status;
}

static void mock_close(switch_stt_bucket_t *b, void *sink_data)
{
	mock_stream_t *s = sink_data;

	switch_mutex_lock(mock.mutex);
	s->closed = 1;
	switch_mutex_unlock(mock.mutex);
}

static const switch_stt_bucket_sink_t mock_sink = { "mock", mock_open, mock_write, mock_close };

static inline int mock_all_closed(void)
{
	int i, all = 1;

	if (!mock.mutex) return 1;
	switch_mutex_lock(mock.mutex);
	for (i = 0; i < mock.nstreams; i++) all &= mock.streams[i].closed;
	switch_mutex_unlock(mock.mutex);
	return all;
}

/* wait until no stream is open and none reopens for `quiet_ms` (longer than the bucket idle timeout):
   only then is it safe to throw the recorded streams away */
static inline int mock_wait_quiet(int quiet_ms, int timeout_ms)
{
	switch_time_t deadline = switch_micro_time_now() + (switch_time_t)timeout_ms * 1000;

	while (switch_micro_time_now() < deadline) {
		int n;
		if (!mock_all_closed()) {
			switch_sleep(10000);
			continue;
		}
		n = mock.nstreams;
		switch_sleep((switch_interval_time_t)quiet_ms * 1000);
		if (mock_all_closed() && n == mock.nstreams) return 1;
	}
	return 0;
}

static inline void mock_reset(void)
{
	static switch_memory_pool_t *pool = NULL;
	int i;

	if (!mock.mutex) {
		switch_core_new_memory_pool(&pool);
		switch_mutex_init(&mock.mutex, SWITCH_MUTEX_NESTED, pool);
	}
	mock_wait_quiet(700, 5000);
	switch_mutex_lock(mock.mutex);
	for (i = 0; i < mock.nstreams; i++) {
		if (mock.streams[i].data) switch_buffer_destroy(&mock.streams[i].data);
	}
	memset(mock.streams, 0, sizeof(mock.streams));
	mock.nstreams = mock.writes = mock.fail_after_writes = mock.writes_this_stream = 0;
	switch_mutex_unlock(mock.mutex);
}

static inline mock_stream_t *mock_find(const char *id)
{
	int i;
	for (i = 0; i < mock.nstreams; i++) {
		if (!strcmp(mock.streams[i].id, id)) return &mock.streams[i];
	}
	return NULL;
}

/* ---- Ogg/Opus decoding ---- */

typedef struct {
	int channels;
	long frames;     /* per channel, at 48 kHz */
	int16_t *pcm;    /* interleaved */
} decoded_t;

/* Decode a complete Ogg/Opus byte stream. Returns 0 on success. */
static inline int decode_ogg_opus(const uint8_t *data, switch_size_t len, decoded_t *out)
{
	switch_size_t pos = 0;
	uint8_t packet[65536];
	switch_size_t plen = 0;
	int npackets = 0, err = 0;
	OpusMSDecoder *dec = NULL;
	long cap = 48000 * 10;

	memset(out, 0, sizeof(*out));

	while (pos + 27 <= len) {
		int nseg, i;
		switch_size_t body;

		if (memcmp(data + pos, "OggS", 4)) return -1;
		nseg = data[pos + 26];
		body = pos + 27 + nseg;
		for (i = 0; i < nseg; i++) {
			int l = data[pos + 27 + i];
			if (body + l > len || plen + l > sizeof(packet)) return -1;
			memcpy(packet + plen, data + body, l);
			plen += l;
			body += l;
			if (l < 255) {
				/* a complete packet */
				if (npackets == 0) {
					unsigned char mapping[255];
					int streams, coupled;
					if (plen < 19 || memcmp(packet, "OpusHead", 8)) return -1;
					out->channels = packet[9];
					if (packet[18] == 0) {
						streams = 1; coupled = out->channels == 2; mapping[0] = 0; mapping[1] = 1;
					} else {
						streams = packet[19]; coupled = packet[20];
						memcpy(mapping, packet + 21, out->channels);
					}
					dec = opus_multistream_decoder_create(48000, out->channels, streams, coupled, mapping, &err);
					if (!dec) return -1;
					out->pcm = calloc((size_t)cap * out->channels, sizeof(int16_t));
				} else if (npackets > 1) {
					int n;
					if (out->frames + 5760 > cap) {
						cap *= 2;
						out->pcm = realloc(out->pcm, (size_t)cap * out->channels * sizeof(int16_t));
					}
					n = opus_multistream_decode(dec, packet, (opus_int32)plen, out->pcm + out->frames * out->channels, 5760, 0);
					if (n > 0) out->frames += n;
				}
				npackets++;
				plen = 0;
			}
		}
		pos = body;
	}
	if (dec) opus_multistream_decoder_destroy(dec);
	return out->channels ? 0 : -1;
}

static inline void decoded_free(decoded_t *d)
{
	switch_safe_free(d->pcm);
}

/* power of `freq` on channel `ch` over frames [start, start+n) (Goertzel) */
static inline double tone_power(const decoded_t *d, int ch, long start, long n, double freq)
{
	double w = 2 * M_PI * freq / 48000.0, c = 2 * cos(w), s1 = 0, s2 = 0, s0;
	long i;

	if (start < 0) start = 0;
	if (start + n > d->frames) n = d->frames - start;
	for (i = 0; i < n; i++) {
		s0 = d->pcm[(start + i) * d->channels + ch] + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return n > 0 ? (s1 * s1 + s2 * s2 - c * s1 * s2) / n : 0;
}

static inline double channel_rms(const decoded_t *d, int ch, long start, long n)
{
	double acc = 0;
	long i;

	if (start < 0) start = 0;
	if (start + n > d->frames) n = d->frames - start;
	for (i = 0; i < n; i++) {
		double v = d->pcm[(start + i) * d->channels + ch];
		acc += v * v;
	}
	return n > 0 ? sqrt(acc / n) : 0;
}

/* which of `cands` dominates channel `ch` over [start, start+n); 0 if the channel is silent */
static inline double dominant_tone(const decoded_t *d, int ch, long start, long n, const double *cands, int ncands)
{
	double best = 0, bf = 0;
	int i;

	if (channel_rms(d, ch, start, n) < 300) return 0;
	for (i = 0; i < ncands; i++) {
		double p = tone_power(d, ch, start, n, cands[i]);
		if (p > best) {
			best = p;
			bf = cands[i];
		}
	}
	return bf;
}

#endif
