# KVS Producer (MKV generation)

MKV/EBML muxing for the KVS PutMedia path: takes H.264 (and audio) frames and emits the
EBML header, segment and cluster headers, and block data that
[`kvs_putmedia`](../kvs_putmedia/) uploads.

Derived from
[amazon-kinesis-video-streams-producer-embedded-c](https://github.com/aws-samples/amazon-kinesis-video-streams-producer-embedded-c).
The upstream `kvsapp` was split: MKV generation was extracted into `mkvapp` (this
component) and the PutMedia flow was removed from it and now lives in `kvs_putmedia`.

This is pure muxing and codec parsing — no camera, encoder, network, or TLS. The only
ESP-IDF–specific file is the port layer, `source/port/port_esp32.c`.

## Contents

| Area | Files |
|---|---|
| MKV/EBML generation | `source/mkv/mkv_generator.c` |
| Frame orchestration | `source/app/mkvapp.c` |
| In-memory frame store | `source/stream/stream.c` |
| H.264 NALU + SPS parsing | `source/codec/nalu.c`, `source/codec/sps_decode.c` |
| Platform port | `source/port/port_esp32.c` (time, ISO-8601, sleep, RNG) |
| Allocator shim | `source/os/allocator.c` (`kvsMalloc` and friends) |

Public API: `include/kvs/mkvapp.h` — `mkv_app_init()`, `mkv_app_start()`,
`mkv_app_add_frame()`, `mkv_app_emit_ebml_header()`, and the
`on_mkv_generated_callback_t` that hands finished buffers to the application.

## Notes

- `source/os/pool_allocator.c` is **not** compiled and there are no `-Wl,--wrap=` flags
  anywhere, so the `__wrap_kvsMalloc` family in `allocator.c` is inert and
  `kvsMalloc == malloc`. Enabling the pool allocator would additionally require a TLSF
  component, which this repository does not have. `include/kvs/pool_allocator.h` is kept
  because `allocator.c` includes it.
- Large allocations use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, so a target without
  PSRAM will fail those allocations rather than falling back to internal RAM.
