# github_dfu example

Reference app for the [`matterizelabs/esp_gh_ota`](https://components.espressif.com/components/matterizelabs/esp_gh_ota) component. Connects to WiFi, starts the config HTTP API, and polls GitHub Releases for firmware updates; flashes a new release when one is detected.

## Build & flash

```bash
cp sdkconfig.secrets.example sdkconfig.secrets
# edit sdkconfig.secrets — fill in WiFi SSID and password
idf.py set-target esp32
idf.py build flash monitor
```

`sdkconfig.defaults` enables partial HTTP download, OTA resumption, and
anti-rollback. Anti-rollback burns eFuses one-way — comment out
`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y` for dev devices.

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
