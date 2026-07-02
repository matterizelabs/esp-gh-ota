#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "github_ota.h"
#include "gh_internal.h"

#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
#include "esp_efuse.h"
#endif

static const char *TAG = "github_ota";

typedef struct {
    const char *token;
} gh_ota_ctx_t;

#define GH_OTA_URL_SIZE 256

#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
#define GH_OTA_RES_NVS         "gh_ota_res"
#define GH_OTA_RES_KEY_WR_LEN  "wr_len"
#define GH_OTA_RES_KEY_URL     "url"

// Min bytes between NVS checkpoints; perform() returns ~1 KB/call, so committing every call thrashes flash.
#define GH_OTA_RES_SAVE_INTERVAL (64 * 1024)

static esp_err_t gh_ota_res_get_len(nvs_handle_t handle, const char *url, uint32_t *wr_len)
{
    char saved_url[GH_OTA_URL_SIZE] = {0};
    size_t url_len = sizeof(saved_url);

    *wr_len = 0;
    esp_err_t err = nvs_get_str(handle, GH_OTA_RES_KEY_URL, saved_url, &url_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "resumption read url failed: %s", esp_err_to_name(err));
        return err;
    }
    if (strcmp(url, saved_url) != 0) {
        ESP_LOGD(TAG, "saved URL differs, restarting OTA from beginning");
        return ESP_ERR_INVALID_STATE;
    }

    // Exact byte count; a stale u16 from an older build gives TYPE_MISMATCH here and safely restarts from 0.
    err = nvs_get_u32(handle, GH_OTA_RES_KEY_WR_LEN, wr_len);
    if (err != ESP_OK) {
        *wr_len = 0;
        return err;
    }
    return ESP_OK;
}

static esp_err_t gh_ota_res_save(nvs_handle_t handle, int wr_len, const char *url)
{
    ESP_RETURN_ON_ERROR(nvs_set_u32(handle, GH_OTA_RES_KEY_WR_LEN, (uint32_t)wr_len),
                        TAG, "set wr_len failed");
    if (wr_len) {
        char saved_url[GH_OTA_URL_SIZE] = {0};
        size_t url_len = sizeof(saved_url);
        esp_err_t err = nvs_get_str(handle, GH_OTA_RES_KEY_URL, saved_url, &url_len);
        if (err == ESP_ERR_NVS_NOT_FOUND || strcmp(saved_url, url) != 0) {
            ESP_RETURN_ON_ERROR(nvs_set_str(handle, GH_OTA_RES_KEY_URL, url),
                                TAG, "set url failed");
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "resumption read url failed");
            return err;
        }
    }
    ESP_RETURN_ON_ERROR(nvs_commit(handle), TAG, "commit failed");
    return ESP_OK;
}

static esp_err_t gh_ota_res_cleanup(nvs_handle_t handle)
{
    esp_err_t ret;
    ESP_GOTO_ON_ERROR(nvs_erase_all(handle), err, TAG, "erase failed");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), err, TAG, "commit failed");
    ret = ESP_OK;
err:
    nvs_close(handle);
    return ret;
}
#endif  /* CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION */

#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
static esp_err_t gh_ota_validate_header(const esp_app_desc_t *new_info)
{
    const uint32_t hw_sec_version = esp_efuse_read_secure_version();
    if (new_info->secure_version < hw_sec_version) {
        ESP_LOGW(TAG, "new secure_version %u < hw %u, anti-rollback rejects",
                 (unsigned)new_info->secure_version, (unsigned)hw_sec_version);
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

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

#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
    nvs_handle_t res_handle;
    esp_err_t nerr = nvs_open(GH_OTA_RES_NVS, NVS_READWRITE, &res_handle);
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "resumption NVS open failed: %s", esp_err_to_name(nerr));
        return nerr;
    }
    uint32_t ota_wr_len = 0;
    gh_ota_res_get_len(res_handle, release->asset_url, &ota_wr_len);
    ESP_LOGD(TAG, "resuming from %lu bytes", (unsigned long)ota_wr_len);
#endif

    esp_http_client_config_t http_config = {
        .url = release->asset_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_ESP_GH_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
        .buffer_size = CONFIG_ESP_GH_OTA_BUF_SIZE,
        .buffer_size_tx = 1024,
        .user_data = &ota_ctx,
#ifdef CONFIG_ESP_GH_OTA_ENABLE_PARTIAL_DOWNLOAD
        .save_client_session = true,
#endif
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
        .http_client_init_cb = gh_ota_http_init_cb,
#ifdef CONFIG_ESP_GH_OTA_ENABLE_PARTIAL_DOWNLOAD
        .partial_http_download = true,
        .max_http_request_size = CONFIG_ESP_GH_OTA_PARTIAL_REQUEST_SIZE,
#endif
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
        .ota_resumption = true,
        .ota_image_bytes_written = (int)ota_wr_len,
#endif
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
        nvs_close(res_handle);
#endif
        return err;
    }

#if defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
    {
        esp_app_desc_t app_desc = {};
        err = esp_https_ota_get_img_desc(ota_handle, &app_desc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "get image desc failed: %s", esp_err_to_name(err));
            esp_https_ota_abort(ota_handle);
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
            nvs_close(res_handle);
#endif
            return err;
        }
        err = gh_ota_validate_header(&app_desc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "image header rejected (anti-rollback)");
            esp_https_ota_abort(ota_handle);
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
            nvs_close(res_handle);
#endif
            return err;
        }
    }
#endif

#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
    int last_saved_len = (int)ota_wr_len;
#endif
    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
        int written = esp_https_ota_get_image_len_read(ota_handle);
        if (written - last_saved_len >= GH_OTA_RES_SAVE_INTERVAL) {
            esp_err_t serr = gh_ota_res_save(res_handle, written, release->asset_url);
            if (serr != ESP_OK) {
                ESP_LOGE(TAG, "resumption save failed: %s", esp_err_to_name(serr));
            } else {
                last_saved_len = written;
            }
        }
#endif
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "OTA download incomplete");
        esp_https_ota_abort(ota_handle);
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
        nvs_close(res_handle);
#endif
        return ESP_FAIL;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
        nvs_close(res_handle);
#endif
        return err;
    }

#ifdef CONFIG_ESP_GH_OTA_ENABLE_RESUMPTION
    gh_ota_res_cleanup(res_handle);
#endif

    ESP_LOGI(TAG, "OTA successful, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
