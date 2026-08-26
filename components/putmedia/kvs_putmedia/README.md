# KVS PutMedia

Streams already-muxed MKV fragments to an Amazon Kinesis Video Streams **video stream**
using the Producer data-plane `PUT_MEDIA` API — a long-lived chunked HTTPS POST. There is
no peer connection and no signaling involved; this is the archival path, not the WebRTC
path.

Derived from
[amazon-kinesis-video-streams-producer-embedded-c](https://github.com/aws-samples/amazon-kinesis-video-streams-producer-embedded-c),
with the REST/HTTP layer reworked to use `esp_http_client` and the MKV generation split
out into the separate [`kvs_producer`](../kvs_producer/) component.

## What it does

| Concern | Handled here |
|---|---|
| Control plane | `DescribeStream`, `CreateStream`, `GetDataEndpoint` |
| Data plane | `PutMedia` — chunked POST, fragment ACK parsing |
| Auth | SigV4 request signing (`x-amz-date`, `x-amz-security-token`, `authorization`) |
| Transport | `esp_http_client` + TLS via `esp_crt_bundle` |
| Buffering | 50-entry queue, buffers deep-copied into PSRAM where available |

It does **not** capture, encode, or mux. The application feeds it `mkv_buf_info_t`
buffers — typically produced by `kvs_producer` — via `put_media_enqueue()`.

## Usage

```c
#include "kvs/put_media.h"

put_media_config_t cfg = {
    .pcHost                   = "s-xxxxxxxx.kinesisvideo.us-east-1.amazonaws.com",
    .pcRegion                 = "us-east-1",
    .pcService                = "kinesisvideo",
    .pcStreamName             = "esp-ingest",
    .kvs_fetch_credentials_cb = fetch_credentials,   /* app supplies AWS credentials */
    .get_mkv_ebml_hdr_func    = get_ebml_hdr,        /* app supplies the MKV EBML header */
    .uDataRetentionHours      = 2,
};

put_media_init(cfg);
put_media_start();
/* ... for each generated MKV buffer ... */
put_media_enqueue(mkv_buf_info);
put_media_stop();
put_media_deinit();
```

Credentials are **pushed in by the application** through `kvs_fetch_credentials_cb` —
the component never fetches them itself. See `include/kvs/put_media.h` for the full API.

## Relationship to the rest of this repo

This component predates its move into the KVS port and still carries its own SigV4
implementation (`source/restful/aws_signer_v4.c`), which duplicates
[`components/credential`](../../credential/). Consolidating the two is tracked in
[KNOWN_ISSUES.md](KNOWN_ISSUES.md); it is a refactor rather than a rename, because the
two trees use incompatible type systems (`STRING_HANDLE`/`BUFFER_HANDLE` here versus
`PCHAR`/`STATUS` in `credential`/`kvs_utils`).
