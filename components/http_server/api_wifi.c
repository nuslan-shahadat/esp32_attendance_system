/*  api_wifi.c — WiFi configuration API handlers
    GET  /api/wifi/status   → {mode, ssid, ip, ap_ssid, rssi}
    GET  /api/wifi/scan     → [{ssid, rssi, secured}]
    POST /api/wifi/save     body: {ssid, pass}
    POST /api/wifi/start-ap                                         */

#include "http_server.h"
#include "auth.h"
#include "wifi_mgr.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_wifi";

/* ── GET /api/wifi/status ────────────────────────────────────── */
esp_err_t api_wifi_status_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char *json = wifi_mgr_status_json();
    esp_err_t ret = http_send_json(req, 200, json ? json : "{\"mode\":\"unknown\"}");
    free(json);
    return ret;
}

/* ── GET /api/wifi/scan ──────────────────────────────────────── */
esp_err_t api_wifi_scan_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    ESP_LOGI(TAG, "WiFi scan requested");
    char *json = wifi_mgr_scan_json();
    esp_err_t ret = http_send_json(req, 200, json ? json : "[]");
    free(json);
    return ret;
}

/* ── POST /api/wifi/save ─────────────────────────────────────── */
esp_err_t api_wifi_save_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char body[256] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "pass");

    if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring[0]) {
        cJSON_Delete(root);
        return http_send_err(req, 400, "missing_ssid");
    }

    const char *ssid = ssid_item->valuestring;
    const char *pass = cJSON_IsString(pass_item) ? pass_item->valuestring : "";

    ESP_LOGI(TAG, "Saving WiFi credentials for SSID: %s", ssid);

    /* Send OK before restarting so the client gets a response */
    http_send_ok(req);

    wifi_mgr_save_and_restart(ssid, pass);   /* saves to DB and restarts */

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── POST /api/wifi/start-ap ─────────────────────────────────── */
esp_err_t api_wifi_start_ap_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    ESP_LOGI(TAG, "Switching to AP mode by request");
    http_send_ok(req);
    wifi_mgr_start_ap();
    return ESP_OK;
}
