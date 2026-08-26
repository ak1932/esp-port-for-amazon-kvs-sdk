# KVS Combined: PutMedia + WebRTC from one camera

Runs both KVS consumers against a single camera and a single H.264 encoder, each
independently start/stoppable from the console. This is the example that
exercises multi-consumer capture in `components/media_stream`.

## What it demonstrates

Two consumers attach to the same encoded stream by different routes:

Both register an encoded-video **sink** (`video_sink.h`) and receive every frame
as a borrowed buffer on the encoder task. What they do inside the callback is
where they differ:

| | sink name | inside the callback | why |
|---|---|---|---|
| **putmedia** (`putmedia.c`) | `putmedia` | copies into the MKV muxer and returns — but only while the PutMedia session is open | a copy plus a list insert fits comfortably in the frame budget; muxing a frame the session cannot accept does not |
| **webrtc** (`kvs_media.c`) | `webrtc` | copies onto its own queue and returns | `writeFrame()` packetizes and sends RTP inline, per session; run on the encoder task it would stall capture for putmedia too, so the send stays on `kvsGlobalVideo` |

There is no pull API in this picture any more. `video_capture_get_frame()` used
to be backed by a built-in `legacy-pull` sink inside `media_stream`; that shim is
gone, and a consumer that needs to do slow work owns the queue that implies —
which is exactly what `kvs_media.c` now does, where the depth and drop policy can
be chosen against WebRTC's actual requirements.

Underneath, `video_capture_init/start/stop/deinit` are reference counted: the
camera comes up for the first consumer and is torn down after the last. Any
order works — start either, start the other, stop either, restart it.

### The trade-off to be aware of

Sinks are dispatched **sequentially on the encoder task** with a *borrowed*
frame that is valid only for the duration of the callback. There is no
per-consumer queue, so nothing can overflow — but every sink shares one
per-frame budget (33 ms at 30 fps) with the encoder itself, and a slow callback
lowers the capture rate for everyone.

That is why the sink API is instrumented rather than merely documented:

- `sink-stats` reports per-sink `max_us` / `avg_us`. Compare `max_us` against the
  frame period; anything approaching it is costing frames.
- A callback slower than `CONFIG_VIDEO_SINK_SLOW_WARN_US` (default 5 ms) is named
  in a rate-limited warning.
- The grabber's 1 s `enc_fps:` line carries `sink_ms=<spent>/<window>`, so a sag
  in capture rate can be attributed to the sinks or exonerated at a glance.

It is also why putmedia refuses frames the PutMedia session cannot accept
*before* copying them. `video_sink_on_frame()` gates on
`put_media_is_session_live()`, which is false whenever there is no open PutMedia
request — the initial connect (DescribeStream, GetDataEndpoint and PutMedia, which
takes seconds, and is never retried if it fails) **and** every later reopen, on a
credential refresh, the 40-minute AWS ceiling, or recovery from a failed write.
Muxing through those windows cost a PSRAM copy, an Annex-B to AVCC rewrite and two
NALU walks per frame out of the budget **the WebRTC sink also draws on**, for
buffers `put_media_enqueue()` refuses anyway.

Note the deliberate pair: `put_media_is_started()` asks whether the upload task is
running and stays true across a reopen; `put_media_is_session_live()` asks whether
there is a connection right now. Producers want the second. See `KNOWN_ISSUES.md`
items 45 and 46.

## Console commands

```
start-putmedia     start PutMedia upload
stop-putmedia      stop it (camera stays up if WebRTC is running)
start-webrtc       start KVS WebRTC signaling and streaming
stop-webrtc        stop it (camera stays up if PutMedia is running)
sink-stats         per-sink frame counts and callback timing
sink-stats-reset   zero the counters
```

## Configuration

`idf.py menuconfig`:

- **AWS Security Credentials** (from `app_common`) — region, WebRTC channel name,
  and credentials. PutMedia signs its own SigV4 requests through
  `kvs_fetch_credentials_cb` and needs **static keys**, so this example defaults
  to `CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n`; fill in `AWS_ACCESS_KEY_ID`,
  `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`. Use a short-lived STS session
  rather than a long-lived IAM user key, and do not commit them.
- **KVS Combined Example** — the PutMedia stream name (separate from the WebRTC
  signaling channel), retention, and the capture profile.

The IAM identity needs `kinesisvideo:DescribeStream`, `:CreateStream`,
`:GetDataEndpoint` and `:PutMedia` for putmedia, plus the usual
signaling-channel permissions for WebRTC.

## Build

The overlay chain must be passed **in the same `set-target` call** — a bare
`set-target` writes an `sdkconfig` that is missing everything the overlay sets,
and re-running with `-D SDKCONFIG_DEFAULTS` afterwards will not fill it back in.

ESP32-P4:

```bash
export KVS_SDK_PATH=<path to this repo>
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py menuconfig      # set credentials
idf.py build flash monitor
```

ESP32-S31 (still a preview target, hence `--preview`):

```bash
idf.py --preview -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s31" \
       set-target esp32s31
idf.py --preview build flash monitor
```

The S31 has no BSP (`CONFIG_BSP_SELECT_NONE=y`), no MIPI-CSI camera, and no H.264
encoder or ISP at all. Its overlay selects a **USB-UVC camera that emits H.264
itself**, forwarded to both sinks without re-encoding — `CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE=y`
plus `CONFIG_MEDIA_STREAM_ENABLE_USB_UVC_CAM_SENSOR=y` (both needed; the second
depends on the first and is silently dropped without it) and
`CONFIG_MEDIA_STREAM_UVC_PASSTHROUGH_H264` on by default, and
`CONFIG_USE_ESP_VIDEO_IF=y` (there is no non-esp_video path to the camera).

With passthrough the camera owns bitrate, GOP length and keyframe placement, so on
this target `video_rate_ctrl` does nothing, bitrate control and JPEG snapshots
return `ESP_ERR_NOT_SUPPORTED`, and a viewer's PLI cannot shorten the wait for a key
frame — recovery is bounded by the camera's own GOP. Expect exactly one
`camera does not support on-demand key frames` warning at startup: esp_video 2.4.0
exposes no V4L2 control for it, so the grabber asks once and stops.
See `components/media_stream/README.md` for the full capability matrix.

**BLE provisioning is off on the S31** (`CONFIG_BT_ENABLED=n`,
`CONFIG_APP_NETWORK_PROV_BLE=n`). Configure Wi-Fi from the console instead:

```
wifi-set <ssid> <password>
```

This is a memory decision, not a preference. NimBLE allocates from internal RAM, the
controller is never torn down once provisioning finishes, and `esp_bt_mem_release` is
unimplemented on this target — the boot log says so outright
(`esp_bt_mem_release not implement yet!`). An already-provisioned device was paying for
the whole NimBLE + controller footprint for the rest of the session, out of the same
internal pool a TLS handshake needs. Set both options back to `y` if you need BLE
provisioning here; `app_wifi_prov.c` supports either path.

### mbedTLS options WebRTC needs

`sdkconfig.defaults` turns on four mbedTLS features that ESP-IDF leaves off by
default on both v5 and v6 — `MBEDTLS_SSL_DTLS_SRTP`, `MBEDTLS_X509_CREATE_C`,
`MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` and `MBEDTLS_SSL_KEYING_MATERIAL_EXPORT`.
Without them `Dtls_mbedtls.c` fails to compile with `unknown type name
'mbedtls_ssl_srtp_profile'` and implicit declarations of `mbedtls_x509write_crt_*`;
the symbols exist in every supported mbedTLS, they are just compiled out. If you
build from a hand-written `sdkconfig`, carry those four across.

## What to look for

1. `start-putmedia` alone — one `No PutMedia session, holding frames` line, then
   a single `Feeding the muxer, opened on keyframe (held N frames)` once the
   session is up. No `upload:` line before it, and `sink-stats` `max_us` for
   `putmedia` stays near zero for the whole connect window. After that: fragment
   acks flow, `enc_fps: out=` matches the configured rate, no KVS error 4006.

   The same pair of lines should bracket every **reopen**, and that window is the
   one to watch: between `Restarting Put Media session` and `Session opened`,
   `queued` should stay flat or fall and `sink_ms=` should drop toward zero. It
   used to add ~300 buffers there, which is what drove the queue to its ceiling
   (`KNOWN_ISSUES.md` item 46).
2. Then `start-webrtc` — putmedia keeps acking while WebRTC connects. Check
   `sink-stats`: `putmedia` and `webrtc` both show rising frame counts, and both
   `max_us` values sit well under the frame period.
3. `stop-webrtc`, then `stop-putmedia`, in either order. The camera should stay
   up until the second one, and the survivor should see no gap.
4. Starting putmedia **while** WebRTC is already streaming: `sink-stats` shows
   a non-zero `gate_held` for `putmedia` that then stops growing. That is the
   registry's start-on-keyframe gate holding frames until the next keyframe, so
   the muxer sees SPS/PPS before anything else. It costs at most one GOP (~0.5 s
   at gop=15). The `webrtc` sink's `avg_us` and the grabber's `sink_ms=` should
   *not* dip across this window — putmedia is not muxing yet, so it is not
   spending the shared budget.

   putmedia's own gate reports the same wait separately, in the `held N
   frames` count on the `Feeding the muxer` line. Both exist because they cover
   different moments: `gate_held` counts the registry gate on the one enable at
   `putmedia_start()`, and `held N` counts every later reopen of the capture gate,
   by which time the registry's has long been consumed.

5. A start that cannot succeed (bad `CONFIG_AWS_ACCESS_KEY_ID`): the `PutMedia
   error` line, then `Session did not open; ... PutMedia does not retry`, then
   silence. No per-frame logging and a flat `sink_ms=` — PutMedia does not retry,
   so `stop-putmedia` / `start-putmedia` is the way back.

6. PutMedia's throughput accounting is at `DEBUG` and off by default:
   `esp_log_level_set("PUT_MEDIA", ESP_LOG_DEBUG)` for the 1 s `stage/` line (where
   the upload thread's wall clock went) and
   `esp_log_level_set("putmedia", ESP_LOG_DEBUG)` for the 1 s `upload:` line
   (buffers/bytes sent and queue depth). A run that is *losing* buffers reports it
   at `WARN` regardless, since that is a fault rather than a metric.

   The per-chunk HTTP write timing that used to print alongside as `http:`
   (`frame` / `chunkhdr` / `write`) has been removed. What it established is
   recorded in
   [`components/putmedia/kvs_putmedia/KNOWN_ISSUES.md`](../../components/putmedia/kvs_putmedia/KNOWN_ISSUES.md);
   re-add it only if a throughput question actually needs per-write attribution again.

   `queued` will still climb on a link slower than the encoder: load shedding is
   not implemented, so the underlying rate mismatch (item 40) remains.

## NAT traversal: LAN vs the internet

A viewer on the same LAN connects on a host↔host candidate pair and needs nothing else. A viewer
on the internet needs a server-reflexive or relay candidate, so the ICE servers have to be
installed **before** the session is created — the SDK gathers reflexive candidates only from
servers present when gathering starts. Anything applied later yields a host-only session that
works on your desk and nowhere else. This was a real bug; see
[`components/kvs_webrtc/KNOWN_ISSUES.md`](../../components/kvs_webrtc/KNOWN_ISSUES.md) item 1.

When an off-LAN session fails, read these lines in order. The first two and the last two are
always logged; the per-candidate lines need
`esp_log_level_set("kvs_webrtc", ESP_LOG_DEBUG)` and
`esp_log_level_set("LWS_API_ESP", ESP_LOG_DEBUG)`, which are off by default so a healthy run is not
buried in one line per candidate.

- `Creating peer connection with STUN server: …` or `ICE server N: …` — servers reached the agent.
  `Creating peer connection without ICE servers` means they did not, and gathering will be
  host-only.
- `iceAgentStartGathering(): [Srflx candidates setup time]` — 0 ms means no STUN server was
  present, so no reflexive candidate will ever exist.
- *(DEBUG)* `Local ICE candidate trickled to peer …` — which candidate types you actually gathered.
  Only host candidates here is the failure above.
- *(DEBUG)* `Remote ICE candidate added: …` — proof the viewer's candidates arrived *and were
  accepted*. A run of `Remote ICE candidate skipped: …` with no `added` lines means the peer's
  candidates are all being thrown away, which fails exactly like receiving none.
  `Signaling message #N received: …` also carries a running count of *dropped* inbound messages;
  a drop is additionally reported at `ERROR` whatever the log level, because **any non-zero drop
  count is a fault**, not noise.
- Different viewers expose different bugs: a desktop Chrome viewer emits lowercase `udp` in its
  candidates while mobile stacks emit the spec's uppercase `UDP`, and a mobile client on CGNAT
  (`100.x`) has no usable host candidate at all. Test with a phone on mobile data, not just a
  laptop on another network.
- `TurnConnection in TURN_STATE_FAILED due to 0x5a00002c` is a 5 s get-credentials timeout, most
  often the `turns:` TCP server whose TLS handshake has to fit inside that window. Harmless on its
  own as long as the `turn:…transport=udp` server allocated and a `typ relay` candidate was
  trickled.
- `0x5a00000d` (`STATUS_ICE_NO_CONNECTED_CANDIDATE_PAIR`) is the 12 s connectivity-check window
  expiring — the consequence, not the cause. Look upwards for which candidates were missing.

## Where the PSRAM goes

If `mem-dump` shows free PSRAM falling from ~16 MB to ~4 MB when you start streaming, that is the
**camera**, not WebRTC. WebRTC's own share is ~136 KB PSRAM plus ~149 KB internal at that point
(cert pool, signaling client, CA store).

There are three capture buffers, each `min(width * height * 2, CONFIG_MEDIA_STREAM_UVC_COMPRESSED_FRAME_MAX_KB)`:

| capture size | uncapped | with the default 1 MB cap |
|---|---|---|
| 640x480 | 1.76 MB | 1.76 MB (cap does not bind) |
| 1280x720 | 5.53 MB | 3 MB |
| 1920x1080 | 11.86 MB | 3 MB |

The `width * height * 2` figure is the camera's declared `dwMaxVideoFrameSize` — an *uncompressed*
frame — even for its H.264 format. The buffer only has to hold the largest real frame, which for
H.264 is a keyframe, so it is capped. Capping too low is silent: `usb_host_uvc` drops every frame
that overruns the buffer and esp_video logs that at DEBUG, so keyframes vanish and the stream runs
while delivering nothing decodable. The symptom here is `failed to receive video frame` with
`enc_fps out=0` — raise the cap first. The two knobs are
`CONFIG_MEDIA_STREAM_UVC_COMPRESSED_FRAME_MAX_KB` and
`CONFIG_MEDIA_STREAM_UVC_CAPTURE_WIDTH` / `_HEIGHT`.

Note that `CONFIG_KVS_COMBINED_VIDEO_WIDTH/HEIGHT` in this example's own menu does **not** affect a
UVC camera — only `CONFIG_MEDIA_STREAM_UVC_CAPTURE_*` does. Details and the remaining levers:
[`components/kvs_webrtc/KNOWN_ISSUES.md`](../../components/kvs_webrtc/KNOWN_ISSUES.md) item 3e.

## Where the internal RAM goes — and why it matters more than PSRAM

PSRAM is plentiful here; **internal RAM, and specifically its DMA-capable subset, is
not**. That subset is what fails first, and it fails in places that do not look like
memory problems at all. The worked example: with WebRTC streaming, `start-putmedia`
died with

```
E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
```

which is `PSA_ERROR_INSUFFICIENT_MEMORY` — mbedTLS 4 defines
`MBEDTLS_ERR_SSL_ALLOC_FAILED` as that PSA code — raised by IDF's hardware SHA PSA
driver failing to get a context out of `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`. PSRAM
had 13 MB free throughout. The `wifi:m f null` lines a second earlier were the same
pool refusing a management frame.

`mem-dump` prints a **DMA-internal** column beside Internal and SPIRAM for exactly this
reason — read that column, not the Internal total. `putmedia.c` logs all three pools
either side of `put_media_start()`, and `app_main()` registers a
`heap_caps_register_failed_alloc_callback()` that names the size and caps of any failed
allocation, so a shortage reports itself instead of being inferred.

The levers that mattered, in order: `CONFIG_SPIRAM_USE_MALLOC` (under
`SPIRAM_USE_CAPS_ALLOC` every plain `malloc()` in esp_http_client, esp-tls and the KVS
SDK came out of the ~350 KB internal heap), BLE off, capture actually running at the
configured resolution rather than 1080p, and the Wi-Fi RX/management pools. Full
account: [`components/putmedia/kvs_putmedia/KNOWN_ISSUES.md`](../../components/putmedia/kvs_putmedia/KNOWN_ISSUES.md)
item 47.

## The camera is held for the whole WebRTC window

`start-webrtc` brings the camera up immediately and holds it until `stop-webrtc`, rather than
letting it come and go with each peer connection. This is not just an optimisation: on a UVC camera
the per-session teardown cannot be undone. Restarting the stream forces the capture buffers to be
freed and re-requested, and at 1080p that is three contiguous ~4 MB PSRAM blocks (`width*height*2`
each, a size the camera's declared frame size will not let us shrink). After one session's worth of
WebRTC and TLS allocation those blocks no longer exist, so a second peer connection failed with
`Failed to require buffers, errno: 12` and streamed without video. Details:
[`components/kvs_webrtc/KNOWN_ISSUES.md`](../../components/kvs_webrtc/KNOWN_ISSUES.md) item 3d.

Two consequences: the camera is powered and streaming whenever WebRTC is started, even with no
viewer connected; and `start-putmedia` during a live WebRTC session joins a camera that is already
up instead of trying to rebuild it.

## Known interaction

`video_rate_ctrl` is process-global and only the WebRTC path feeds it. When both
sides run, WebRTC backing off for congestion also throttles the frame rate
putmedia archives. This is recorded in
`components/putmedia/kvs_putmedia/KNOWN_ISSUES.md` and is not yet arbitrated.
