idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_gh_ota
                                  protocol_examples_common
                                  nvs_flash
                                  esp_netif
                                  esp_event
                                  esp_wifi
                                  app_update
                                  esp_https_ota)
