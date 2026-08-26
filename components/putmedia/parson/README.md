# parson

Vendored copy of [kgabis/parson](https://github.com/kgabis/parson) — a small C89 JSON
parser and serializer. MIT licensed; see [LICENSE](LICENSE).

**Version 1.4.0**, per `PARSON_VERSION_STRING` in `include/parson.h`. Vendored as plain
files, not a submodule.

Used by [`kvs_putmedia`](../kvs_putmedia/) to parse KVS control-plane responses
(`source/misc/json_helper.c`, `source/restful/kvs/restapi_kvs.c`).

> This repository also carries `espressif/jsmn` and `espressif/cjson` for the WebRTC
> path. parson is kept separate rather than folded into either: jsmn is a token scanner
> with no DOM, and porting the PutMedia REST layer to cJSON is a rewrite of its response
> handling, not a swap.
