#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GITHUB_NVS_NAMESPACE "github_ota"
#define GITHUB_NVS_KEY_OWNER  "owner"
#define GITHUB_NVS_KEY_REPO   "repo"
#define GITHUB_NVS_KEY_TOKEN  "token"
#define GITHUB_NVS_KEY_POLL   "poll_sec"

typedef struct {
    char owner[64];
    char repo[64];
    char token[128];
    int poll_interval_sec;
} github_config_t;

typedef struct {
    char tag_name[32];
    char asset_url[256];
    bool needs_auth;
} github_release_t;

esp_err_t github_config_load_defaults(github_config_t *config);
esp_err_t github_config_save(const github_config_t *config);

esp_err_t github_fetch_latest_release(const github_config_t *config, github_release_t *release);
bool github_release_is_newer(const github_release_t *release);

esp_err_t github_ota_perform(const github_config_t *config, const github_release_t *release);

void github_poller_start(void);
void github_poller_trigger(void);

esp_err_t github_config_api_start(void);

#ifdef __cplusplus
}
#endif
