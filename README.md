# esp-gh-ota

ESP-IDF component for firmware updates via GitHub Releases. Polls the GitHub
API for new releases, downloads the `.bin` asset, and flashes it. Runtime
config (owner/repo/token/poll interval) is stored in NVS and editable over a
local HTTP API.

Features:
- **Partial HTTP download** (`ESP_GH_OTA_ENABLE_PARTIAL_DOWNLOAD`): image is
  fetched over multiple HTTP Range requests.
- **OTA resumption** (`ESP_GH_OTA_ENABLE_RESUMPTION`): an interrupted OTA
  resumes from the last checkpoint after a reboot (bytes written + asset URL
  stored in NVS).
- **Anti-rollback**: software check rejects release tags older than the running
  version; hardware check (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`) rejects
  images with `secure_version` below the eFuse value.

## Layout

| Path | Purpose |
|---|---|
| `include/github_ota.h` | Public API |
| `src/gh_config.c` | NVS load/save + Kconfig defaults |
| `src/gh_release.c` | GitHub API fetch + version compare |
| `src/gh_ota.c` | HTTPS OTA perform |
| `src/gh_http_api.c` | HTTP config API (`/api/config`) |
| `src/gh_poller.c` | Poller task + trigger |
| `Kconfig.projbuild` | `ESP_GH_OTA_*` options |
| `examples/github_dfu/` | Reference app wiring WiFi + the component |

## Use in your project

```bash
idf.py add-dependency matterizelabs/esp_gh_ota
```

Then in `app_main` (after `nvs_flash_init`, netif, and WiFi connect):

```c
#include "github_ota.h"
github_config_api_start();
github_poller_start();
```

## Runtime config

```bash
curl -X POST http://<esp-ip>/api/config -H 'Content-Type: application/json' \
  -d '{"owner":"myorg","repo":"esp-github-dfu"}'

curl http://<esp-ip>/api/config
```

For private repos, add `"token":"ghp_xxx"`.

## Release workflow

Push a semver git tag to trigger CI build and release:

```bash
git tag v1.0.0 && git push origin v1.0.0
```

Only strict `vX.Y.Z` tags create releases. Tags like `v1.0.0-rc1` are skipped.
The device polls every 5 minutes and OTA-updates when a newer version is
detected.
