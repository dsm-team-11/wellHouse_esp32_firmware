idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_core wifi_mgr mqtt_link spi_link evt_queue proto
                                nvs_flash esp_netif esp_event driver)
