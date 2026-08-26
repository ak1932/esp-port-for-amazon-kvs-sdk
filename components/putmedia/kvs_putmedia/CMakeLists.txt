cmake_minimum_required(VERSION 3.10)

set(srcs
    "source/misc/json_helper.c"
    "source/net/http_helper.c"
    "source/net/http_parser_adapter_llhttp.c"
    "source/net/netio.c"
    "source/restful/iot/iot_credential_provider.c"
    "source/restful/kvs/restapi_kvs.c"
    "source/restful/aws_signer_v4.c"
)

# parson and c_utility are used solely from source/ (json_helper.c, http_helper.c,
# restapi_kvs.c, aws_signer_v4.c), so they stay private; only kvs_producer is
# public, because include/kvs/restapi.h pulls in its types.
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "source"
    REQUIRES "kvs_producer"
    PRIV_REQUIRES "esp_http_client" "mbedtls" "esp_timer" "parson" "c_utility"
)
