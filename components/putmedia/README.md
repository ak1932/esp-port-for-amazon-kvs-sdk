# putmedia

The **PutMedia** ingestion path: capture-side MKV muxing plus a KVS Producer data-plane
client that streams the resulting fragments straight into a Kinesis Video Streams *video
stream* over a long-lived chunked HTTPS POST.

This is independent of the WebRTC stack. There is no peer connection and no signaling —
where WebRTC media-storage ingestion archives footage by making the KVS backend the
*viewer*, PutMedia uploads to the stream directly.

| Component | Purpose |
|-----------|---------|
| **[kvs_putmedia](kvs_putmedia/)** | PutMedia client — SigV4 signing, chunked upload, fragment ACKs, `DescribeStream`/`CreateStream`/`GetDataEndpoint` |
| **[kvs_producer](kvs_producer/)** | MKV/EBML generation, H.264 NALU and SPS parsing, ESP-IDF port layer |

Consumers would reference these as `${KVS_SDK_PATH}/components/putmedia/<name>`.

## Status

> **Best on ESP-IDF v6.1+, builds on v5.4 and later. Not in the CI example matrix.**
>
> `kvs_putmedia` prefers `esp_http_client_chunk_write_begin()` /
> `esp_http_client_chunk_write_end()`, which first ship in v6.1 — they landed after the
> v6.0.2 tag. Older IDFs take a hand-rolled chunk-framing path instead, so the component
> still builds on v5.4.x and v5.5.x.
>
> Building on IDF v6.x also needed two mbedtls 4.x fixes: SigV4 runs off PSA Crypto
> rather than the SHA-256 and HMAC APIs that became private.
>
> [`examples/kvs_combined`](../../examples/kvs_combined/) exercises this path together
> with WebRTC. It is not in the CI example matrix, so these components are not built by
> CI.

## Configuration

`kvs_putmedia` exposes one Kconfig option, under **Component config → AWS KVS PutMedia**:

| Option | Default | |
|---|---|---|
| `CONFIG_PUT_MEDIA_UPLOAD_QUEUE_DEPTH` | 128, **1024 on ESP32-S31** | Depth of the queue between `put_media_enqueue()` and the upload task, in *buffers* — the muxer emits two per frame. |

Two things make the depth worth thinking about rather than defaulting blindly. A buffer
dropped for want of a slot corrupts the whole cluster it belonged to, so overflow costs
every frame up to the next key frame. And `xQueueCreate()` allocates from internal RAM, so
the depth costs `depth * 24` bytes up front — about 24 KB at 1024 — before any payload.

ESP32-S31 gets the larger default because it runs the camera's H.264 through untouched, and
on that path how much can be throttled depends on the camera. Frame rate never can — a
passthrough frame is already encoded, so dropping one corrupts the stream to the next IDR.
Bitrate can, *if* the camera exposes a usable UVC H.264 extension unit; `media_stream`
drives it there (see `components/media_stream/README.md`). On a camera without one nothing
can be throttled at all and this queue is the only slack in the system, which is the case
the large default is sized for. Targets that own the encoder shed load upstream instead.

`put_media_config_t.uUploadQueueDepth` overrides it per instance; zero selects the Kconfig
value.

## Provenance

`kvs_putmedia` and `kvs_producer` are derived from
[amazon-kinesis-video-streams-producer-embedded-c](https://github.com/aws-samples/amazon-kinesis-video-streams-producer-embedded-c)
(Apache-2.0); upstream's `kvsapp` was split into MKV generation (`kvs_producer`) and the
PutMedia flow (`kvs_putmedia`), and the REST/HTTP layer was reworked onto
`esp_http_client`.

Upstream is written against `parson` and a subset of the Azure C shared utility, both of
which were originally vendored here. Neither survives: JSON now goes through
`espressif/cjson`, which the firmware already links, and the Azure pieces that were
actually reachable - a header collection, a growable string, two mutexes and two
intrusive lists - are replaced by a small local header list, `pthread_mutex_t`,
`sys/queue.h` TAILQs and `snprintf`.
