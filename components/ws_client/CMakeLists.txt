idf_component_register(SRCS "ws_client.c"
                       INCLUDE_DIRS "include"
                       REQUIRES proto esp_websocket_client mbedtls)
