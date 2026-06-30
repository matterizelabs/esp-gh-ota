#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "github_ota.h"
#include "gh_internal.h"

static const char *TAG = "github_ota";

EventGroupHandle_t s_gh_event_group;
static httpd_handle_t s_httpd = NULL;

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

static void gh_json_set_str(cJSON *root, cJSON *item, char *dst, size_t dst_size)
{
    if (item && cJSON_IsString(item)) {
        gh_str_copy(dst, item->valuestring, dst_size);
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
        if (config.poll_interval_sec > 86400) {
            config.poll_interval_sec = 86400;
        }
    }

    cJSON_Delete(root);

    esp_err_t err = github_config_save(&config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    github_poller_trigger();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t github_config_api_start(void)
{
    if (s_gh_event_group == NULL) {
        s_gh_event_group = xEventGroupCreate();
        if (s_gh_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
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
