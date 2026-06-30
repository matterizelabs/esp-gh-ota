idf_component_register(SRC_DIRS "src"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES esp_http_client app_update esp_https_ota
                                  nvs_flash esp_http_server mbedtls)
