#pragma once

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GH_BIT_CONFIG_CHANGED (1 << 0)

extern EventGroupHandle_t s_gh_event_group;

void gh_str_copy(char *dst, const char *src, size_t size);
void gh_set_auth_header(esp_http_client_handle_t client, const char *token);

#ifdef __cplusplus
}
#endif
