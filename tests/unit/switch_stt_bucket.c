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
	}
	FST_SUITE_END()
}
FST_CORE_END()
