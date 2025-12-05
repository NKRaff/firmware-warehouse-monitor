idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_http_server esp_driver_gpio
                    INCLUDE_DIRS ".")