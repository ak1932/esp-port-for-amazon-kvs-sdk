cmake_minimum_required(VERSION 3.10)

set(srcs
    "src/parson.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
)
