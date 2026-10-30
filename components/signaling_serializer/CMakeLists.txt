set(srcs
    "src/signaling_serializer.c"
    "src/json_serializer.c"
    "src/protobuf_serializer.c"
    "src/raw_serializer.c"
    "proto/signaling.pb-c.c"
)

if (LINUX_BUILD)
    # Use local protobuf-c sources from ESP components
    set(PROTOBUF_C_SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../components/esp_hosted/common/protobuf-c/protobuf-c/protobuf-c.c"
        # jsmn, as kvs_utils builds it
        "${CMAKE_CURRENT_SOURCE_DIR}/../kvs_utils/src/jsmn.c"
    )

    set(PROTOBUF_C_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../components/esp_hosted/common/protobuf-c"
    )

    add_library(signaling_serializer STATIC ${srcs} ${PROTOBUF_C_SRCS})

    target_include_directories(signaling_serializer
        PUBLIC
        "include"
        PRIVATE
        "proto"
        # Include protobuf-c headers
        ${PROTOBUF_C_INCLUDE_DIRS}
        # jsmn.h
        "${CMAKE_CURRENT_SOURCE_DIR}/../kvs_utils/include"
    )

    # Add a definition for Linux builds
    target_compile_definitions(signaling_serializer PRIVATE KVS_PLAT_LINUX_UNIX)
else()
    # ESP-IDF specific build
    idf_component_register(
        SRCS ${srcs}
        PRIV_INCLUDE_DIRS "proto"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES protobuf-c mbedtls kvs_utils
    )
endif()
