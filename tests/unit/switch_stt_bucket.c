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
 * switch_stt_bucket.c -- calls multiplexed into one multichannel Ogg/Opus stream
 *
 * Fake calls feed tones through the same entry points the RTP read path uses
 * (PCMU and OPUS payloads through private decoders, plus raw PCM). The stream
 * goes to the file sink; tests/unit/stt_bucket_verify.py checks per-channel content.
 */
#include <switch.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

#include <test/switch_test.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include "stt_bucket_test_util.h"

typedef struct {
	const char *uuid;
	double freq;
	double phase;
	switch_stt_bucket_slot_t *slot;
	uint32_t gen;
	int channel;
} fake_call_t;

static char stream_ids[4][128];
static int stream_opens = 0, attach_events = 0, detach_events = 0;

static void stt_event_cb(switch_event_t *event)
{
	const char *sub = switch_event_get_header(event, "Event-Subclass");
	const char *action = switch_event_get_header(event, "STT-Action");

	if (!sub) return;
	if (!strcmp(sub, SWITCH_STT_BUCKET_EVENT_STREAM) && action && !strcmp(action, "open") && stream_opens < 4) {
		switch_copy_string(stream_ids[stream_opens++], switch_event_get_header(event, "STT-Stream-ID"), sizeof(stream_ids[0]));
	} else if (!strcmp(sub, SWITCH_STT_BUCKET_EVENT_ATTACH)) {
		attach_events++;
	} else if (!strcmp(sub, SWITCH_STT_BUCKET_EVENT_DETACH)) {
		detach_events++;
	}
}

static void tone(fake_call_t *c, int16_t *pcm, int samples, int rate)
{
	int i;
	for (i = 0; i < samples; i++) {
		pcm[i] = (int16_t)(8000 * sin(c->phase));
		c->phase += 2 * M_PI * c->freq / rate;
	}
}

static void feed_codec(fake_call_t *c, switch_codec_t *codec)
{
	int16_t pcm[960];
	uint8_t enc[1500];
	uint32_t enc_len = sizeof(enc), enc_rate = 0;
	unsigned int flag = 0;
	int rate = codec->implementation->actual_samples_per_second;
	int samples = codec->implementation->samples_per_packet;
	switch_frame_t frame = { 0 };

	tone(c, pcm, samples, rate);
	switch_core_codec_encode(codec, NULL, pcm, samples * 2, rate, enc, &enc_len, &enc_rate, &flag);
	frame.data = enc;
	frame.datalen = enc_len;
	switch_stt_bucket_feed_frame(c->slot, c->gen, codec->implementation, NULL, &frame);
}

static void feed_pcm(fake_call_t *c)
{
	int16_t pcm[320];
	tone(c, pcm, 320, 16000);
	switch_stt_bucket_feed_pcm(c->slot, c->gen, pcm, 320, 16000);
}

static switch_status_t attach_call(fake_call_t *c)
{
	switch_status_t status = switch_stt_bucket_attach(c->uuid, &c->slot, &c->gen);
	switch_stt_bucket_slot_info(c->slot, NULL, &c->channel);
	return status;
}

#define attach(_c) fst_requires(attach_call(_c) == SWITCH_STATUS_SUCCESS)

static void use_sink(const char *name, int max_buckets)
{
	switch_stt_bucket_config_t cfg;
	switch_stt_bucket_get_config(&cfg);
	switch_copy_string(cfg.sink, name, sizeof(cfg.sink));
	if (max_buckets) cfg.max_buckets = max_buckets;
	switch_stt_bucket_configure(&cfg);
}

/* feed `ms` of a tone to every call in `calls`, paced in real time */
static void feed_ms(fake_call_t **calls, int n, int ms)
{
	int t, i;
	for (t = 0; t < ms; t += 20) {
		for (i = 0; i < n; i++) feed_pcm(calls[i]);
		switch_sleep(20000);
	}
}

/* a TCP listener standing in for a provider: accepts one connection and keeps what it receives */
typedef struct {
	int lfd;
	int port;
	uint8_t *buf;
	size_t len, cap;
	volatile int done;
} tcp_rx_t;

static void *tcp_rx_thread(void *obj)
{
	tcp_rx_t *rx = obj;
	int fd = accept(rx->lfd, NULL, NULL);
	ssize_t n;

	if (fd >= 0) {
		for (;;) {
			if (rx->len + 4096 > rx->cap) {
				rx->cap = rx->cap ? rx->cap * 2 : 65536;
				rx->buf = realloc(rx->buf, rx->cap);
			}
			if ((n = read(fd, rx->buf + rx->len, 4096)) <= 0) break;
			rx->len += (size_t)n;
		}
		close(fd);
	}
	rx->done = 1;
	return NULL;
}

static int tcp_rx_start(tcp_rx_t *rx, pthread_t *th)
{
	struct sockaddr_in sa = { 0 };
	socklen_t sl = sizeof(sa);

	memset(rx, 0, sizeof(*rx));
	if ((rx->lfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) return -1;
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0; /* any free port */
	if (bind(rx->lfd, (struct sockaddr *)&sa, sizeof(sa)) || listen(rx->lfd, 1) || getsockname(rx->lfd, (struct sockaddr *)&sa, &sl)) return -1;
	rx->port = ntohs(sa.sin_port);
	return pthread_create(th, NULL, tcp_rx_thread, rx);
}

FST_CORE_BEGIN("./conf_stt_bucket")
{
	FST_SUITE_BEGIN(switch_stt_bucket)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_opus");
			switch_event_bind("stt_bucket_test", SWITCH_EVENT_CUSTOM, SWITCH_EVENT_SUBCLASS_ANY, stt_event_cb, NULL);
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_event_unbind_callback(stt_event_cb);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(multiplex_calls_into_channels)
		{
			switch_codec_t pcmu = { 0 }, opus = { 0 };
			fake_call_t A = { "aaaaaaaa-0000-0000-0000-000000000001", 440 };
			fake_call_t B = { "bbbbbbbb-0000-0000-0000-000000000002", 880 };
			fake_call_t C = { "cccccccc-0000-0000-0000-000000000003", 1320 };
			fake_call_t D = { "dddddddd-0000-0000-0000-000000000004", 1760 };
			fake_call_t E = { "eeeeeeee-0000-0000-0000-000000000005", 660 };
			char uuid[64];
			switch_stream_handle_t stream = { 0 };
			int t;

			fst_requires(switch_stt_bucket_enabled());
			fst_requires(switch_core_codec_init(&pcmu, "PCMU", NULL, NULL, 8000, 20, 1,
							SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, fst_pool) == SWITCH_STATUS_SUCCESS);
			fst_requires(switch_core_codec_init(&opus, "OPUS", "mod_opus", NULL, 48000, 20, 1,
							SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, fst_pool) == SWITCH_STATUS_SUCCESS);

			attach(&A);
			attach(&B);
			attach(&C);
			fst_check_int_equals(A.channel, 0);
			fst_check_int_equals(B.channel, 1);
			fst_check_int_equals(C.channel, 2);

			/* 1 s: A = PCMU RTP, B = OPUS RTP, C = raw PCM, paced like real calls */
			for (t = 0; t < 1000; t += 20) {
				feed_codec(&A, &pcmu);
				feed_codec(&B, &opus);
				feed_pcm(&C);
				switch_sleep(20000);
			}

			/* C hangs up, D arrives at once: channel 2 is quarantined, D must get channel 3 */
			switch_stt_bucket_detach(C.slot, C.gen);
			attach(&D);
			fst_check_int_equals(D.channel, 3);

			/* a late frame for the departed call must be rejected */
			fst_check(switch_stt_bucket_feed_pcm(C.slot, C.gen, (int16_t[320]){0}, 320, 16000) != SWITCH_STATUS_SUCCESS);

			for (t = 0; t < 600; t += 20) {
				feed_codec(&A, &pcmu);
				feed_codec(&B, &opus);
				feed_pcm(&D);
				switch_sleep(20000);
			}

			{
				/* pages must reach the sink while calls are live, not at the end */
				char path[256];
				struct stat st = { 0 };
				long size;
				switch_snprintf(path, sizeof(path), "/tmp/stt_bucket_test_%s.opus", stream_ids[0]);
				stat(path, &st);
				size = (long)st.st_size;
				printf("live stream size after 1.6 s: %ld bytes\n", size);
				fst_check(size > 10000);
			}

			SWITCH_STANDARD_STREAM(stream);
			switch_stt_bucket_status(&stream);
			printf("%s", (char *)stream.data);
			switch_safe_free(stream.data);

			switch_stt_bucket_detach(A.slot, A.gen);
			switch_stt_bucket_detach(B.slot, B.gen);
			switch_stt_bucket_detach(D.slot, D.gen);

			/* drain + idle timeout -> stream closed */
			switch_sleep(1500000);

			fst_requires(stream_opens == 1);
			printf("stream: %s\n", stream_ids[0]);

			/* route "provider results" back to calls by (stream, channel, time) */
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 0, 800, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(uuid, A.uuid);
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 1, 1500, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(uuid, B.uuid);
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 2, 500, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(uuid, C.uuid);
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 2, 1400, uuid, sizeof(uuid)) == SWITCH_STATUS_NOTFOUND);
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 3, 1300, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(uuid, D.uuid);
			fst_check(switch_stt_bucket_resolve(stream_ids[0], 3, 300, uuid, sizeof(uuid)) == SWITCH_STATUS_NOTFOUND);

			fst_check_int_equals(attach_events, 4);
			fst_check_int_equals(detach_events, 4);

			/* the next call opens a fresh stream (new headers, new id) and the lowest channel */
			attach(&E);
			fst_check_int_equals(E.channel, 0);
			for (t = 0; t < 300; t += 20) {
				feed_pcm(&E);
				switch_sleep(20000);
			}
			switch_stt_bucket_detach(E.slot, E.gen);
			switch_sleep(1200000);
			fst_check(stream_opens == 2);
			fst_check(strcmp(stream_ids[0], stream_ids[1]) != 0);
			fst_check(switch_stt_bucket_resolve(stream_ids[1], 0, 100, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
			fst_check_string_equals(uuid, E.uuid);

			switch_core_codec_destroy(&pcmu);
			switch_core_codec_destroy(&opus);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(sink_failure_restarts_stream)
		{
			/* a sink failing mid-stream (provider drops the socket) must not lose the call:
			   the bucket starts a new stream (fresh headers, new id) and the call carries on in it */
			fake_call_t F = { "ffffffff-0000-0000-0000-000000000006", 1000 };
			fake_call_t *calls[1] = { &F };
			char uuid[64];
			decoded_t d;
			int i, ok_streams = 0, n_with_call = 0;

			mock_reset();
			fst_requires(switch_stt_bucket_register_sink(&mock_sink) == SWITCH_STATUS_SUCCESS);
			use_sink("mock", 1);
			switch_mutex_lock(mock.mutex);
			mock.fail_after_writes = 6; /* every stream dies on its 6th page */
			switch_mutex_unlock(mock.mutex);

			attach(&F);
			feed_ms(calls, 1, 2000);
			switch_stt_bucket_detach(F.slot, F.gen);
			/* the call has left: stop failing, so later streams are ordinary idle ones */
			switch_mutex_lock(mock.mutex);
			mock.fail_after_writes = 0;
			n_with_call = mock.nstreams;
			switch_mutex_unlock(mock.mutex);
			fst_check(mock_wait_quiet(700, 5000));

			printf("sink failures produced %d streams, %d while the call was there\n", mock.nstreams, n_with_call);
			fst_check(n_with_call >= 2);
			for (i = 0; i < mock.nstreams; i++) {
				mock_stream_t *ms = &mock.streams[i];
				if (i) fst_check(strcmp(ms->id, mock.streams[i - 1].id) != 0);
				/* every stream starts with its own Ogg/Opus headers, so a receiver can decode it alone */
				if (switch_buffer_inuse(ms->data) && !decode_ogg_opus(switch_buffer_get_head_pointer(ms->data), switch_buffer_inuse(ms->data), &d)) {
					ok_streams++;
					fst_check_int_equals(d.channels, 4);
					decoded_free(&d);
				}
				/* the call is announced in every stream opened while it was there, from that stream's start
				   (the last one may hold only the call's final few ms, so check offset 0) */
				if (i < n_with_call) {
					fst_check(switch_stt_bucket_resolve(ms->id, F.channel, 0, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
					fst_check_string_equals(uuid, F.uuid);
				}
			}
			fst_check_int_equals(ok_streams, mock.nstreams);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(unregister_sink_while_streaming)
		{
			/* a module unloading its sink mid-call: no crash, and the bucket resumes once a sink is back */
			fake_call_t G = { "11111111-0000-0000-0000-000000000007", 1500 };
			fake_call_t *calls[1] = { &G };
			int before;

			mock_reset();
			fst_requires(switch_stt_bucket_register_sink(&mock_sink) == SWITCH_STATUS_SUCCESS);
			use_sink("mock", 1);

			attach(&G);
			feed_ms(calls, 1, 500);
			fst_check_int_equals(mock.nstreams, 1);

			switch_stt_bucket_unregister_sink("mock");
			fst_check(mock.streams[0].closed);
			before = mock.nstreams;
			feed_ms(calls, 1, 500); /* the call keeps feeding while there is no sink */
			fst_check_int_equals(mock.nstreams, before);

			fst_requires(switch_stt_bucket_register_sink(&mock_sink) == SWITCH_STATUS_SUCCESS);
			feed_ms(calls, 1, 1500); /* the bucket retries about once a second */
			fst_check(mock.nstreams > before);

			switch_stt_bucket_detach(G.slot, G.gen);
			fst_wait_until(mock.streams[mock.nstreams - 1].closed, 3000);
			fst_check(mock.streams[mock.nstreams - 1].closed);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(tcp_sink_delivers_ogg)
		{
			/* the built-in tcp sink: one connection per stream, carrying a decodable Ogg/Opus stream */
			fake_call_t H = { "22222222-0000-0000-0000-000000000008", 1100 };
			fake_call_t *calls[1] = { &H };
			switch_stt_bucket_config_t cfg;
			tcp_rx_t rx;
			pthread_t th;
			decoded_t d;
			double got;
			const double cands[] = { 500, 1100, 1700 };

			fst_requires(tcp_rx_start(&rx, &th) == 0);
			switch_stt_bucket_get_config(&cfg);
			switch_snprintf(cfg.tcp_target, sizeof(cfg.tcp_target), "127.0.0.1:%d", rx.port);
			switch_copy_string(cfg.sink, "tcp", sizeof(cfg.sink));
			switch_stt_bucket_configure(&cfg);

			attach(&H);
			feed_ms(calls, 1, 1500);
			switch_stt_bucket_detach(H.slot, H.gen);

			/* closing the stream shuts the socket down, so the listener sees EOF */
			fst_wait_until(rx.done, 5000);
			fst_requires(rx.done);
			pthread_join(th, NULL);
			close(rx.lfd);
			printf("tcp sink delivered %zu bytes\n", rx.len);

			fst_requires(rx.len > 4 && !memcmp(rx.buf, "OggS", 4));
			fst_requires(decode_ogg_opus(rx.buf, rx.len, &d) == 0);
			fst_check_int_equals(d.channels, 4);
			got = dominant_tone(&d, H.channel, d.frames / 2 - 12000, 24000, cands, 3);
			fst_check_int_equals((int)got, 1100);
			decoded_free(&d);
			free(rx.buf);
			use_sink("mock", 1);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(capacity_and_packing)
		{
			/* calls fill the lowest bucket first, the next bucket opens when it is full,
			   and attaching past max-buckets is refused (the call simply goes without STT) */
			fake_call_t calls[9];
			char uuids[9][64];
			const char *name = NULL;
			int i, ch;

			mock_reset();
			fst_requires(switch_stt_bucket_register_sink(&mock_sink) == SWITCH_STATUS_SUCCESS);
			use_sink("mock", 2);

			for (i = 0; i < 9; i++) {
				memset(&calls[i], 0, sizeof(calls[i]));
				switch_snprintf(uuids[i], sizeof(uuids[i]), "33333333-0000-0000-0000-%012d", i);
				calls[i].uuid = uuids[i];
				calls[i].freq = 400 + 100 * i;
			}
			for (i = 0; i < 8; i++) {
				attach(&calls[i]);
				switch_stt_bucket_slot_info(calls[i].slot, &name, &ch);
				fst_check_string_equals(name, i < 4 ? "b0" : "b1");
				fst_check_int_equals(ch, i % 4);
			}
			fst_check(attach_call(&calls[8]) != SWITCH_STATUS_SUCCESS);
			fst_check(calls[8].slot == NULL);

			{
				fake_call_t *all[8];
				for (i = 0; i < 8; i++) all[i] = &calls[i];
				feed_ms(all, 8, 500);
			}
			fst_check_int_equals(mock.nstreams, 2); /* one stream (one provider connection) per bucket */

			for (i = 0; i < 8; i++) switch_stt_bucket_detach(calls[i].slot, calls[i].gen);
			fst_wait_until(mock.streams[0].closed && mock.streams[1].closed, 3000);
			fst_check(mock.streams[0].closed && mock.streams[1].closed);
			use_sink("mock", 1);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
