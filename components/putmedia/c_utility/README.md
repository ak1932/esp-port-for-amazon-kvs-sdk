# c_utility

A hand-cut subset of
[Azure/azure-c-shared-utility](https://github.com/Azure/azure-c-shared-utility), the
portable C helper library the upstream KVS producer sources are written against. MIT
licensed; see [LICENSE](LICENSE).

**No upstream commit or tag is recorded** in the vendored tree — there is no VERSION
file, no submodule pin, and no `.git`. Vendored as plain files.

## What is included

| API | Files |
|---|---|
| `STRING_HANDLE` | `src/strings.c` |
| `BUFFER_HANDLE` | `src/buffer.c` |
| `MAP_HANDLE` | `src/map.c` |
| `HTTP_HEADERS_HANDLE` | `src/httpheaders.c` |
| `DLIST_ENTRY` | `src/doublylinkedlist.c` |
| `mallocAndStrcpy_s` etc. | `src/crt_abstractions.c` |
| `LOCK_HANDLE` | `adapters/lock_pthreads.c` |

Plus two of upstream's own vendored dependencies, headers only:
`deps/azure-macro-utils-c` (including the 22k-line generated `macro_utils_generated.h`,
which the retained headers require) and `deps/umock-c`.

Consumed by both [`kvs_putmedia`](../kvs_putmedia/) and
[`kvs_producer`](../kvs_producer/).

> `components/kvs_utils` covers similar ground (linked lists, mutexes, string helpers)
> but with an entirely different API shape — `PDoubleList`/`STATUS`/`PCHAR` versus
> `DLIST_ENTRY`/`STRING_HANDLE`. It also has no `HTTPHeaders_*` equivalent. Replacing
> c_utility would mean rewriting the imported sources, so it is carried as-is.
