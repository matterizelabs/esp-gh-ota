#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

// Heap cap for the buffered API response; grow on demand up to this instead of silently truncating.
#define GH_RESP_MAX_SIZE (64 * 1024)

static char *s_response_buf;
static int s_response_len;
static int s_response_cap;
static bool s_response_truncated;

static void gh_response_reset(void)
{
    free(s_response_buf);
    s_response_buf = NULL;
    s_response_len = 0;
    s_response_cap = 0;
    s_response_truncated = false;
}

static esp_err_t gh_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || s_response_truncated) {
        return ESP_OK;
    }

    int need = s_response_len + evt->data_len + 1;
    if (need > s_response_cap) {
        int new_cap = s_response_cap ? s_response_cap : 1024;
        while (new_cap < need) {
            new_cap *= 2;
        }
        if (new_cap > GH_RESP_MAX_SIZE) {
            new_cap = GH_RESP_MAX_SIZE;
        }
        if (need > new_cap) {
            ESP_LOGE(TAG, "API response exceeds %d bytes, aborting", GH_RESP_MAX_SIZE);
            s_response_truncated = true;
            return ESP_OK;
        }
        char *grown = realloc(s_response_buf, new_cap);
        if (grown == NULL) {
            ESP_LOGE(TAG, "API response buffer alloc failed");
            s_response_truncated = true;
            return ESP_OK;
        }
        s_response_buf = grown;
        s_response_cap = new_cap;
    }

    memcpy(s_response_buf + s_response_len, evt->data, evt->data_len);
    s_response_len += evt->data_len;
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

    gh_response_reset();
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
        gh_response_reset();
        return err == ESP_OK ? ESP_FAIL : err;
    }

    esp_http_client_cleanup(client);

    if (s_response_truncated || s_response_buf == NULL) {
        ESP_LOGE(TAG, "API response incomplete, cannot parse");
        gh_response_reset();
        return ESP_FAIL;
    }

    s_response_buf[s_response_len] = '\0';
    ESP_LOGI(TAG, "API response (%d bytes)", s_response_len);

    cJSON *root = cJSON_Parse(s_response_buf);
    gh_response_reset();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
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
