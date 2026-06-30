#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "github_ota.h"
#include "gh_internal.h"

static const char *TAG = "github_ota";

void gh_str_copy(char *dst, const char *src, size_t size)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static void gh_nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t len = size;
    nvs_get_str(handle, key, out, &len);
}

esp_err_t github_config_load_defaults(github_config_t *config)
{
    memset(config, 0, sizeof(*config));

#ifdef CONFIG_ESP_GH_OTA_DEFAULT_OWNER
    if (strlen(CONFIG_ESP_GH_OTA_DEFAULT_OWNER) > 0) {
        gh_str_copy(config->owner, CONFIG_ESP_GH_OTA_DEFAULT_OWNER, sizeof(config->owner));
    }
#endif
#ifdef CONFIG_ESP_GH_OTA_DEFAULT_REPO
    if (strlen(CONFIG_ESP_GH_OTA_DEFAULT_REPO) > 0) {
        gh_str_copy(config->repo, CONFIG_ESP_GH_OTA_DEFAULT_REPO, sizeof(config->repo));
    }
#endif
    config->poll_interval_sec = CONFIG_ESP_GH_OTA_POLL_INTERVAL_SEC;

    nvs_handle_t handle;
    if (nvs_open(GITHUB_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }

    gh_nvs_get_str(handle, GITHUB_NVS_KEY_OWNER, config->owner, sizeof(config->owner));
    gh_nvs_get_str(handle, GITHUB_NVS_KEY_REPO, config->repo, sizeof(config->repo));
    gh_nvs_get_str(handle, GITHUB_NVS_KEY_TOKEN, config->token, sizeof(config->token));

    uint16_t poll = 0;
    if (nvs_get_u16(handle, GITHUB_NVS_KEY_POLL, &poll) == ESP_OK && poll > 0) {
        config->poll_interval_sec = poll;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t github_config_save(const github_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GITHUB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, GITHUB_NVS_KEY_OWNER, config->owner);
    nvs_set_str(handle, GITHUB_NVS_KEY_REPO, config->repo);
    nvs_set_str(handle, GITHUB_NVS_KEY_TOKEN, config->token);
    nvs_set_u16(handle, GITHUB_NVS_KEY_POLL, (uint16_t)config->poll_interval_sec);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}
