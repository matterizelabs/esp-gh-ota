# github_dfu example

Reference app for the [`matterizelabs/esp_gh_ota`](https://components.espressif.com/components/matterizelabs/esp_gh_ota) component. Connects to WiFi, starts the config HTTP API, and polls GitHub Releases for firmware updates; flashes a new release when one is detected.

## Build & flash this example

```bash
cp sdkconfig.secrets.example sdkconfig.secrets
# edit sdkconfig.secrets — fill in WiFi SSID and password
idf.py set-target esp32
idf.py build flash monitor
```

`sdkconfig.defaults` enables partial HTTP download, OTA resumption, and
anti-rollback. Anti-rollback burns eFuses one-way — comment out
`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` for dev devices.

## Wire it into your own project

Start from the ESP-IDF `hello-world` (or any template) and add three things.

**1. Declare the dependency** — `main/idf_component.yml`:

```yaml
dependencies:
  matterizelabs/esp_gh_ota: "^1.0.1"
```

(plus your WiFi-connect helper if you use one; this example uses
`protocol_examples_common` from the IDF tree).

**2. Require the component** — `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_gh_ota
                                  nvs_flash
                                  esp_netif
                                  esp_event
                                  esp_wifi
                                  app_update
                                  esp_https_ota)
```

**3. Bootwire in `app_main`** — `main/app_main.c`:

```c
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "github_ota.h"

static const char *TAG = "main";

static void ota_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base != ESP_HTTPS_OTA_EVENT) return;
    switch (id) {
    case ESP_HTTPS_OTA_START:    ESP_LOGI(TAG, "OTA started");      break;
    case ESP_HTTPS_OTA_CONNECTED:ESP_LOGI(TAG, "connected");       break;
    case ESP_HTTPS_OTA_FINISH:   ESP_LOGI(TAG, "OTA finish");       break;
    case ESP_HTTPS_OTA_ABORT:    ESP_LOGW(TAG, "OTA abort");        break;
    default: break;
    }
}

void app_main(void)
{
    /* 1. NVS (component stores config + resumption state here) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 2. netif + default event loop + OTA event logging */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(ESP_HTTPS_OTA_EVENT, ESP_EVENT_ANY_ID,
                                               &ota_event_handler, NULL));

    /* 3. WiFi — use your own connect here; example_connect() is from
     *    protocol_examples_common. Max throughput for OTA. */
    ESP_ERROR_CHECK(example_connect());
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 4. (optional) cancel a pending rollback from a previous OTA */
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
#endif

    /* 5. Start the /api/config HTTP endpoint + the GitHub poller task */
    ESP_ERROR_CHECK(github_config_api_start());
    github_poller_start();
}
```

That's the whole integration: `github_config_api_start()` brings up the
runtime config endpoint; `github_poller_start()` spawns the task that polls
GitHub and flashes new releases.

## Runtime config

Defaults come from `CONFIG_ESP_GH_OTA_DEFAULT_OWNER` / `CONFIG_ESP_GH_OTA_DEFAULT_REPO`
(or NVS if previously saved). Override at runtime over HTTP:

```bash
# point the device at a repo + (optional) private-repo token
curl -X POST http://<esp-ip>/api/config -H 'Content-Type: application/json' \
  -d '{"owner":"matterizelabs","repo":"esp-github-dfu","token":"ghp_xxx"}'

# inspect current config + running firmware version
curl http://<esp-ip>/api/config
```

A config change wakes the poller immediately (no need to wait for the next
interval).

## Release workflow

Push a strict `vX.Y.Z` tag to the firmware repo to trigger its CI build +
GitHub Release with the `.bin` asset attached. The device polls every 5
minutes (default) and OTA-updates when a newer version is detected.
