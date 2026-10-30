# credential

AWS credential providers for the KVS WebRTC SDK. Each provider constructs a
`PAwsCredentialProvider` that the signaling/peer layers use to sign AWS requests, so the rest of
the stack is agnostic to *where* credentials come from.

## Providers

| Provider | Constructor | Use |
|----------|-------------|-----|
| **Static** | `createStaticCredentialProvider()` | Access key + secret (+ optional session token) supplied directly. Simplest; good for development. |
| **IoT Core** | `createIotCredentialProvider()` | Exchanges an IoT Core device cert/key for temporary credentials via a role alias (mutual-TLS). |
| **File** | `file_credential_provider_create()` | Reads credentials from a file. |
| **Callback** | `createCallbackCredentialProvider()` | Your `CredentialFetchCallback` returns credentials on demand — used for dynamic renewal (e.g. ESP RainMaker security tokens). |

Each has a matching `free*` function. The mode is normally selected from menuconfig — see
[docs/building.md → AWS credentials](../../docs/building.md#aws-credentials).

### Sharing a provider

One provider can serve several KVS clients, so credentials are fetched and refreshed once for all
of them. The credentials a provider hands out stay valid until its refresh after next, which is
enough for a user that takes them just before signing a request. A user that keeps them longer
reads through `createCredentialProviderView()`: the view hands out its own copy, valid until the
view's own get after next. Signaling does this with a provider passed in
`kvs_signaling_config_t.pCredentialProvider`.

## Internals

Also provides the AWS SigV4 signing helper (`aws_signer_v4.c`) used by the providers.

- **Requires:** `kvs_utils`, `mbedtls`, `esp_http_client`.

## Origin

Ported from the [Amazon Kinesis Video Streams PIC](https://github.com/awslabs/amazon-kinesis-video-streams-pic) credential providers and adapted for ESP-IDF (ESP-TLS / esp_http_client transport).
