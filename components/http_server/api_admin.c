#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "wifi_mgr.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include <stdio.h>

/* Allocate the import buffer preferring external PSRAM (8 MB on ESP32-S3)
 * so that the 128 KB block doesn't have to come from the small internal
 * DRAM heap that is already pressured by WiFi + httpd + SQLite.
 * Falls back to malloc() (internal DRAM) on boards without PSRAM. */
static inline char *import_buf_alloc(size_t sz)
{
    char *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sz);   /* PSRAM absent or full — try internal DRAM */
    return p;
}

/* ── Schema ──────────────────────────────────────────────────── */

/* GET /api/schema */
esp_err_t api_schema_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char *json = db_schema_get_json();
    esp_err_t ret = http_send_json(req, 200, json ? json : "{}");
    free(json);
    return ret;
}

/* POST /api/schema/add  body: {"coltype":"student","key":"phone","label":"Phone"} */
esp_err_t api_schema_add_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[256] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *ct    = cJSON_IsString(cJSON_GetObjectItem(root,"coltype"))
                      ? cJSON_GetObjectItem(root,"coltype")->valuestring : "";
    const char *key   = cJSON_IsString(cJSON_GetObjectItem(root,"key"))
                      ? cJSON_GetObjectItem(root,"key")->valuestring : "";
    const char *label = cJSON_IsString(cJSON_GetObjectItem(root,"label"))
                      ? cJSON_GetObjectItem(root,"label")->valuestring : "";

    esp_err_t ret;
    if (!strlen(ct)||!strlen(key)||!strlen(label))
        ret = http_send_err(req, 400, "missing_fields");
    else {
        int ask = (strcmp(ct,"student")==0) ? 1 : 0;
        db_schema_add(ct, key, label, ask);
        ret = http_send_ok(req);
    }
    cJSON_Delete(root);
    return ret;
}

/* GET /api/schema/delete?type=student&key=phone */
esp_err_t api_schema_delete_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char type[16]={0}, key[32]={0};
    if (!http_query_param(req,"type",type,sizeof(type)) ||
        !http_query_param(req,"key", key, sizeof(key)))
        return http_send_err(req, 400, "missing_params");
    db_schema_delete(type, key);
    return http_send_ok(req);
}

/* POST /api/schema/edit  body: {"coltype":"student","key":"phone","label":"Mobile"} */
esp_err_t api_schema_edit_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[256] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *ct    = cJSON_IsString(cJSON_GetObjectItem(root,"coltype"))
                      ? cJSON_GetObjectItem(root,"coltype")->valuestring : "";
    const char *key   = cJSON_IsString(cJSON_GetObjectItem(root,"key"))
                      ? cJSON_GetObjectItem(root,"key")->valuestring : "";
    const char *label = cJSON_IsString(cJSON_GetObjectItem(root,"label"))
                      ? cJSON_GetObjectItem(root,"label")->valuestring : "";

    db_schema_edit_label(ct, key, label);
    cJSON_Delete(root);
    return http_send_ok(req);
}

/* GET /api/schema/toggle?coltype=student&key=phone&flag=ask&val=1 */
esp_err_t api_schema_toggle_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char ct[16]={0}, key[32]={0}, flag[16]={0}, valbuf[4]={0};
    http_query_param(req,"coltype",ct,  sizeof(ct));
    http_query_param(req,"key",   key,  sizeof(key));
    http_query_param(req,"flag",  flag, sizeof(flag));
    http_query_param(req,"val",   valbuf,sizeof(valbuf));
    if (!strlen(ct)||!strlen(key)||!strlen(flag))
        return http_send_err(req, 400, "missing_params");
    db_schema_toggle_flag(ct, key, flag, atoi(valbuf));
    return http_send_ok(req);
}

/* ── Admin actions ───────────────────────────────────────────── */

/* GET /api/admin/settings — returns {"min_att_pct":N} */
esp_err_t api_admin_settings_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    int val = 75; /* default */
    const char *buf = db_config_get("min_att_pct");
    if (buf && buf[0]) val = atoi(buf);
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"min_att_pct\":%d}", val);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* POST /api/admin/settings  body: {"min_att_pct":75} */
esp_err_t api_admin_settings_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[128] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    cJSON *mp = cJSON_GetObjectItem(root, "min_att_pct");
    if (cJSON_IsNumber(mp)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", mp->valueint);
        db_config_set("min_att_pct", buf);
    }
    cJSON_Delete(root);
    return http_send_ok(req);
}

/* POST /api/admin/reset-class  body: {"classnum":7} */
esp_err_t api_admin_reset_class_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[64] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");
    int cn = cJSON_GetObjectItem(root,"classnum")
           ? cJSON_GetObjectItem(root,"classnum")->valueint : 0;
    cJSON_Delete(root);
    if (!cn) return http_send_err(req, 400, "missing_classnum");
    db_reset_class(cn);
    return http_send_ok(req);
}

/* GET /api/admin/reset-all */
esp_err_t api_admin_reset_all_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    db_reset_all();
    return http_send_ok(req);
}

/* POST /api/admin/factory-reset  body: {"classes":"6,7,8","batches":"08:00,10:00"} */
esp_err_t api_admin_factory_reset_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[256] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *classes  = cJSON_IsString(cJSON_GetObjectItem(root,"classes"))
                         ? cJSON_GetObjectItem(root,"classes")->valuestring
                         : "6,7,8,9,10";
    const char *batches  = cJSON_IsString(cJSON_GetObjectItem(root,"batches"))
                         ? cJSON_GetObjectItem(root,"batches")->valuestring
                         : "08:00,10:00,12:00,14:00,16:00";

    db_factory_reset(classes, batches);
    auth_set_selected_class(0);
    cJSON_Delete(root);
    return http_send_ok(req);
}

/* GET /api/admin/backup-now */
esp_err_t api_admin_backup_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    db_sd_backup();
    return http_send_ok(req);
}

/* GET /api/admin/export-csv?c=7&type=students */
esp_err_t api_admin_export_csv_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char cbuf[8]={0}, type[16]={0};
    if (!http_query_param(req,"c",   cbuf, sizeof(cbuf)) ||
        !http_query_param(req,"type",type, sizeof(type)))
        return http_send_err(req, 400, "missing_params");

    int cn = atoi(cbuf);
    char *csv = db_export_csv(cn, type);
    if (!csv) return http_send_err(req, 500, "export_failed");

    /* Build Content-Disposition header */
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"class%d_%s.csv\"", cn, type);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Send in 4096-byte chunks to avoid a single large DMA-capable DRAM
     * allocation that httpd_resp_sendstr() would require (~124 KB for a
     * full attendance export).  Each chunk only needs ~4 KB of DMA DRAM. */
    size_t total = strlen(csv);
    size_t sent  = 0;
    esp_err_t ret = ESP_OK;
    while (sent < total && ret == ESP_OK) {
        size_t chunk = total - sent;
        if (chunk > 4096) chunk = 4096;
        ret = httpd_resp_send_chunk(req, csv + sent, (ssize_t)chunk);
        sent += chunk;
    }
    httpd_resp_send_chunk(req, NULL, 0); /* finalize chunked transfer */
    free(csv);
    return ret;
}

/* POST /api/admin/import-csv
   Multipart form: classnum, type, file (CSV text) */

/* Import buffer — allocated on demand, freed immediately after use.
 * Previously this was a 128 KB static array in .bss which permanently
 * consumed a quarter of internal DRAM from boot, contributing to the
 * heap fragmentation that caused the DMA crash on the attendance page. */
#define IMPORT_BUF_SIZE 131072
static char   *s_import_buf = NULL;
static size_t  s_import_len = 0;

esp_err_t api_csv_upload(httpd_req_t *req)
{
    /* Allocate buffer if not already allocated */
    if (!s_import_buf) {
        s_import_buf = import_buf_alloc(IMPORT_BUF_SIZE);
        if (!s_import_buf) {
            ESP_LOGE("api_admin", "Failed to allocate import buffer (%d bytes)",
                     IMPORT_BUF_SIZE);
            return ESP_ERR_NO_MEM;
        }
    }
    s_import_len = 0;
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t avail = IMPORT_BUF_SIZE - s_import_len - 1;
        if (avail == 0) break; /* buffer full */
        size_t to_read = remaining < avail ? remaining : avail;
        int ret = httpd_req_recv(req, s_import_buf + s_import_len, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        s_import_len += (size_t)ret;
        remaining    -= (size_t)ret;
        /* Yield to avoid watchdog on large uploads */
        vTaskDelay(1);
    }
    s_import_buf[s_import_len] = '\0';
    return ESP_OK;
}

esp_err_t api_admin_import_csv_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    /* classnum and type come as query params since body is the CSV */
    char cbuf[8]={0}, type[16]={0};
    http_query_param(req,"classnum",cbuf, sizeof(cbuf));
    http_query_param(req,"type",    type, sizeof(type));

    int cn = atoi(cbuf);
    if (!cn || !strlen(type))
        return http_send_err(req, 400, "missing_classnum_or_type");

    if (s_import_len == 0) {
        /* Read body directly — buffer holds IMPORT_BUF_SIZE-1 bytes + null terminator */
        if (!s_import_buf) {
            s_import_buf = import_buf_alloc(IMPORT_BUF_SIZE);
            if (!s_import_buf)
                return http_send_err(req, 500, "oom");
        }
        /* http_read_body returns byte count, or -1 on error.
         * Must NOT use strlen() — malloc buffer is uninitialised and
         * http_read_body does not null-terminate. */
        int nread = http_read_body(req, s_import_buf, IMPORT_BUF_SIZE - 1);
        if (nread < 0) {
            free(s_import_buf); s_import_buf = NULL;
            return http_send_err(req, 400, "read_error");
        }
        s_import_buf[nread] = '\0';   /* null-terminate for safety */
        s_import_len = (size_t)nread;
    }

    if (s_import_len == 0) {
        free(s_import_buf); s_import_buf = NULL;
        return http_send_err(req, 400, "no_data");
    }

    db_import_result_t result = db_import_csv(cn, type, s_import_buf, s_import_len);
    s_import_len = 0;
    /* Free the buffer immediately — it's only needed during import */
    free(s_import_buf);
    s_import_buf = NULL;

    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"added\":%d,\"skipped\":%d}",
             result.added, result.skipped);
    return http_send_json(req, 200, out);
}


/* GET /api/status */
esp_err_t api_status_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    int cls = auth_get_selected_class();
    char *json = db_status_json(cls);
    esp_err_t ret = http_send_json(req, 200, json ? json : "{}");
    free(json);
    return ret;
}

/* HEAD /api/admin/export-csv — browser preflight for downloads */
esp_err_t api_admin_export_csv_head(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

/* GET /api/admin/export-all-zip — export all data as combined CSV */
esp_err_t api_admin_export_all_zip_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char *csv = db_export_all_csv();
    if (!csv) return http_send_err(req, 500, "export_failed");

    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"all_data.csv\"");
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Send in chunks to avoid large single send */
    size_t total = strlen(csv);
    size_t sent  = 0;
    esp_err_t ret = ESP_OK;
    while (sent < total && ret == ESP_OK) {
        size_t chunk = total - sent;
        if (chunk > 4096) chunk = 4096;
        ret = httpd_resp_send_chunk(req, csv + sent, (ssize_t)chunk);
        sent += chunk;
        vTaskDelay(1);
    }
    httpd_resp_send_chunk(req, NULL, 0); /* finalize */
    free(csv);
    return ret;
}

/* POST /api/admin/import-attendance-zip — import attendance CSV (same as import-csv) */
esp_err_t api_admin_import_attendance_zip_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    /* Allocate import buffer if not already held */
    if (!s_import_buf) {
        s_import_buf = import_buf_alloc(IMPORT_BUF_SIZE);
        if (!s_import_buf)
            return http_send_err(req, 500, "oom");
    }

    /* Read body into import buffer */
    s_import_len = 0;
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t avail = IMPORT_BUF_SIZE - s_import_len - 1;
        if (avail == 0) break;
        size_t to_read = remaining < avail ? remaining : avail;
        int ret = httpd_req_recv(req, s_import_buf + s_import_len, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        s_import_len += (size_t)ret;
        remaining    -= (size_t)ret;
        vTaskDelay(1);
    }
    s_import_buf[s_import_len] = '\0';

    if (s_import_len == 0) {
        free(s_import_buf); s_import_buf = NULL;
        return http_send_err(req, 400, "no_data");
    }

    /* Try to get class from query param, fallback to session class */
    char cbuf[8] = {0};
    int cn = 0;
    if (http_query_param(req, "classnum", cbuf, sizeof(cbuf)))
        cn = atoi(cbuf);
    if (!cn) cn = auth_get_selected_class();
    if (!cn) {
        free(s_import_buf); s_import_buf = NULL;
        return http_send_err(req, 400, "no_class");
    }

    db_import_result_t result = db_import_csv(cn, "attendance", s_import_buf, s_import_len);
    s_import_len = 0;
    free(s_import_buf);
    s_import_buf = NULL;

    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"added\":%d,\"skipped\":%d}",
             result.added, result.skipped);
    return http_send_json(req, 200, out);
}

/* POST /api/admin/reboot */
esp_err_t api_admin_reboot_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    http_send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
