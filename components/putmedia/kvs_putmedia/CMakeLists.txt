cmake_minimum_required(VERSION 3.10)

set(srcs
    "source/misc/json_helper.c"
    "source/net/http_helper.c"
    "source/restful/kvs/restapi_kvs.c"
    "source/restful/aws_signer_v4.c"
    "source/restful/iot/iot_credential_provider.c"
    "source/put_media/put_media.c"
)

# REQUIRES vs PRIV_REQUIRES: only kvs_producer is public - include/kvs/put_media.h
# pulls in kvs/mkvapp.h for mkv_buf_info_t. cJSON and c_utility are used solely
# from source/ (json_helper.c, http_helper.c, restapi_kvs.c, aws_signer_v4.c), so
# they stay private. esp-tls is what provides esp_crt_bundle.h, used by
# http_helper.c for *.amazonaws.com server verification; it was previously
# resolved only by accident through another component's transitive requires.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "source"
    REQUIRES "kvs_producer"
    PRIV_REQUIRES "esp_http_client" "esp-tls" "mbedtls" "esp_timer" "espressif__cjson" "c_utility"
)
