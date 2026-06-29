#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "github_ota.h"

static const char *TAG = "github_ota";
static EventGroupHandle_t s_gh_event_group;
static char s_response_buf[4096];
static int s_response_len;
#define GH_BIT_CONFIG_CHANGED  (1 << 0)

typedef struct {
    const char *token;
} gh_ota_ctx_t;

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

static void gh_set_auth_header(esp_http_client_handle_t client, const char *token)
{
    if (token && token[0] != '\0') {
        char buf[256];
        snprintf(buf, sizeof(buf), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", buf);
    }
}

static void gh_nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t len = size;
    nvs_get_str(handle, key, out, &len);
}

esp_err_t github_config_load_defaults(github_config_t *config)
{
    memset(config, 0, sizeof(*config));

#ifdef CONFIG_EXAMPLE_GITHUB_DEFAULT_OWNER
    if (strlen(CONFIG_EXAMPLE_GITHUB_DEFAULT_OWNER) > 0) {
        strncpy(config->owner, CONFIG_EXAMPLE_GITHUB_DEFAULT_OWNER, sizeof(config->owner) - 1);
    }
#endif
#ifdef CONFIG_EXAMPLE_GITHUB_DEFAULT_REPO
    if (strlen(CONFIG_EXAMPLE_GITHUB_DEFAULT_REPO) > 0) {
        strncpy(config->repo, CONFIG_EXAMPLE_GITHUB_DEFAULT_REPO, sizeof(config->repo) - 1);
    }
#endif
    config->poll_interval_sec = CONFIG_EXAMPLE_GITHUB_POLL_INTERVAL_SEC;

    nvs_handle_t handle;
    if (nvs_open(GITHUB_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return ESP_OK;
    }

    gh_nvs_get_str(handle, GITHUB_NVS_KEY_OWNER, config->owner, sizeof(config->owner));
    gh_nvs_get_str(handle, GITHUB_NVS_KEY_REPO, config->repo, sizeof(config->repo));
    gh_nvs_get_str(handle, GITHUB_NVS_KEY_TOKEN, config->token, sizeof(config->token));

    uint16_t poll = 0;
    if (nvs_get_u16(handle, GITHUB_NVS_KEY_POLL, &poll) == ESP_OK && poll > 0) {
        config->poll_interval_sec = poll;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t github_config_save(const github_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GITHUB_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    nvs_set_str(handle, GITHUB_NVS_KEY_OWNER, config->owner);
    nvs_set_str(handle, GITHUB_NVS_KEY_REPO, config->repo);
    nvs_set_str(handle, GITHUB_NVS_KEY_TOKEN, config->token);
    nvs_set_u16(handle, GITHUB_NVS_KEY_POLL, (uint16_t)config->poll_interval_sec);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
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
                    cJSON *url_item = cJSON_GetObjectItem(asset, "url");
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

    strncpy(release->tag_name, tag->valuestring, sizeof(release->tag_name) - 1);
    strncpy(release->asset_url, asset_url, sizeof(release->asset_url) - 1);
    release->needs_auth = (config->token[0] != '\0');

    ESP_LOGI(TAG, "Latest release: %s, asset: %s", release->tag_name, release->asset_url);
    cJSON_Delete(root);
    return ESP_OK;
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

    ESP_LOGI(TAG, "New version available: running=%s, release=%s",
             running_info.version, release->tag_name);
    return true;
}

static esp_err_t gh_ota_http_init_cb(esp_http_client_handle_t client)
{
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
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
        .timeout_ms = CONFIG_EXAMPLE_OTA_RECV_TIMEOUT,
        .keep_alive_enable = true,
        .buffer_size = CONFIG_EXAMPLE_OTA_BUF_SIZE,
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

static esp_err_t http_api_get_config(httpd_req_t *req)
{
    github_config_t config;
    github_config_load_defaults(&config);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "owner", config.owner);
    cJSON_AddStringToObject(root, "repo", config.repo);
    cJSON_AddNumberToObject(root, "poll_interval_sec", config.poll_interval_sec);
    cJSON_AddStringToObject(root, "token", config.token[0] ? "(set)" : "(not set)");

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t running_info;
    if (esp_ota_get_partition_description(running, &running_info) == ESP_OK) {
        cJSON_AddStringToObject(root, "firmware_version", running_info.version);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static void gh_json_set_str(cJSON *obj, cJSON *item, char *dst, size_t dst_size)
{
    if (item && cJSON_IsString(item)) {
        strncpy(dst, item->valuestring, dst_size - 1);
    }
}

static esp_err_t http_api_post_config(httpd_req_t *req)
{
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    github_config_t config;
    github_config_load_defaults(&config);

    gh_json_set_str(root, cJSON_GetObjectItem(root, "owner"), config.owner, sizeof(config.owner));
    gh_json_set_str(root, cJSON_GetObjectItem(root, "repo"), config.repo, sizeof(config.repo));
    gh_json_set_str(root, cJSON_GetObjectItem(root, "token"), config.token, sizeof(config.token));

    cJSON *poll = cJSON_GetObjectItem(root, "poll_interval_sec");
    if (poll && cJSON_IsNumber(poll)) {
        config.poll_interval_sec = poll->valueint;
        if (config.poll_interval_sec < 60) {
            config.poll_interval_sec = 60;
        }
    }

    cJSON_Delete(root);

    esp_err_t err = github_config_save(&config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    xEventGroupSetBits(s_gh_event_group, GH_BIT_CONFIG_CHANGED);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static httpd_handle_t s_httpd = NULL;

esp_err_t github_config_api_start(void)
{
    s_gh_event_group = xEventGroupCreate();
    if (s_gh_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.task_priority = tskIDLE_PRIORITY + 5;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_get = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = http_api_get_config,
    };
    httpd_register_uri_handler(s_httpd, &uri_get);

    httpd_uri_t uri_post = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = http_api_post_config,
    };
    httpd_register_uri_handler(s_httpd, &uri_post);

    ESP_LOGI(TAG, "Config API started on port %d", config.server_port);
    return ESP_OK;
}

void github_poller_task(void *arg)
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
    xTaskCreate(&github_poller_task, "github_poller", 1024 * 10, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void github_poller_trigger(void)
{
    if (s_gh_event_group) {
        xEventGroupSetBits(s_gh_event_group, GH_BIT_CONFIG_CHANGED);
    }
}
