#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "wifi_mgr.h"
#include "hid_rfid.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include <stdio.h>

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
esp_err_t api_schema_delete_handler(httpd_req_t *req)
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
esp_err_t api_schema_toggle_post(httpd_req_t *req)
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
    /* FIX-24: db_config_get() malloc's — caller must free(). Previously the
     * pointer was dropped, leaking on every GET /api/admin/settings request. */
    char *buf = db_config_get("min_att_pct");
    if (buf && buf[0]) val = atoi(buf);
    free(buf);
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
esp_err_t api_admin_reset_all_post(httpd_req_t *req)
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
esp_err_t api_admin_backup_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    db_sd_backup();
    return http_send_ok(req);
}

/* GET /api/admin/backup-list — returns JSON array of .bak filenames */
esp_err_t api_admin_backup_list_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char *list = db_sd_list_backups();
    if (!list) return http_send_err(req, 500, "list_failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, list);
    free(list);
    return ESP_OK;
}

/* POST /api/admin/restore-backup  body: {"filename":"attendance_2026-04-09.bak"} */
esp_err_t api_admin_restore_backup_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char buf[128] = {0};
    int  len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return http_send_err(req, 400, "no_body");

    cJSON *root = cJSON_Parse(buf);
    if (!root) return http_send_err(req, 400, "bad_json");

    cJSON *fn = cJSON_GetObjectItemCaseSensitive(root, "filename");
    if (!cJSON_IsString(fn) || !fn->valuestring || !fn->valuestring[0]) {
        cJSON_Delete(root);
        return http_send_err(req, 400, "missing_filename");
    }

    bool ok = db_sd_restore(fn->valuestring);
    cJSON_Delete(root);

    if (!ok) return http_send_err(req, 500, "restore_failed");
    return http_send_ok(req);
}

/* POST /api/admin/sd-remount — soft re-mount without reboot/format */
esp_err_t api_admin_sd_remount_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    int rc = db_sd_remount();
    if (rc != ESP_OK) return http_send_err(req, 500, "remount_failed");
    return http_send_ok(req);
}

/* GET /api/admin/sd-health — returns {"healthy":true/false} */
esp_err_t api_admin_sd_health_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    bool healthy = db_sd_health_check();
    char out[32];
    snprintf(out, sizeof(out), "{\"healthy\":%s}", healthy ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* GET /api/admin/delete-backup?file=attendance_2026-04-09.bak */
esp_err_t api_admin_delete_backup_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char fname[64] = {0};
    if (!http_query_param(req, "file", fname, sizeof(fname)))
        return http_send_err(req, 400, "missing_file");
    if (!db_sd_delete_backup(fname))
        return http_send_err(req, 500, "delete_failed");
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

/* \u2500\u2500 Streaming CSV import \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
 *
 * ROOT CAUSE OF "oom" BUG: the old code did a single 128 KB
 * heap_caps_malloc/malloc call for the import buffer.  CONFIG_SPIRAM
 * was not set, so the PSRAM path always returned NULL, and 128 KB of
 * *contiguous* internal DRAM is unavailable once WiFi + httpd + SQLite
 * have fragmented the heap.
 *
 * FIX A \u2013 sdkconfig: CONFIG_SPIRAM=y now enabled for N16R8 (8 MB OPI
 *          PSRAM), so import_buf_alloc would succeed.  But we go further:
 *
 * FIX B \u2013 No large buffer at all: read the POST body in 4 KB socket
 *          chunks, accumulate lines in a 512-byte stack buffer, and call
 *          db_import_process_line() per row.  Peak extra heap: 4 KB.
 *          Works even on boards without PSRAM.
 *
 * ALSO FIXED: api_csv_upload() was defined but never registered as a
 * route \u2014 dead code.  Removed along with s_import_buf / s_import_len.
 * \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */

#define CSV_CHUNK  4096   /* socket read chunk \u2014 always fits in DRAM */
#define CSV_LMAX   512    /* max bytes per CSV row                    */

/* Split a CSV line in-place (strtok). Returns field count. */
static int csv_split_line(char *line, char **fields, int max_fields)
{
    int n = 0;
    char *tok = strtok(line, ",");
    while (tok && n < max_fields) {
        while (*tok == ' ') tok++;   /* strip leading whitespace */
        fields[n++] = tok;
        tok = strtok(NULL, ",");
    }
    return n;
}

/* Core streaming helper shared by both import endpoints.
 * Reads POST body in CSV_CHUNK pieces, parses lines, inserts rows.
 * Never allocates more than CSV_CHUNK bytes from the heap. */
static esp_err_t stream_import_csv(httpd_req_t *req,
                                   int class_num, const char *type)
{
    /* FIX-26: Reject empty bodies early — avoids opening a useless transaction
     * that writes nothing and wastes a flash write cycle. */
    if (req->content_len == 0) {
        return http_send_err(req, 400, "empty_body");
    }

    char *chunk = malloc(CSV_CHUNK);
    if (!chunk) return http_send_err(req, 500, "oom");

    char line_buf[CSV_LMAX];
    int  line_len     = 0;
    bool skip_hdr     = true;   /* first non-empty line is the header */
    /* FIX-25: Track whether the current line exceeded CSV_LMAX so we can
     * skip it entirely instead of inserting truncated, corrupt data. */
    bool line_overflow = false;
    db_import_result_t result = {0, 0};
    int remaining = (int)req->content_len;

    db_import_begin();

    while (remaining > 0) {
        int to_read = remaining < CSV_CHUNK ? remaining : CSV_CHUNK;
        int got = httpd_req_recv(req, chunk, to_read);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        remaining -= got;

        for (int i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\n') {
                if (line_overflow) {
                    /* FIX-25: Line was too long — skip it rather than inserting
                     * the truncated garbage that was buffered so far. */
                    ESP_LOGW("import", "Skipped CSV line exceeding %d bytes", CSV_LMAX);
                    line_overflow = false;
                    result.skipped++;
                } else {
                    /* Strip trailing \r */
                    if (line_len > 0 && line_buf[line_len - 1] == '\r') line_len--;
                    line_buf[line_len] = '\0';
                    if (line_len > 0) {
                        if (skip_hdr) {
                            skip_hdr = false;
                        } else {
                            char *fields[8] = {0};
                            int nf = csv_split_line(line_buf, fields, 8);
                            db_import_process_line(class_num, type, fields, nf, &result);
                        }
                    }
                }
                line_len = 0;
            } else if (line_len < CSV_LMAX - 1) {
                line_buf[line_len++] = c;
            } else {
                /* FIX-25: Buffer full — mark overflow, keep reading until '\n' */
                line_overflow = true;
            }
        }
        vTaskDelay(1);  /* yield to watchdog / IDLE on large uploads */
    }

    /* Handle last line if file has no trailing newline */
    if (!line_overflow && line_len > 0 && !skip_hdr) {
        if (line_buf[line_len - 1] == '\r') line_len--;
        line_buf[line_len] = '\0';
        char *fields[8] = {0};
        int nf = csv_split_line(line_buf, fields, 8);
        db_import_process_line(class_num, type, fields, nf, &result);
    }

    /* FIX-26: Only commit if at least one row was processed; otherwise rollback
     * to avoid a useless write transaction on flash storage. */
    if (result.added > 0 || result.skipped > 0) {
        db_import_end();
    } else {
        db_exec_raw("ROLLBACK;");
    }
    free(chunk);

    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"added\":%d,\"skipped\":%d}",
             result.added, result.skipped);
    return http_send_json(req, 200, out);
}

/* POST /api/admin/import-csv?classnum=7&type=students  (body: CSV text) */
esp_err_t api_admin_import_csv_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char cbuf[8] = {0}, type[16] = {0};
    http_query_param(req, "classnum", cbuf, sizeof(cbuf));
    http_query_param(req, "type",     type, sizeof(type));

    int cn = atoi(cbuf);
    if (!cn || !strlen(type))
        return http_send_err(req, 400, "missing_classnum_or_type");

    return stream_import_csv(req, cn, type);
}

/* POST /api/admin/import-attendance-zip?classnum=7  (body: CSV text) */
esp_err_t api_admin_import_attendance_zip_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");

    char cbuf[8] = {0};
    int cn = 0;
    if (http_query_param(req, "classnum", cbuf, sizeof(cbuf)))
        cn = atoi(cbuf);
    if (!cn) cn = auth_get_selected_class();
    if (!cn) return http_send_err(req, 400, "no_class");

    return stream_import_csv(req, cn, "attendance");
}

/* GET /api/status */
esp_err_t api_status_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    int cls = auth_get_selected_class();
    char *json = db_status_json(cls);

    /* Inject ntp_ok and rfid_ok into the status response. */
    char *ntp_val = db_config_get("ntp_synced");
    bool ntp_ok  = (ntp_val && ntp_val[0] == '1');
    free(ntp_val);
    bool rfid_ok = hid_rfid_is_connected();

    esp_err_t ret;
    if (json && json[0] == '{') {
        size_t jlen = strlen(json);
        char *augmented = malloc(jlen + 64);
        if (augmented) {
            memcpy(augmented, json, jlen - 1);
            snprintf(augmented + jlen - 1, 64,
                     ",\"ntp_ok\":%s,\"rfid_ok\":%s}",
                     ntp_ok  ? "true" : "false",
                     rfid_ok ? "true" : "false");
            ret = http_send_json(req, 200, augmented);
            free(augmented);
        } else {
            ret = http_send_json(req, 200, json);
        }
    } else {
        char fallback[64];
        snprintf(fallback, sizeof(fallback),
                 "{\"ntp_ok\":%s,\"rfid_ok\":%s}",
                 ntp_ok  ? "true" : "false",
                 rfid_ok ? "true" : "false");
        ret = http_send_json(req, 200, fallback);
    }
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


/* POST /api/admin/reboot */
esp_err_t api_admin_reboot_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    http_send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* POST /api/admin/rfid-reconnect
 * Triggers a power-cycle of the USB RFID reader and waits up to 5 s
 * for re-enumeration.  Returns {"ok":true,"connected":true/false}.
 * Blocks the HTTP worker task for the duration — acceptable because
 * hid_rfid_reconnect() is capped at ~6 s and the ESP-IDF httpd has
 * multiple worker threads.                                           */
esp_err_t api_admin_rfid_reconnect_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    bool connected = hid_rfid_reconnect();
    char buf[48];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"connected\":%s}", connected ? "true" : "false");
    return http_send_json(req, 200, buf);
}
