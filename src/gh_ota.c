#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "freertos/task.h"
#include "github_ota.h"
#include "gh_internal.h"

static const char *TAG = "github_ota";

typedef struct {
    const char *token;
} gh_ota_ctx_t;

static esp_err_t gh_ota_http_init_cb(esp_http_client_handle_t client)
{
    void *ud = NULL;
    esp_http_client_get_user_data(client, &ud);
    if (ud) {
        gh_set_auth_header(client, ((gh_ota_ctx_t *)ud)->token);
    }
    return ESP_OK;
}

esp_err_t github_ota_perform(const github_config_t *config, const github_release_t *release)
{
    ESP_LOGI(TAG, "Starting OTA from %s", release->asset_url);

    gh_ota_ctx_t ota_ctx = {
        .token = release->needs_auth ? config->token : NULL,
    };

    esp_http_client_config_t http_config = {
        .url = release->asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_ESP_GH_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
        .buffer_size = CONFIG_ESP_GH_OTA_BUF_SIZE,
        .buffer_size_tx = 1024,
        .user_data = &ota_ctx,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .http_client_init_cb = gh_ota_http_init_cb,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        return err;
    }

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA download incomplete");
        esp_https_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA successful, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
