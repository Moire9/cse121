idf_component_register(SRCS "main.c"
					PRIV_REQUIRES esp-tls esp_wifi nvs_flash esp_http_client esp_driver_i2c
                    INCLUDE_DIRS ".")
