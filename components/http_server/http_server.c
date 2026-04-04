#include "http_server.h"
#include "static_files.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "http_server";

/* ── Forward declarations — handlers defined in api_*.c files ── */
/* Auth */
esp_err_t api_login_post(httpd_req_t *req);
esp_err_t api_logout_post(httpd_req_t *req);
esp_err_t api_session_get(httpd_req_t *req);

/* Classes */
esp_err_t api_classes_get(httpd_req_t *req);
esp_err_t api_classes_add_post(httpd_req_t *req);
esp_err_t api_classes_delete_get(httpd_req_t *req);
esp_err_t api_set_class_get(httpd_req_t *req);
esp_err_t api_batches_get(httpd_req_t *req);
esp_err_t api_batches_add_post(httpd_req_t *req);
esp_err_t api_batches_delete_get(httpd_req_t *req);

/* Students */
esp_err_t api_students_get(httpd_req_t *req);
esp_err_t api_students_register_post(httpd_req_t *req);
esp_err_t api_students_edit_post(httpd_req_t *req);
esp_err_t api_students_archive_get(httpd_req_t *req);
esp_err_t api_students_restore_get(httpd_req_t *req);
esp_err_t api_students_delete_get(httpd_req_t *req);
esp_err_t api_students_detail_get(httpd_req_t *req);

/* Attendance */
esp_err_t api_attendance_get(httpd_req_t *req);
esp_err_t api_attendance_mark_post(httpd_req_t *req);
esp_err_t api_attendance_delete_get(httpd_req_t *req);
esp_err_t api_rfid_events_get(httpd_req_t *req);

/* Report */
esp_err_t api_report_get(httpd_req_t *req);

/* Schema */
esp_err_t api_schema_get(httpd_req_t *req);
esp_err_t api_schema_add_post(httpd_req_t *req);
esp_err_t api_schema_delete_get(httpd_req_t *req);
esp_err_t api_schema_edit_post(httpd_req_t *req);
esp_err_t api_schema_toggle_get(httpd_req_t *req);

/* Admin */
esp_err_t api_admin_settings_get(httpd_req_t *req);
esp_err_t api_admin_settings_post(httpd_req_t *req);
esp_err_t api_admin_reset_class_post(httpd_req_t *req);
esp_err_t api_admin_reset_all_get(httpd_req_t *req);
esp_err_t api_admin_factory_reset_post(httpd_req_t *req);
esp_err_t api_admin_backup_get(httpd_req_t *req);
esp_err_t api_admin_export_csv_get(httpd_req_t *req);
esp_err_t api_admin_import_csv_post(httpd_req_t *req);

/* WiFi */
esp_err_t api_wifi_status_get(httpd_req_t *req);
esp_err_t api_wifi_scan_get(httpd_req_t *req);
esp_err_t api_wifi_save_post(httpd_req_t *req);
esp_err_t api_wifi_start_ap_post(httpd_req_t *req);

/* System status */
esp_err_t api_status_get(httpd_req_t *req);

/* New endpoints */
esp_err_t api_attendance_summary_get(httpd_req_t *req);
esp_err_t api_admin_reboot_post(httpd_req_t *req);
esp_err_t api_admin_export_all_zip_get(httpd_req_t *req);
esp_err_t api_admin_import_attendance_zip_post(httpd_req_t *req);
esp_err_t api_admin_export_csv_head(httpd_req_t *req);

/* ── Route table ────────────────────────────────────────────────
   Using a macro to cut the boilerplate of httpd_uri_t literals. */
#define ROUTE(m, p, h)  { .uri = (p), .method = (m), .handler = (h), .user_ctx = NULL }

static const httpd_uri_t routes[] = {
    /* Auth */
    ROUTE(HTTP_POST, "/api/login",   api_login_post),
    ROUTE(HTTP_POST, "/api/logout",  api_logout_post),
    ROUTE(HTTP_GET,  "/api/session", api_session_get),

    /* Classes */
    ROUTE(HTTP_GET,  "/api/classes",         api_classes_get),
    ROUTE(HTTP_POST, "/api/classes/add",     api_classes_add_post),
    ROUTE(HTTP_GET,  "/api/classes/delete",  api_classes_delete_get),
    ROUTE(HTTP_GET,  "/api/set-class",       api_set_class_get),
    ROUTE(HTTP_GET,  "/api/batches",         api_batches_get),
    ROUTE(HTTP_POST, "/api/batches/add",     api_batches_add_post),
    ROUTE(HTTP_GET,  "/api/batches/delete",  api_batches_delete_get),

    /* Students */
    ROUTE(HTTP_GET,  "/api/students",             api_students_get),
    ROUTE(HTTP_POST, "/api/students/register",    api_students_register_post),
    ROUTE(HTTP_POST, "/api/students/edit",        api_students_edit_post),
    ROUTE(HTTP_GET,  "/api/students/archive",     api_students_archive_get),
    ROUTE(HTTP_GET,  "/api/students/restore",     api_students_restore_get),
    ROUTE(HTTP_GET,  "/api/students/delete",      api_students_delete_get),
    ROUTE(HTTP_GET,  "/api/students/detail",      api_students_detail_get),

    /* Attendance */
    ROUTE(HTTP_GET,  "/api/attendance",        api_attendance_get),
    ROUTE(HTTP_POST, "/api/attendance/mark",   api_attendance_mark_post),
    ROUTE(HTTP_GET,  "/api/attendance/delete", api_attendance_delete_get),
    ROUTE(HTTP_GET,  "/api/rfid-events",       api_rfid_events_get),

    /* Report */
    ROUTE(HTTP_GET, "/api/report", api_report_get),

    /* Schema */
    ROUTE(HTTP_GET,  "/api/schema",         api_schema_get),
    ROUTE(HTTP_POST, "/api/schema/add",     api_schema_add_post),
    ROUTE(HTTP_GET,  "/api/schema/delete",  api_schema_delete_get),
    ROUTE(HTTP_POST, "/api/schema/edit",    api_schema_edit_post),
    ROUTE(HTTP_GET,  "/api/schema/toggle",  api_schema_toggle_get),

    /* Admin */
    ROUTE(HTTP_GET,  "/api/admin/settings",       api_admin_settings_get),
    ROUTE(HTTP_POST, "/api/admin/settings",       api_admin_settings_post),
    ROUTE(HTTP_POST, "/api/admin/reset-class",    api_admin_reset_class_post),
    ROUTE(HTTP_GET,  "/api/admin/reset-all",      api_admin_reset_all_get),
    ROUTE(HTTP_POST, "/api/admin/factory-reset",  api_admin_factory_reset_post),
    ROUTE(HTTP_GET,  "/api/admin/backup-now",     api_admin_backup_get),
    ROUTE(HTTP_GET,  "/api/admin/export-csv",     api_admin_export_csv_get),
    ROUTE(HTTP_POST, "/api/admin/import-csv",     api_admin_import_csv_post),

    /* Additional endpoints */
    ROUTE(HTTP_GET,  "/api/attendance/summary",            api_attendance_summary_get),
    ROUTE(HTTP_POST, "/api/admin/reboot",                  api_admin_reboot_post),
    ROUTE(HTTP_GET,  "/api/admin/export-all-zip",          api_admin_export_all_zip_get),
    ROUTE(HTTP_POST, "/api/admin/import-attendance-zip",   api_admin_import_attendance_zip_post),
    ROUTE(HTTP_HEAD, "/api/admin/export-csv",              api_admin_export_csv_head),

    /* WiFi */
    ROUTE(HTTP_GET,  "/api/wifi/status",    api_wifi_status_get),
    ROUTE(HTTP_GET,  "/api/wifi/scan",      api_wifi_scan_get),
    ROUTE(HTTP_POST, "/api/wifi/save",      api_wifi_save_post),
    ROUTE(HTTP_POST, "/api/wifi/start-ap",  api_wifi_start_ap_post),

    /* System */
    ROUTE(HTTP_GET, "/api/status", api_status_get),

    /* Static files — wildcard catch-all MUST be last */
    ROUTE(HTTP_GET, "/*", static_file_handler),
};

#define ROUTE_COUNT (sizeof(routes) / sizeof(routes[0]))

/* ── Server start ───────────────────────────────────────────── */
httpd_handle_t http_server_start(void)
{
    httpd_config_t config       = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers     = ROUTE_COUNT + 10;
    config.max_open_sockets     = 7;
    config.backlog_conn         = 5;
    config.recv_wait_timeout    = 30;
    config.send_wait_timeout    = 30;
    config.uri_match_fn         = httpd_uri_match_wildcard;
    /* Stack size for the HTTP server task */
    config.stack_size           = 20480;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register route: %s", routes[i].uri);
        }
    }

    ESP_LOGI(TAG, "HTTP server started — %zu routes registered", ROUTE_COUNT);
    return server;
}

/* ── Shared utilities ───────────────────────────────────────── */
esp_err_t http_send_json(httpd_req_t *req, int status_code, const char *json_str)
{
    /* esp_http_server only accepts string status like "200 OK" */
    char status_str[32];
    switch (status_code) {
        case 200: strcpy(status_str, "200 OK");              break;
        case 201: strcpy(status_str, "201 Created");         break;
        case 400: strcpy(status_str, "400 Bad Request");     break;
        case 401: strcpy(status_str, "401 Unauthorized");    break;
        case 403: strcpy(status_str, "403 Forbidden");       break;
        case 404: strcpy(status_str, "404 Not Found");       break;
        case 500: strcpy(status_str, "500 Internal Error");  break;
        default:  snprintf(status_str, sizeof(status_str), "%d", status_code); break;
    }
    httpd_resp_set_status(req, status_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, json_str);
}

esp_err_t http_send_ok(httpd_req_t *req)
{
    return http_send_json(req, 200, "{\"ok\":true}");
}

esp_err_t http_send_err(httpd_req_t *req, int http_status, const char *msg)
{
    /* Build {"error":"<msg>"} — msg must not contain double-quotes */
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg ? msg : "unknown");
    return http_send_json(req, http_status, buf);
}

int http_read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int total = req->content_len;
    if (total <= 0)    return 0;
    if ((size_t)total >= buf_len) {
        ESP_LOGW(TAG, "Body too large: %d bytes (max %zu)", total, buf_len - 1);
        return -1;
    }
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        received += ret;
    }
    buf[received] = '\0';
    return received;
}

bool http_query_param(httpd_req_t *req, const char *key,
                      char *out_buf, size_t out_len)
{
    /* esp_http_server provides httpd_req_get_url_query_str */
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return false;

    char *qstr = malloc(qlen);
    if (!qstr) return false;

    bool found = false;
    if (httpd_req_get_url_query_str(req, qstr, qlen) == ESP_OK) {
        if (httpd_query_key_value(qstr, key, out_buf, out_len) == ESP_OK)
            found = true;
    }
    free(qstr);
    return found;
}
