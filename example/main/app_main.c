#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "protocol_examples_common.h"
#include "github_ota.h"

static const char *TAG = "main";

static void ota_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != ESP_HTTPS_OTA_EVENT) {
        return;
    }
    switch (id) {
    case ESP_HTTPS_OTA_START:
        ESP_LOGI(TAG, "OTA started");
        break;
    case ESP_HTTPS_OTA_CONNECTED:
        ESP_LOGI(TAG, "connected to server");
        break;
    case ESP_HTTPS_OTA_FINISH:
        ESP_LOGI(TAG, "OTA finish");
        break;
    case ESP_HTTPS_OTA_ABORT:
        ESP_LOGW(TAG, "OTA abort");
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                               &ota_event_handler, NULL));
    ESP_ERROR_CHECK(example_connect());

    esp_wifi_set_ps(WIFI_PS_NONE);

#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "app valid, rollback cancelled");
        } else {
            ESP_LOGE(TAG, "failed to cancel rollback");
        }
    }
#endif

    ESP_ERROR_CHECK(github_config_api_start());
    github_poller_start();
}
