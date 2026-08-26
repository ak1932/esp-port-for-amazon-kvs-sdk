cmake_minimum_required(VERSION 3.10)

set(srcs
    "source/app/mkvapp.c"
    "source/codec/nalu.c"
    "source/codec/sps_decode.c"
    "source/mkv/mkv_generator.c"
    "source/os/allocator.c"
    "source/stream/stream.c"

    "source/port/port_esp32.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "source"
    PRIV_REQUIRES "esp_timer" "pthread"
)
