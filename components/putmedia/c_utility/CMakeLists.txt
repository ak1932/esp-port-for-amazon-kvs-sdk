cmake_minimum_required(VERSION 3.10)

set(srcs
    "adapters/lock_pthreads.c"
    "src/buffer.c"
    "src/crt_abstractions.c"
    "src/doublylinkedlist.c"
    "src/httpheaders.c"
    "src/map.c"
    "src/strings.c"
)

set(inc_dirs
    "inc"
    "deps/azure-macro-utils-c/inc" 
    "deps/umock-c/inc" 
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${inc_dirs}
)

# This vendored Azure tree predates C23. inc/azure_c_shared_utility/crt_abstractions.h
# typedefs _Bool and bool (both keywords in C23) and #errors out on any
# __STDC_VERSION__ it does not recognise, so it will not compile under the gnu23
# default that ESP-IDF v6.x uses. Pin the language standard rather than patching
# upstream sources. Any component that includes these headers needs the same pin.
target_compile_options(${COMPONENT_LIB} PUBLIC -std=gnu17)
