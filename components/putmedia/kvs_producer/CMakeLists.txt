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

# c_utility is private: mkvapp.c and stream.c use its Lock_*/DList_*/
# mallocAndStrcpy_s, but no public header in include/ exposes those types.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "source"
    PRIV_REQUIRES "esp_timer" "c_utility"
)
