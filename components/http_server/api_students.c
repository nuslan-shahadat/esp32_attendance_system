#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdlib.h>

/* ── Streaming callback context ────────────────────────────────
 * Wraps httpd_resp_send_chunk so db_students_stream_json can push
 * each student row directly into the TCP stack without ever building
 * a full JSON string in heap. */
typedef struct {
    httpd_req_t *req;
    esp_err_t    last_err;
} student_stream_ctx_t;

static esp_err_t student_chunk_cb(const char *chunk, size_t len, void *ctx)
{
    student_stream_ctx_t *c = (student_stream_ctx_t *)ctx;
    c->last_err = httpd_resp_send_chunk(c->req, chunk, (ssize_t)len);
    return c->last_err;
}

/* GET /api/students?archived=0
 *
 * Uses chunked transfer encoding so the lwIP layer only needs to
 * DMA-allocate ~300 bytes at a time (one student record) instead of
 * a 14 KB+ contiguous DRAM buffer for the whole list.  This prevents
 * the "Not enough heap memory" DMA crash seen with >75 students. */
esp_err_t api_students_get(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    int cls = auth_get_selected_class();

    char arch_buf[4] = {0};
    int archived = 0;
    if (http_query_param(req, "archived", arch_buf, sizeof(arch_buf)))
        archived = atoi(arch_buf);

    /* Get min_att_pct from config */
    char *mp = db_config_get("min_att_pct");
    int min_pct = mp ? atoi(mp) : 75;
    free(mp);

    /* Set headers for a chunked JSON response.
     * httpd_resp_send_chunk() will add Transfer-Encoding: chunked
     * automatically; we just need Content-Type and CORS. */
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    student_stream_ctx_t ctx = { .req = req, .last_err = ESP_OK };
    esp_err_t ret = db_students_stream_json(cls, archived, min_pct,
                                            student_chunk_cb, &ctx);

    /* Terminate chunked transfer (zero-length chunk = end of body) */
    httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

/* GET /api/students/detail?uid=1234567890 */
esp_err_t api_students_detail_get(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char uid[32] = {0};
    if (!http_query_param(req, "uid", uid, sizeof(uid)))
        return http_send_err(req, 400, "missing_uid");
    char *json = db_student_detail_json(uid, auth_get_selected_class());
    esp_err_t ret = http_send_json(req, 200, json ? json : "{}");
    free(json);
    return ret;
}

/* POST /api/students/register
   body: {"name":"...","roll":"...","card_uid":"...","batchtime":"..."} */
esp_err_t api_students_register_post(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char body[2048] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *name      = cJSON_IsString(cJSON_GetObjectItem(root,"name"))
                           ? cJSON_GetObjectItem(root,"name")->valuestring : "";
    const char *roll      = cJSON_IsString(cJSON_GetObjectItem(root,"roll"))
                           ? cJSON_GetObjectItem(root,"roll")->valuestring : "";
    const char *uid       = cJSON_IsString(cJSON_GetObjectItem(root,"card_uid"))
                           ? cJSON_GetObjectItem(root,"card_uid")->valuestring : "";
    const char *batchtime = cJSON_IsString(cJSON_GetObjectItem(root,"batchtime"))
                           ? cJSON_GetObjectItem(root,"batchtime")->valuestring : "";

    /* Build extra_data JSON from any "extra_<key>" fields in the body */
    cJSON *extra = cJSON_CreateObject();
    cJSON *item  = NULL;
    cJSON_ArrayForEach(item, root) {
        if (item->string && strncmp(item->string, "extra_", 6) == 0) {
            const char *field_key = item->string + 6; /* skip "extra_" prefix */
            if (cJSON_IsString(item))
                cJSON_AddStringToObject(extra, field_key, item->valuestring);
        }
    }
    char *extra_str = cJSON_PrintUnformatted(extra);
    cJSON_Delete(extra);

    esp_err_t ret;
    if (strlen(uid) < 1)
        ret = http_send_err(req, 400, "uid_required");
    else {
        db_student_register(auth_get_selected_class(), uid, name, roll, batchtime,
                            extra_str ? extra_str : "{}");
        ret = http_send_ok(req);
    }
    if (extra_str) free(extra_str);
    cJSON_Delete(root);
    return ret;
}

/* POST /api/students/edit */
esp_err_t api_students_edit_post(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char body[2048] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");

    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");

    const char *uid_orig  = cJSON_IsString(cJSON_GetObjectItem(root,"uid_orig"))
                           ? cJSON_GetObjectItem(root,"uid_orig")->valuestring : "";
    const char *uid_new   = cJSON_IsString(cJSON_GetObjectItem(root,"card_uid"))
                           ? cJSON_GetObjectItem(root,"card_uid")->valuestring : uid_orig;
    const char *name      = cJSON_IsString(cJSON_GetObjectItem(root,"name"))
                           ? cJSON_GetObjectItem(root,"name")->valuestring : "";
    const char *roll      = cJSON_IsString(cJSON_GetObjectItem(root,"roll"))
                           ? cJSON_GetObjectItem(root,"roll")->valuestring : "";
    const char *batchtime = cJSON_IsString(cJSON_GetObjectItem(root,"batchtime"))
                           ? cJSON_GetObjectItem(root,"batchtime")->valuestring : "";

    db_student_edit(auth_get_selected_class(), uid_orig, uid_new, name, roll, batchtime);
    cJSON_Delete(root);
    return http_send_ok(req);
}

/* GET /api/students/archive?uid= */
esp_err_t api_students_archive_post(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char uid[32] = {0};
    if (!http_query_param(req,"uid",uid,sizeof(uid)))
        return http_send_err(req, 400, "missing_uid");
    db_student_archive(uid, auth_get_selected_class());
    return http_send_ok(req);
}

/* GET /api/students/restore?uid= */
esp_err_t api_students_restore_post(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char uid[32] = {0};
    if (!http_query_param(req,"uid",uid,sizeof(uid)))
        return http_send_err(req, 400, "missing_uid");
    db_student_restore(uid, auth_get_selected_class());
    return http_send_ok(req);
}

/* GET /api/students/delete?uid= */
esp_err_t api_students_delete_handler(httpd_req_t *req)
{
    if (!auth_check_with_class(req)) return http_send_err(req, 403, "no_class");
    char uid[32] = {0};
    if (!http_query_param(req,"uid",uid,sizeof(uid)))
        return http_send_err(req, 400, "missing_uid");
    db_student_delete(uid, auth_get_selected_class());
    return http_send_ok(req);
}
