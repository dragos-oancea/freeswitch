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
 * switch_stt_bucket_sip.c -- STT bucket integration: real SIP/RTP calls between two local
 * sofia profiles. Audio is tapped in switch_core_media_read_frame(), so this covers the
 * hook, codec negotiation (PCMU and OPUS), attach on first media, detach on hangup,
 * the channel variables, attach-all / opt-out, and routing a channel back to its call.
 */
#include <switch.h>
#include <test/switch_test.h>
#include "stt_bucket_test_util.h"

#define MAX_LEGS 16

typedef struct {
	char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char peer[SWITCH_UUID_FORMATTED_LENGTH + 1];
	char stream_id[128];
	int channel;
	long start_ms, end_ms;
	int attached, detached;
	/* the first time on the bucket, kept when the leg leaves and joins again */
	int first_channel;
	long first_start_ms, first_end_ms;
} leg_rec_t;

static leg_rec_t legs[MAX_LEGS];
static int nlegs = 0, stream_closes = 0;
static switch_mutex_t *ev_mutex = NULL;

static leg_rec_t *leg_by_uuid(const char *uuid, int create)
{
	int i;
	for (i = 0; i < nlegs; i++) {
		if (!strcmp(legs[i].uuid, uuid)) return &legs[i];
	}
	if (!create || nlegs >= MAX_LEGS) return NULL;
	switch_copy_string(legs[nlegs].uuid, uuid, sizeof(legs[0].uuid));
	legs[nlegs].end_ms = -1;
	return &legs[nlegs++];
}

static void stt_event_cb(switch_event_t *event)
{
	const char *sub = switch_event_get_header(event, "Event-Subclass");
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	leg_rec_t *l;

	if (!sub) return;
	switch_mutex_lock(ev_mutex);
	if (!strcmp(sub, SWITCH_STT_BUCKET_EVENT_STREAM)) {
		const char *action = switch_event_get_header(event, "STT-Action");
		if (action && !strcmp(action, "close")) stream_closes++;
	} else if (uuid && !strcmp(sub, SWITCH_STT_BUCKET_EVENT_ATTACH) && (l = leg_by_uuid(uuid, 1))) {
		l->attached++;
		l->channel = atoi(switch_event_get_header_nil(event, "STT-Channel"));
		l->start_ms = atol(switch_event_get_header_nil(event, "STT-Start-Ms"));
		switch_copy_string(l->peer, switch_event_get_header_nil(event, "STT-Peer-UUID"), sizeof(l->peer));
		if (l->attached == 1) {
			l->first_channel = l->channel;
			l->first_start_ms = l->start_ms;
		}
		switch_copy_string(l->stream_id, switch_event_get_header_nil(event, "STT-Stream-ID"), sizeof(l->stream_id));
	} else if (uuid && !strcmp(sub, SWITCH_STT_BUCKET_EVENT_DETACH) && (l = leg_by_uuid(uuid, 1))) {
		const char *end = switch_event_get_header(event, "STT-End-Ms");
		l->detached++;
		l->end_ms = end ? atol(end) : -1;
		if (l->detached == 1) l->first_end_ms = l->end_ms;
	}
	switch_mutex_unlock(ev_mutex);
}

static int count_detached(void)
{
	int i, n = 0;
	switch_mutex_lock(ev_mutex);
	for (i = 0; i < nlegs; i++) n += legs[i].detached > 0;
	switch_mutex_unlock(ev_mutex);
	return n;
}

/* place a call from profile `from` to the other profile's `exten`; the calling leg plays `tone_hz`.
   Returns the calling leg's uuid in `uuid`. */
static switch_status_t place_call(const char *vars, const char *from, int to_port, const char *exten, int tone_hz, char *uuid, switch_size_t uuid_len)
{
	switch_stream_handle_t stream = { 0 };
	char *cmd;
	switch_status_t status = SWITCH_STATUS_FALSE;

	SWITCH_STANDARD_STREAM(stream);
	cmd = switch_mprintf("{%s}sofia/%s/%s@127.0.0.1:%d &endless_playback(tone_stream://%%(1000,0,%d))", vars, from, exten, to_port, tone_hz);
	switch_api_execute("originate", cmd, NULL, &stream);
	if (stream.data && !strncmp((char *)stream.data, "+OK ", 4)) {
		char *e;
		switch_copy_string(uuid, (char *)stream.data + 4, uuid_len);
		if ((e = strpbrk(uuid, "\r\n"))) *e = '\0';
		status = SWITCH_STATUS_SUCCESS;
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "originate %s -> %s\n", cmd, switch_str_nil((char *)stream.data));
	}
	switch_safe_free(cmd);
	switch_safe_free(stream.data);
	return status;
}

/* the other leg of a SIP call between our two profiles: same Call-ID, other uuid */
static switch_bool_t find_peer_leg(const char *uuid, char *peer, switch_size_t peer_len)
{
	switch_core_session_t *s = switch_core_session_locate(uuid);
	char *call_id = NULL;
	int i;

	if (!s) return SWITCH_FALSE;
	call_id = switch_core_session_strdup(s, switch_str_nil(switch_channel_get_variable(switch_core_session_get_channel(s), "sip_call_id")));
	switch_core_session_rwunlock(s);

	for (i = 0; i < nlegs; i++) {
		switch_core_session_t *o;
		if (!strcmp(legs[i].uuid, uuid) || !(o = switch_core_session_locate(legs[i].uuid))) continue;
		if (!zstr(call_id) && !strcmp(switch_str_nil(switch_channel_get_variable(switch_core_session_get_channel(o), "sip_call_id")), call_id)) {
			switch_copy_string(peer, legs[i].uuid, peer_len);
			switch_core_session_rwunlock(o);
			return SWITCH_TRUE;
		}
		switch_core_session_rwunlock(o);
	}
	return SWITCH_FALSE;
}

static const char *leg_var(const char *uuid, const char *var, char *buf, switch_size_t len)
{
	switch_core_session_t *s = switch_core_session_locate(uuid);
	*buf = '\0';
	if (s) {
		switch_copy_string(buf, switch_str_nil(switch_channel_get_variable(switch_core_session_get_channel(s), var)), len);
		switch_core_session_rwunlock(s);
	}
	return buf;
}

static int legs_with(int attached, int detached)
{
	int i, n = 0;
	switch_mutex_lock(ev_mutex);
	for (i = 0; i < nlegs; i++) n += legs[i].attached == attached && legs[i].detached == detached;
	switch_mutex_unlock(ev_mutex);
	return n;
}

static void api(const char *cmd, const char *args)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute(cmd, args, NULL, &stream);
	switch_safe_free(stream.data);
}

static void kill_call(const char *uuid)
{
	switch_stream_handle_t stream = { 0 };
	SWITCH_STANDARD_STREAM(stream);
	switch_api_execute("uuid_kill", uuid, NULL, &stream);
	switch_safe_free(stream.data);
}

static void use_mock_sink(switch_bool_t attach_all)
{
	switch_stt_bucket_config_t cfg;
	switch_stt_bucket_get_config(&cfg);
	switch_copy_string(cfg.sink, "mock", sizeof(cfg.sink));
	cfg.attach_all = attach_all;
	switch_stt_bucket_configure(&cfg);
}

static const double TONES[] = { 500, 600, 700, 900, 1100, 1300, 1500, 1700 };

FST_CORE_BEGIN("./conf_stt_bucket_sip")
{
	FST_SUITE_BEGIN(switch_stt_bucket_sip)
	{
		FST_SETUP_BEGIN()
		{
			fst_requires_module("mod_sofia");
			fst_requires_module("mod_opus");
			fst_requires(switch_stt_bucket_enabled());
			if (!ev_mutex) {
				static switch_memory_pool_t *ev_pool = NULL; /* outlives fst_pool, which is per test */
				switch_core_new_memory_pool(&ev_pool);
				switch_mutex_init(&ev_mutex, SWITCH_MUTEX_NESTED, ev_pool);
			}
			mock_reset();
			switch_stt_bucket_register_sink(&mock_sink);
			memset(legs, 0, sizeof(legs));
			nlegs = stream_closes = 0;
			switch_event_bind("stt_bucket_sip_test", SWITCH_EVENT_CUSTOM, SWITCH_EVENT_SUBCLASS_ANY, stt_event_cb, NULL);
			fst_wait_until(fst_sofia_profile_running("stt_a") && fst_sofia_profile_running("stt_b"), 10000);
			fst_requires(fst_sofia_profile_running("stt_a") && fst_sofia_profile_running("stt_b"));
		}
		FST_SETUP_END()

		FST_TEARDOWN_BEGIN()
		{
			switch_event_unbind_callback(stt_event_cb);
		}
		FST_TEARDOWN_END()

		FST_TEST_BEGIN(real_calls_pcmu_and_opus)
		{
			/* call 1 negotiates PCMU, call 2 OPUS; every leg plays its own tone and must appear,
			   alone, on the channel its call was given */
			struct { const char *codec; const char *exten; int in_tone, out_tone; char out_uuid[64], in_uuid[64]; } calls[2] = {
				{ "PCMU", "tone1300", 1300, 700 },
				{ "OPUS", "tone1700", 1700, 900 }
			};
			char buf[256], uuid[64];
			decoded_t d;
			mock_stream_t *ms;
			int i, j;

			use_mock_sink(SWITCH_FALSE);

			for (i = 0; i < 2; i++) {
				char *vars = switch_mprintf("stt_bucket=true,absolute_codec_string=%s", calls[i].codec);
				fst_requires(place_call(vars, "stt_a", 62181, calls[i].exten, calls[i].out_tone, calls[i].out_uuid, sizeof(calls[i].out_uuid)) == SWITCH_STATUS_SUCCESS);
				switch_safe_free(vars);
			}

			/* both legs of both calls attach on their first audio frame */
			fst_wait_until(nlegs >= 4, 5000);
			fst_check_int_equals(nlegs, 4);
			for (i = 0; i < 2; i++) {
				fst_requires(find_peer_leg(calls[i].out_uuid, calls[i].in_uuid, sizeof(calls[i].in_uuid)));
				fst_check(!strcasecmp(leg_var(calls[i].out_uuid, "read_codec", buf, sizeof(buf)), calls[i].codec));
				fst_check(!strcasecmp(leg_var(calls[i].in_uuid, "read_codec", buf, sizeof(buf)), calls[i].codec));
			}

			/* the channel variables agree with the events */
			for (j = 0; j < nlegs; j++) {
				fst_check_int_equals(legs[j].attached, 1);
				fst_check_int_equals(atoi(leg_var(legs[j].uuid, "stt_bucket_channel", buf, sizeof(buf))), legs[j].channel);
				fst_check_string_equals(leg_var(legs[j].uuid, "stt_bucket_name", buf, sizeof(buf)), "b0");
			}

			switch_sleep(2500000); /* 2.5 s of audio on every channel */

			for (i = 0; i < 2; i++) kill_call(calls[i].out_uuid);

			/* hangup (BYE to the other leg) detaches all four, then the idle bucket closes its stream */
			fst_wait_until(count_detached() == 4, 5000);
			fst_check_int_equals(count_detached(), 4);
			fst_wait_until(stream_closes >= 1, 5000);
			fst_requires(stream_closes >= 1);

			fst_requires(mock.nstreams == 1);
			ms = &mock.streams[0];
			fst_requires(decode_ogg_opus(switch_buffer_get_head_pointer(ms->data), switch_buffer_inuse(ms->data), &d) == 0);
			fst_check_int_equals(d.channels, 8);
			printf("decoded %ld ms of %d channels\n", d.frames / 48, d.channels);

			for (i = 0; i < 2; i++) {
				struct { const char *uuid; int expect; } check[2] = {
					{ calls[i].out_uuid, calls[i].in_tone }, /* the caller hears the called side's tone */
					{ calls[i].in_uuid, calls[i].out_tone }
				};
				for (j = 0; j < 2; j++) {
					leg_rec_t *l = leg_by_uuid(check[j].uuid, 0);
					long start, n;
					double got;

					fst_requires(l);
					printf("%s leg %.8s ch %d: on channel %ld..%ld ms\n", calls[i].codec, check[j].uuid, l->channel, l->start_ms, l->end_ms);
					fst_check(l->end_ms > l->start_ms + 1500);
					/* middle second of that leg's time on the channel */
					start = (l->start_ms + (l->end_ms - l->start_ms) / 2 - 500) * 48;
					n = 48000;
					got = dominant_tone(&d, l->channel, start, n, TONES, (int)(sizeof(TONES) / sizeof(TONES[0])));
					printf("%s leg %.8s ch %d: expect %d Hz, got %.0f Hz\n", calls[i].codec, check[j].uuid, l->channel, check[j].expect, got);
					fst_check_int_equals((int)got, check[j].expect);

					/* the provider-side lookup routes that channel/time back to this leg */
					fst_check(switch_stt_bucket_resolve(l->stream_id, l->channel, (uint64_t)(start / 48 + 500), uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
					fst_check_string_equals(uuid, check[j].uuid);
				}
			}

			/* channels nobody used stay silent */
			for (i = 0; i < d.channels; i++) {
				int used = 0;
				for (j = 0; j < nlegs; j++) used |= legs[j].channel == i;
				if (!used) fst_check(channel_rms(&d, i, 0, d.frames) < 50);
			}
			decoded_free(&d);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(bridged_call_both_legs)
		{
			/* A phone calls the PBX (extension bridge900); the dialplan sets stt_bucket=true and bridges to
			   another phone. The PBX's two legs must land on two channels: the A-leg carrying what the caller
			   says (700 Hz), the B-leg what the callee says (900 Hz). The phones themselves stay out. */
			char caller[64], a_leg[64] = "", b_leg[64] = "", buf[64], uuid[64];
			leg_rec_t *a = NULL, *b = NULL;
			decoded_t d;
			int i;

			use_mock_sink(SWITCH_FALSE);
			fst_requires(place_call("stt_bucket=false,absolute_codec_string=PCMU", "stt_a", 62181, "bridge900", 700, caller, sizeof(caller)) == SWITCH_STATUS_SUCCESS);

			fst_wait_until(nlegs >= 2, 5000);
			switch_sleep(300000);
			fst_requires(nlegs == 2);

			/* the B-leg is the one the A-leg originated */
			for (i = 0; i < 2; i++) {
				char orig[64];
				if (!zstr(leg_var(legs[i].uuid, "originator", orig, sizeof(orig))) && !strcmp(orig, legs[!i].uuid)) {
					b = &legs[i];
					a = &legs[!i];
				}
			}
			fst_requires(a && b);
			switch_copy_string(a_leg, a->uuid, sizeof(a_leg));
			switch_copy_string(b_leg, b->uuid, sizeof(b_leg));
			fst_check(strcmp(a_leg, caller) != 0);
			fst_check(strcmp(b_leg, caller) != 0);

			/* the B-leg joined without the variable of its own, and its event pairs it with the A-leg */
			fst_check(zstr(leg_var(b_leg, "stt_bucket", buf, sizeof(buf))));
			fst_check_string_equals(b->peer, a_leg);
			fst_check(zstr(a->peer) || !strcmp(a->peer, b_leg));
			fst_check(a->channel != b->channel);

			switch_sleep(2000000);

			/* turning STT off for the conversation mid-call takes both legs out; the call goes on */
			{
				char *args = switch_mprintf("%s stt_bucket false", a_leg);
				api("uuid_setvar", args);
				switch_safe_free(args);
			}
			fst_wait_until(legs_with(1, 1) == 2, 3000);
			fst_check_int_equals(legs_with(1, 1), 2);
			fst_check(!zstr(leg_var(a_leg, "read_codec", buf, sizeof(buf)))); /* still up */
			fst_check(zstr(leg_var(a_leg, "stt_bucket_channel", buf, sizeof(buf))));
			fst_check(zstr(leg_var(b_leg, "stt_bucket_channel", buf, sizeof(buf))));

			/* and back on: both join again */
			{
				char *args = switch_mprintf("%s stt_bucket true", a_leg);
				api("uuid_setvar", args);
				switch_safe_free(args);
			}
			fst_wait_until(legs_with(2, 1) == 2, 3000);
			fst_check_int_equals(legs_with(2, 1), 2);
			fst_check_string_equals(b->peer, a_leg);
			switch_sleep(1000000);

			kill_call(caller);
			fst_wait_until(legs_with(2, 2) == 2, 5000);
			fst_check_int_equals(legs_with(2, 2), 2);
			fst_check_int_equals(nlegs, 2); /* the two phones never joined */
			fst_wait_until(stream_closes >= 1, 5000);
			fst_requires(stream_closes >= 1);

			fst_requires(mock.nstreams == 1);
			fst_requires(decode_ogg_opus(switch_buffer_get_head_pointer(mock.streams[0].data), switch_buffer_inuse(mock.streams[0].data), &d) == 0);
			{
				struct { leg_rec_t *l; int expect; const char *name; } check[2] = { { a, 700, "A-leg" }, { b, 900, "B-leg" } };
				for (i = 0; i < 2; i++) {
					leg_rec_t *l = check[i].l;
					long mid = l->first_start_ms + (l->first_end_ms - l->first_start_ms) / 2;
					double got;

					printf("%s ch %d: %ld..%ld ms, then ch %d from %ld ms\n", check[i].name, l->first_channel, l->first_start_ms, l->first_end_ms,
						   l->channel, l->start_ms);
					fst_check(l->first_end_ms > l->first_start_ms + 1500);
					got = dominant_tone(&d, l->first_channel, (mid - 500) * 48, 48000, TONES, (int)(sizeof(TONES) / sizeof(TONES[0])));
					printf("%s: expect %d Hz, got %.0f Hz\n", check[i].name, check[i].expect, got);
					fst_check_int_equals((int)got, check[i].expect);
					fst_check(switch_stt_bucket_resolve(l->stream_id, l->first_channel, (uint64_t)mid, uuid, sizeof(uuid)) == SWITCH_STATUS_SUCCESS);
					fst_check_string_equals(uuid, l->uuid);

					/* the second stay carries the same side of the conversation */
					got = dominant_tone(&d, l->channel, (l->start_ms + 200) * 48, 48000 / 2, TONES, (int)(sizeof(TONES) / sizeof(TONES[0])));
					fst_check_int_equals((int)got, check[i].expect);
				}
			}
			decoded_free(&d);
			fst_wait_until(switch_core_session_count() == 0, 5000);
		}
		FST_TEST_END()

		FST_TEST_BEGIN(attach_all_and_opt_out)
		{
			/* attach-all puts calls in the bucket without the variable; stt_bucket=false keeps a call out */
			char plain_out[64], plain_in[64] = "", opt_out[64], buf[64];
			leg_rec_t *l;

			use_mock_sink(SWITCH_TRUE);

			fst_requires(place_call("absolute_codec_string=PCMU", "stt_a", 62181, "plain1100", 500, plain_out, sizeof(plain_out)) == SWITCH_STATUS_SUCCESS);
			fst_requires(place_call("stt_bucket=false,absolute_codec_string=PCMU", "stt_a", 62181, "optout1500", 600, opt_out, sizeof(opt_out)) == SWITCH_STATUS_SUCCESS);

			fst_wait_until(nlegs >= 2, 5000);
			switch_sleep(500000); /* give the opted-out call the same chance to (wrongly) attach */

			fst_check_int_equals(nlegs, 2);
			fst_requires(find_peer_leg(plain_out, plain_in, sizeof(plain_in)));
			fst_check((l = leg_by_uuid(plain_out, 0)) && l->attached == 1);
			fst_check((l = leg_by_uuid(plain_in, 0)) && l->attached == 1);
			fst_check(leg_by_uuid(opt_out, 0) == NULL);
			fst_check(zstr(leg_var(opt_out, "stt_bucket_channel", buf, sizeof(buf))));

			kill_call(plain_out);
			kill_call(opt_out);
			fst_wait_until(count_detached() == 2, 5000);
			fst_check_int_equals(count_detached(), 2);
			fst_wait_until(switch_core_session_count() == 0, 5000);

			use_mock_sink(SWITCH_FALSE);
		}
		FST_TEST_END()
	}
	FST_SUITE_END()
}
FST_CORE_END()
