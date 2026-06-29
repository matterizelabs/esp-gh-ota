# ESP GitHub DFU

ESP32 device firmware update via GitHub Releases. Polls the GitHub API for new releases, downloads and flashes new firmware automatically.

## Setup

```bash
cp sdkconfig.secrets.example sdkconfig.secrets
# edit sdkconfig.secrets — fill in WiFi SSID and password
idf.py set-target esp32
idf.py build flash monitor
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

Only strict `vX.Y.Z` tags create releases. Tags like `v1.0.0-rc1` are skipped. The GitHub Action builds with `PROJECT_VER` set to the tag (minus `v`), creates a release, and attaches the `.bin` asset. The device polls every 5 minutes and OTA-updates when a newer version is detected.

## Build

```bash
idf.py build -DPROJECT_VER="1.0.0"
```

## Structure

| File | Purpose |
|---|---|
| `main/github_ota.c` | GitHub API poller, NVS config, HTTP config API, OTA download |
| `main/advanced_https_ota_example.c` | App entry point, WiFi connect |
| `server_certs/github_fullchain.pem` | TLS certificate chain for api.github.com |
| `sdkconfig.ghdfu` | Public build defaults |
| `sdkconfig.secrets` | Gitignored — WiFi credentials |
