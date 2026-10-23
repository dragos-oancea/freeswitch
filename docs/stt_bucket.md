# STT bucket: many calls, one multichannel Ogg/Opus stream

The STT bucket takes the inbound audio of many calls and sends it to a speech-to-text
provider over **one long-lived connection**. Each call gets its own channel in a single
Ogg/Opus stream (channel mapping family 255, up to 255 independent mono channels).
Results that come back per channel are routed to the right call with the events and
the lookup described below.

It lives in the core (`src/switch_core_stt_bucket.c`). Audio is tapped in
`switch_core_media_read_frame()`, with no media bug, so it works for any call that has
media flowing through FreeSWITCH: inbound, outbound, and both legs of a bridge.

## Data path

```
RTP read (per call thread, switch_core_media_read_frame)
   -> private decoder + resampler (per channel; the session codec is not touched)
   -> per-channel PCM ring buffer
bucket clock thread (one per bucket, every ptime)
   -> one frame from every channel (silence if none) -> interleave N channels
   -> libopusenc, mapping family 255
   -> Ogg pages -> sink (file / tcp / a sink registered by a module)
```

## Requirements

- FreeSWITCH built with libopusenc (`configure` detects it with pkg-config; without it
  the bucket logs an error and stays disabled).
- Media must flow through FreeSWITCH: calls with `bypass_media` or `proxy_media` cannot
  join.

## Quick start

`autoload_configs/stt_bucket.conf.xml`:

```xml
<configuration name="stt_bucket.conf">
  <settings>
    <param name="enabled" value="true"/>
    <param name="sink" value="file"/>
    <param name="file-path" value="$${recordings_dir}/stt_bucket_%s.opus"/>
  </settings>
</configuration>
```

Dialplan, a bridged call with both sides transcribed:

```xml
<extension name="to_agent">
  <condition field="destination_number" expression="^(\d+)$">
    <action application="set" data="stt_bucket=true"/>
    <action application="bridge" data="sofia/gateway/carrier/$1"/>
  </condition>
</extension>
```

The A-leg joins because of the variable; the B-leg created by `bridge` joins because it
follows the leg that originated it (`bridge-legs`, on by default). Each side of the
conversation lands on its own channel.

## Which calls join

The decision is made on the call's first audio frame and then again about every half
second, so it can change during the call.

| Situation | Result |
|---|---|
| `stt_bucket=true` on the channel | joins |
| `stt_bucket=false` on the channel | stays out / leaves |
| variable not set, `attach-all=true` | joins |
| variable not set, the channel was originated by a call with `stt_bucket=true`, `bridge-legs=true` | joins (B-leg of a bridge) |
| anything else | stays out |

Mid-call control:

```
uuid_setvar <a-leg-uuid> stt_bucket false   # the conversation leaves (B-leg follows)
uuid_setvar <a-leg-uuid> stt_bucket true    # and joins again, on new channels
```

When a call leaves, its buffered audio is still sent; the channel then stays silent for
`quarantine-ms` before another call may use it, so late results for the old call cannot
be mistaken for the new one.

## Channel variables set on the call

| Variable | Meaning |
|---|---|
| `stt_bucket_name` | bucket (`b0`, `b1`, ...) = which provider connection |
| `stt_bucket_channel` | channel index in that stream (0-based) |

Both are cleared when the call leaves the bucket.

## Events

All are `CUSTOM` events.

**`stt_bucket::attach`**: a call appears on a channel of a stream.
**`stt_bucket::detach`**: its audio on that channel ends.

| Header | |
|---|---|
| `Unique-ID` | the call (channel UUID) |
| `STT-Peer-UUID` | the other leg of the conversation, when known (for a B-leg: its A-leg) |
| `STT-Bucket` | `b0`, `b1`, ... |
| `STT-Stream-ID` | id of the Ogg stream (see below) |
| `STT-Channel` | channel index |
| `STT-Start-Ms` | stream time at which the call's audio starts on the channel |
| `STT-End-Ms` | (detach only) stream time at which it ends |

**`stt_bucket::stream`**: a stream opens or closes.

| Header | |
|---|---|
| `STT-Action` | `open` or `close` |
| `STT-Bucket`, `STT-Stream-ID` | |
| `STT-Channels`, `STT-Rate` | stream layout |
| `STT-Duration-Ms` | stream length so far |

Stream ids look like `<node>-<bucket>-<epoch seconds>-<n>`. A new stream (and a new id,
with fresh Ogg headers) starts whenever the sink fails or the bucket was idle for
`idle-timeout-ms`. A call that is in the bucket across a restart gets a new `attach`
event for the new stream.

Stream time is the position in the stream in milliseconds, which is also how providers
timestamp their results, so `(stream id, channel, result time)` identifies the call.

## Routing results back

From fs_cli / ESL:

```
stt_bucket status
stt_bucket resolve <stream_id> <channel> <offset_ms>
```

`resolve` prints the UUID of the call that was on that channel at that stream time.
From C: `switch_stt_bucket_resolve()`. The last 8 occupancies of every channel are kept.

To pair the two sides of a bridged conversation, use `STT-Peer-UUID` from the B-leg's
attach event.

## Configuration reference

| Param | Default | |
|---|---|---|
| `enabled` | `false` | |
| `channels` | `255` | calls per stream, 1..255 |
| `max-buckets` | `4` | streams = provider connections, up to 64. Calls are packed into the lowest bucket first |
| `rate` | `16000` | 8000, 12000, 16000, 24000 or 48000 |
| `ptime` | `20` | mixer clock, ms: 10, 20, 40 or 60 |
| `bitrate-per-channel` | `16000` | Opus bitrate per channel, bps |
| `page-ms` | `250` (`100` in the sample config) | force an Ogg page out this often (latency against overhead) |
| `quarantine-ms` | `2000` | a freed channel stays silent this long before reuse |
| `idle-timeout-ms` | `5000` | close the stream when no call has been in the bucket this long |
| `max-buffer-ms` | `500` | cap of buffered audio per call; older audio is dropped on overflow |
| `dtx` | `false` | Opus DTX on silent channels: smaller stream, but ffmpeg's family-255 decoder bleeds audio across channels with it |
| `attach-all` | `false` | every call joins unless it has `stt_bucket=false` |
| `bridge-legs` | `true` | the B-leg of a bridge joins and leaves with its A-leg |
| `sink` | `file` | `file`, `tcp`, or the name of a sink registered by a module |
| `file-path` | | file sink: path template, `%s` = stream id |
| `tcp-target` | | tcp sink: `host:port`; the raw Ogg byte stream is written to the socket |

## Sinks

A sink receives the Ogg pages of one stream. Built in:

- **file**: one `.opus` file per stream, for testing and offline jobs.
- **tcp**: the raw Ogg stream over a TCP connection, one connection per stream.

Modules can add their own, e.g. a websocket client for a given provider:

```c
static switch_status_t my_open(switch_stt_bucket_t *b, const char *stream_id, int channels, int rate, void **sink_data);
static switch_status_t my_write(switch_stt_bucket_t *b, void *sink_data, const uint8_t *page, switch_size_t len);
static void my_close(switch_stt_bucket_t *b, void *sink_data);

static const switch_stt_bucket_sink_t my_sink = { "myprovider", my_open, my_write, my_close };

/* load */   switch_stt_bucket_register_sink(&my_sink);
/* unload */ switch_stt_bucket_unregister_sink("myprovider");
```

then `<param name="sink" value="myprovider"/>`.

- All callbacks run on the bucket's clock thread: `write()` must not block for long.
- Returning an error from `write()` closes the sink and restarts the stream (new id, new
  headers), retrying about once a second.
- Unregistering a sink stops the buckets using it; buckets that still carry calls restart
  once a sink with that name is available again.

## C API

Declared in `src/include/switch_stt_bucket.h`:

- `switch_stt_bucket_attach()` / `switch_stt_bucket_attach_peer()` / `switch_stt_bucket_detach()`:
  claim and release a channel by hand, e.g. for audio that does not come from RTP.
- `switch_stt_bucket_feed_frame()`: an encoded payload, decoded with a private decoder.
- `switch_stt_bucket_feed_pcm()`: signed linear mono at any rate.
- `switch_stt_bucket_configure()` / `switch_stt_bucket_get_config()`: change settings at
  runtime; they apply to streams opened afterwards.

## Decoding a stream for checks

libopusfile (and so mod_opusfile) refuses channel mapping family 255. ffmpeg decodes it,
and `tests/unit/stt_bucket_verify.py <file.opus>` prints the dominant tone of every
channel in 100 ms windows. The unit tests decode with libopus directly
(`tests/unit/stt_bucket_test_util.h`).

## Tests

- `tests/unit/switch_stt_bucket.c`: the bucket itself (calls multiplexed into channels, sink failure and restart,
  unregistering a sink while streaming, the tcp sink, capacity and packing).
- `tests/unit/switch_stt_bucket_sip.c`: real SIP/RTP calls between two local sofia
  profiles: PCMU and OPUS calls, attach-all and opt-out, and a bridged call where only the
  PBX's two legs join, each carrying its own side, with off and on again mid-call.

## Limitations

- Only inbound audio of each leg is tapped; for a bridged call that covers both speakers,
  one per channel.
- No audio for calls whose media bypasses FreeSWITCH.
- Results from the provider are not parsed by the core; a sink module (or an ESL client
  using `resolve`) maps them back to calls.
