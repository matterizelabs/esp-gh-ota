#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "github_ota.h"
#include "gh_internal.h"

static const char *TAG = "github_ota";

static void github_poller_task(void *arg)
{
    ESP_LOGI(TAG, "GitHub poller started");

    while (1) {
        github_config_t config;
        github_config_load_defaults(&config);

        if (config.owner[0] != '\0' && config.repo[0] != '\0') {
            github_release_t release;
            if (github_fetch_latest_release(&config, &release) == ESP_OK) {
                if (github_release_is_newer(&release)) {
                    ESP_LOGI(TAG, "New release found, starting OTA...");
                    github_ota_perform(&config, &release);
                }
            }
        } else {
            ESP_LOGD(TAG, "No GitHub repo configured, skipping check");
        }

        int delay_ms = config.poll_interval_sec * 1000;
        if (delay_ms < 60000) {
            delay_ms = 60000;
        }
        xEventGroupWaitBits(s_gh_event_group, GH_BIT_CONFIG_CHANGED,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(delay_ms));
    }
}

void github_poller_start(void)
{
    if (s_gh_event_group == NULL) {
        s_gh_event_group = xEventGroupCreate();
        if (s_gh_event_group == NULL) {
            ESP_LOGE(TAG, "failed to create event group");
            return;
        }
    }
    xTaskCreate(&github_poller_task, "github_poller", 1024 * 10, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void github_poller_trigger(void)
{
    if (s_gh_event_group) {
        xEventGroupSetBits(s_gh_event_group, GH_BIT_CONFIG_CHANGED);
    }
}
