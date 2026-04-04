#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "hid_rfid.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

/* GET /api/rfid-events — polled by frontend every 3 s instead of SSE */
esp_err_t api_rfid_events_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    hid_rfid_event_t ev = {0};
    hid_rfid_get_last_event(&ev);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"seq\":%lu,\"uid\":\"%s\",\"name\":\"%s\","
             "\"time\":\"%s\",\"status\":\"%s\"}",
             (unsigned long)ev.seq,
             ev.uid, ev.name, ev.time, ev.status);
    return http_send_json(req, 200, buf);
}

/* GET /api/attendance
 *
 * Uses chunked transfer encoding — each student/record row is emitted
 * as a ~300-400 byte chunk directly into the TCP stack.  This avoids
 * the 300 KB+ contiguous DMA-capable DRAM allocation that
 * db_attendance_json() + httpd_resp_sendstr() would require, which
 * caused the "Not enough heap memory" crash with > ~75 students. */
typedef struct { httpd_req_t *req; esp_err_t last_err; } att_stream_ctx_t;
static esp_err_t att_chunk_cb(const char *chunk, size_t len, void *ctx)
{
    att_stream_ctx_t *c = (att_stream_ctx_t *)ctx;
    c->last_err = httpd_resp_send_chunk(c->req, chunk, (ssize_t)len);
    return c->last_err;
}

esp_err_t api_attendance_get(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    att_stream_ctx_t ctx = { .req = req, .last_err = ESP_OK };
    esp_err_t ret = db_attendance_stream_json(auth_get_selected_class(),
                                              att_chunk_cb, &ctx);
    httpd_resp_send_chunk(req, NULL, 0); /* terminate chunked transfer */
    return ret;
}

/* POST /api/attendance/mark
   body: {"uid":"...","date":"YYYY-MM-DD","time":"HH:MM:SS"} */
esp_err_t api_attendance_mark_post(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char body[256] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *uid  = cJSON_IsString(cJSON_GetObjectItem(root,"uid"))
                     ? cJSON_GetObjectItem(root,"uid")->valuestring : "";
    const char *date = cJSON_IsString(cJSON_GetObjectItem(root,"date"))
                     ? cJSON_GetObjectItem(root,"date")->valuestring : "";
    const char *time = cJSON_IsString(cJSON_GetObjectItem(root,"time"))
                     ? cJSON_GetObjectItem(root,"time")->valuestring : "";

    esp_err_t ret;
    if (!strlen(uid) || !strlen(date))
        ret = http_send_err(req, 400, "missing_uid_or_date");
    else {
        db_attendance_mark(auth_get_selected_class(), uid, date, time);
        ret = http_send_ok(req);
    }
    cJSON_Delete(root);
    return ret;
}

/* GET /api/attendance/delete?id=42 */
esp_err_t api_attendance_delete_get(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char buf[12] = {0};
    if (!http_query_param(req, "id", buf, sizeof(buf)))
        return http_send_err(req, 400, "missing_id");
    db_attendance_delete_by_id(atoi(buf), auth_get_selected_class());
    return http_send_ok(req);
}

/* GET /api/report?month=2025-01 */
esp_err_t api_report_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char month[12] = {0};
    if (!http_query_param(req, "month", month, sizeof(month))) {
        struct tm ti;
        db_get_localtime(&ti);
        strftime(month, sizeof(month), "%Y-%m", &ti);
    }
    char cbuf[8] = {0};
    int class_num = auth_get_selected_class();
    if (http_query_param(req, "c", cbuf, sizeof(cbuf)) && atoi(cbuf) > 0)
        class_num = atoi(cbuf);
    if (class_num <= 0) return http_send_err(req, 400, "no_class");

    char *mp = db_config_get("min_att_pct");
    int min_pct = mp ? atoi(mp) : 75;
    free(mp);

    /* Chunked streaming — avoids large contiguous DMA buffer */
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    att_stream_ctx_t ctx = { .req = req, .last_err = ESP_OK };
    esp_err_t ret = db_report_stream_json(class_num, month, min_pct,
                                          att_chunk_cb, &ctx);
    httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

/* GET /api/attendance/summary?c=6&date=2025-04-04 */
esp_err_t api_attendance_summary_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char cbuf[8]={0}, date[12]={0};
    if (!http_query_param(req,"c",cbuf,sizeof(cbuf)))
        return http_send_err(req, 400, "missing_c");
    if (!http_query_param(req,"date",date,sizeof(date))) {
        struct tm ti;
        db_get_localtime(&ti);
        strftime(date, sizeof(date), "%Y-%m-%d", &ti);
    }
    int cn = atoi(cbuf);
    if (cn <= 0) return http_send_err(req, 400, "invalid_class");
    char *json = db_attendance_summary_json(cn, date);
    esp_err_t ret = http_send_json(req, 200, json ? json : "{'present':0,'absent':0,'total':0}");
    free(json);
    return ret;
}
