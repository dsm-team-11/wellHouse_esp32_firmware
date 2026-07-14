idf_component_register(SRCS "app_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES proto ws_client spi_link evt_queue wifi_mgr esp_timer)
