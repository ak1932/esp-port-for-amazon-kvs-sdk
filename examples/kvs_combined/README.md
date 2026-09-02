# KVS Combined Example — **PutMedia archive + WebRTC live, together**

Both Amazon Kinesis Video Streams paths from one device and one camera: a continuous
**PutMedia** upload into a KVS video stream for recording and playback, and a live
**WebRTC** session for viewers. Each can be started and stopped at runtime,
independently, without disturbing the other.

## What You'll Build

A camera that records to AWS continuously *and* serves a live low-latency viewer at the
same time. WebRTC starts automatically at boot; the archive upload is started from the
console with `start-putmedia`.

## Features

- **PutMedia archive** — MKV muxing plus a long-lived chunked HTTPS upload, with fragment
  acks, retention and automatic session reopen
- **WebRTC live** — KVS signaling and peer connection, viewable from the AWS test page
- **Independent control** — start/stop either path at any time, in any order
- **One camera, one encoder** — both paths share the capture pipeline; the camera comes up
  for the first path that needs it and goes down after the last

## Hardware Requirements

| Board | Camera | Notes |
|---|---|---|
| **ESP32-S31** | USB-UVC camera that emits H.264 itself | Preview target (`--preview`), native Wi-Fi, no on-chip encoder |
| **ESP32-P4-Function-EV-Board** / P4-Eye | MIPI-CSI sensor | Hardware H.264 encoder; **recommended starting point** |
| **ESP32-P4** + C6/C5 co-processor boards | MIPI-CSI sensor | Flash `examples/network_adapter` on the co-processor first |

8 MB flash and PSRAM are required (the example ships an 8 MB partition table).

## Build & Flash

This is tested on IDF master (esp32s31 target requires this)
The overlay chain must be passed **in the same `set-target` call** — a bare `set-target`
writes an `sdkconfig` missing everything the overlay sets, and re-running with
`-D SDKCONFIG_DEFAULTS` afterwards will not fill it back in.

```bash
export KVS_SDK_PATH=<path to this repo>
cd examples/kvs_combined

# ESP32-P4 (P4-Eye shown; pick your board overlay):
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py menuconfig                 # Wi-Fi + AWS credentials (see below)
idf.py flash monitor

# ESP32-S31 with a UVC camera:
idf.py --preview -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s31" \
       set-target esp32s31
idf.py menuconfig
idf.py flash monitor
```

Board overlays available here: `esp32p4`, `esp32s31`, `esp32s3`, `p4_eye.esp32p4`,
`p4_c5_core_board.esp32p4`, `p4_function_ev_board_v12.esp32p4`,
`p4_function_ev_board_v16.esp32p4`.

> **The first build on a fresh tree stops on purpose.** It applies the local patches to
> the camera-path managed components, and two of those add Kconfig options the running
> configure cannot see. **Run the build again** — it picks them up and the message does
> not come back.

For the shared mechanics — the overlay-chain gotcha, the ESP32-P4 + C6 dual-chip wiring,
console selection and generic troubleshooting — see
[**docs/building.md**](../../docs/building.md). Wi-Fi (BLE) provisioning is documented in
[app_common](../common_components/app_common/README.md#wifi-provisioning); you can also
set credentials at the console with `wifi-set <ssid> <password>`.

## Configure

`idf.py menuconfig`:

**AWS Security Credentials** — region, WebRTC channel name and credentials. PutMedia signs
its own SigV4 requests and needs **static keys**, so this example defaults to
`CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n`; fill in `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`. Use a short-lived STS session rather than
a long-lived IAM user key, and do not commit them. (IoT Core certificate credentials work
for the WebRTC side only.)

**KVS Combined Example** — the PutMedia **stream name** (separate from the WebRTC
signaling channel), stream retention in hours, and the capture profile.

The IAM identity needs `kinesisvideo:DescribeStream`, `:CreateStream`, `:GetDataEndpoint`
and `:PutMedia` for the archive, plus the usual signaling-channel permissions for WebRTC.
Both resources are created on demand if they do not exist and the identity is allowed to.

On a UVC camera, note that `CONFIG_KVS_COMBINED_VIDEO_WIDTH/HEIGHT` do **not** apply —
`CONFIG_MEDIA_STREAM_UVC_CAPTURE_*` and the `CONFIG_ESP_VIDEO_UVC_H264_*` options do.

## Console commands

```
start-putmedia     start the PutMedia archive upload
stop-putmedia      stop it (camera stays up if WebRTC is running)
start-webrtc       start KVS WebRTC signaling and streaming
stop-webrtc        stop it (camera stays up if PutMedia is running)
sink-stats         per-consumer frame counts and callback timing
sink-stats-reset   zero those counters
```

`reboot`, `mem-dump`, `task-dump`, `sock-dump`, `heap-trace` and
`wifi-set <ssid> <password>` come from the shared utilities.

## View your stream

**Live (WebRTC)** — open the
[AWS KVS WebRTC Test Page](https://awslabs.github.io/amazon-kinesis-video-streams-webrtc-sdk-js/examples/index.html),
enter your credentials and the channel name from menuconfig, then **Join as viewer** →
**Start Viewer**.

**Archive (PutMedia)** — run `start-putmedia`, then open the stream in the AWS console
(Kinesis Video Streams → your stream → Media playback), or pull HLS/DASH with
`GetHLSStreamingSessionURL`. Fragment acks with `errorId=0` in the device log mean AWS
accepted the data.

## Operating notes

- **WebRTC holds the camera for its whole window.** `start-webrtc` brings the camera up
  immediately and keeps it up until `stop-webrtc`, rather than cycling it per peer
  connection — re-requesting the large contiguous capture buffers after a session's worth
  of TLS allocation can fail. So the camera is powered even with no viewer, and
  `start-putmedia` during a live session joins a camera that is already up.
- **On a UVC camera, restart with `reboot`, not by cutting power.** A UVC camera cut off
  mid-stream does not recover until it is physically replugged, because a soft reset does
  not drop VBUS. `media_stream` stops the stream from a shutdown handler, which covers
  `esp_restart()`, an OTA reboot and the `reboot` command — but not the EN pin, a power
  cut or a panic.
- **Throughput accounting is at `DEBUG`:** `esp_log_level_set("PUT_MEDIA", ESP_LOG_DEBUG)`
  for the 1 s `stage/` line, `esp_log_level_set("putmedia", ESP_LOG_DEBUG)` for the 1 s
  `upload:` line (buffers/bytes sent, queue depth). Lost buffers are reported at `WARN`
  regardless.

## Troubleshooting

| Problem | Symptoms | Solution |
|---|---|---|
| **PutMedia will not start** | `No AWS credentials...` | `CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n` and all three key fields filled in |
| **Session never opens** | `Session did not open; retrying in 2000 ms (1/3)`, then `after 3 attempts` | credentials, IAM policy, region, or the clock — SigV4 fails on a wrong time. Nothing retries after the third attempt: `stop-putmedia` then `start-putmedia` |
| **Upload falls behind** | `queued` climbing in the `upload:` line | the uplink cannot carry what the encoder produces; lower resolution/fps/bitrate. Load shedding is not implemented / increase CONFIG_PUT_MEDIA_UPLOAD_QUEUE_DEPTH |
| **No frames at all** | `failed to receive video frame`, `enc_fps: out=0` | on UVC, raise `CONFIG_MEDIA_STREAM_UVC_COMPRESSED_FRAME_MAX_KB` — the capture buffer may be too small for a keyframe |
| **Viewer connects, no video off-LAN** | signaling fine, no media | ICE servers must reach the agent before the session is created; raise the `kvs_webrtc` log level and check the candidates gathered are not host-only |

Expected sequence on a healthy `start-putmedia`:

```
I putmedia: No PutMedia session, holding frames
I PUT_MEDIA: Session opened, reopening in 2400 s
I putmedia: Feeding the muxer, opened on keyframe (held 37 frames)
I putmedia: Fragment ack: type=1 timecode=0 errorId=0
```

## How it works

Both paths consume the same encoded H.264 stream from `components/media_stream`, and both
receive frames on the encoder task, so each copies what it needs and returns immediately:

| | source file | in the callback |
|---|---|---|
| **PutMedia** | `main/putmedia.c` | copies into the MKV muxer; the muxer's own task produces clusters, and an upload task sends them |
| **WebRTC** | `components/kvs_webrtc/src/kvs_media.c` | copies onto its own queue; RTP packetisation and sending run on a separate thread |

Capture is reference counted (`video_capture_init/start/stop/deinit`), which is what makes
the two paths independent in any order. Frames are also gated in two useful places: a path
only begins on a key frame, so a decoder sees SPS/PPS first, and PutMedia drops frames
while its session is not live (during the initial connect and every reopen) instead of
paying to mux buffers the session would refuse. `sink-stats` shows both effects —
`gate_held` for the first, `max_us`/`avg_us` for how much of the shared per-frame budget
each path uses.

**Known limitation.** `video_rate_ctrl` is process-global and only the WebRTC path feeds
it, so where it is active, WebRTC backing off for a congested viewer link also throttles
the frame rate the archive records. On an S31 with a UVC camera nothing throttles at all:
the camera owns bitrate and GOP, so `CONFIG_ESP_VIDEO_UVC_H264_BITRATE_BPS`,
`_IFRAME_PERIOD_MS` and the capture resolution are static choices. Details:
[`components/media_stream/README.md`](../../components/media_stream/README.md).

## Next Steps

- **[components/putmedia/README.md](../../components/putmedia/README.md)** — the PutMedia
  components and their API
- **[webrtc_classic](../webrtc_classic/README.md)** — WebRTC on its own
- **[API_USAGE.md](../../API_USAGE.md)** — complete API reference

## License

Apache License 2.0
