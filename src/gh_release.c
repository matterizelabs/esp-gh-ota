#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "github_ota.h"
#include "gh_internal.h"

static const char *TAG = "github_ota";

static char s_response_buf[4096];
static int s_response_len;

static esp_err_t gh_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_response_len < sizeof(s_response_buf) - 1) {
        int copy = evt->data_len;
        if (s_response_len + copy >= sizeof(s_response_buf)) {
            copy = sizeof(s_response_buf) - s_response_len - 1;
        }
        memcpy(s_response_buf + s_response_len, evt->data, copy);
        s_response_len += copy;
    }
    return ESP_OK;
}

void gh_set_auth_header(esp_http_client_handle_t client, const char *token)
{
    if (token && token[0] != '\0') {
        char buf[256];
        snprintf(buf, sizeof(buf), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", buf);
    }
}

esp_err_t github_fetch_latest_release(const github_config_t *config, github_release_t *release)
{
    if (config->owner[0] == '\0' || config->repo[0] == '\0') {
        ESP_LOGW(TAG, "GitHub owner/repo not configured");
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest",
             config->owner, config->repo);

    s_response_len = 0;
    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .event_handler = gh_http_event_handler,
        .method = HTTP_METHOD_GET,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_http_client_set_header(client, "User-Agent", "ESP32-GitHub-OTA");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    gh_set_auth_header(client, config->token);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status_code == 404) {
        if (status_code == 404) {
            ESP_LOGW(TAG, "No releases found for %s/%s", config->owner, config->repo);
            err = ESP_ERR_NOT_FOUND;
        } else if (status_code == 403) {
            ESP_LOGW(TAG, "GitHub API rate limit exceeded or access denied");
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "GitHub API request failed: %d", status_code);
        }
        esp_http_client_cleanup(client);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    s_response_buf[s_response_len] = '\0';
    ESP_LOGI(TAG, "API response (%d bytes)", s_response_len);
    esp_http_client_cleanup(client);

    cJSON *root = cJSON_Parse(s_response_buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON (first 128 bytes): %.128s", s_response_buf);
        return ESP_FAIL;
    }

    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    cJSON *assets = cJSON_GetObjectItem(root, "assets");
    cJSON *prerelease = cJSON_GetObjectItem(root, "prerelease");

    if (tag == NULL || !cJSON_IsString(tag)) {
        ESP_LOGE(TAG, "No tag_name in release");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (prerelease && cJSON_IsTrue(prerelease)) {
        ESP_LOGI(TAG, "Skipping pre-release %s", tag->valuestring);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const char *asset_url = NULL;
    if (assets && cJSON_IsArray(assets)) {
        cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItem(asset, "name");
            if (name && cJSON_IsString(name)) {
                const char *an = name->valuestring;
                size_t nl = strlen(an);
                if (nl > 4 && strcasecmp(an + nl - 4, ".bin") == 0) {
                    cJSON *url_item = cJSON_GetObjectItem(asset, "browser_download_url");
                    if (url_item && cJSON_IsString(url_item)) {
                        asset_url = url_item->valuestring;
                        break;
                    }
                }
            }
        }
    }

    if (asset_url == NULL) {
        ESP_LOGW(TAG, "No .bin asset found in release %s", tag->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    gh_str_copy(release->tag_name, tag->valuestring, sizeof(release->tag_name));
    gh_str_copy(release->asset_url, asset_url, sizeof(release->asset_url));
    release->needs_auth = (config->token[0] != '\0');

    ESP_LOGI(TAG, "Latest release: %s, asset: %s, auth: %s",
             release->tag_name, release->asset_url,
             release->needs_auth ? "yes" : "no");
    cJSON_Delete(root);
    return ESP_OK;
}

static int gh_semver_parse(const char *s, int *major, int *minor, int *patch)
{
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    return sscanf(s, "%d.%d.%d", major, minor, patch) == 3 ? 0 : -1;
}

bool github_release_is_newer(const github_release_t *release)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_info;
    if (esp_ota_get_partition_description(running, &running_info) != ESP_OK) {
        return true;
    }

    const char *tag = release->tag_name;
    if (tag[0] == 'v' || tag[0] == 'V') {
        tag++;
    }

    if (strcmp(tag, running_info.version) == 0) {
        ESP_LOGI(TAG, "Running version %s matches release %s, skipping",
                 running_info.version, release->tag_name);
        return false;
    }

    int rM = 0, rm = 0, rp = 0, cM = 0, cm = 0, cp = 0;
    if (gh_semver_parse(release->tag_name, &rM, &rm, &rp) == 0 &&
        gh_semver_parse(running_info.version, &cM, &cm, &cp) == 0) {
        int cmp = (rM != cM) ? (rM - cM) : ((rm != cm) ? (rm - cm) : (rp - cp));
        if (cmp <= 0) {
            ESP_LOGW(TAG, "release %s <= running %s, anti-rollback skips downgrade",
                     release->tag_name, running_info.version);
            return false;
        }
    }

    ESP_LOGI(TAG, "New version available: running=%s, release=%s",
             running_info.version, release->tag_name);
    return true;
}
