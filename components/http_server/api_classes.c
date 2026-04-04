#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_classes";

/* GET /api/classes */
esp_err_t api_classes_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char *json = db_classes_list_json();
    esp_err_t ret = http_send_json(req, 200, json ? json : "[]");
    free(json);
    return ret;
}

/* POST /api/classes/add  body: {"classnum": 7} */
esp_err_t api_classes_add_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[64] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");
    int cn = cJSON_GetObjectItem(root, "classnum")
           ? cJSON_GetObjectItem(root, "classnum")->valueint : 0;
    cJSON_Delete(root);
    if (cn < 1 || cn > 12) return http_send_err(req, 400, "invalid_class");
    db_class_add(cn);
    return http_send_ok(req);
}

/* GET /api/classes/delete?c=7 */
esp_err_t api_classes_delete_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char buf[8] = {0};
    if (!http_query_param(req, "c", buf, sizeof(buf)))
        return http_send_err(req, 400, "missing_c");
    int cn = atoi(buf);
    /* If this was the selected class, clear it */
    if (auth_get_selected_class() == cn) auth_set_selected_class(0);
    db_class_delete(cn);
    return http_send_ok(req);
}

/* GET /api/set-class?c=7 */
esp_err_t api_set_class_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char buf[8] = {0};
    if (!http_query_param(req, "c", buf, sizeof(buf)))
        return http_send_err(req, 400, "missing_c");
    int cn = atoi(buf);
    if (!db_class_exists(cn)) return http_send_err(req, 400, "invalid_class");
    auth_set_selected_class(cn);
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"class\":%d}", cn);
    return http_send_json(req, 200, out);
}

/* GET /api/batches?c=7 */
esp_err_t api_batches_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char buf[8] = {0};
    if (!http_query_param(req, "c", buf, sizeof(buf)))
        return http_send_err(req, 400, "missing_c");
    char *json = db_batches_list_json(atoi(buf));
    esp_err_t ret = http_send_json(req, 200, json ? json : "[]");
    free(json);
    return ret;
}

/* POST /api/batches/add  body: {"classnum":7,"batch":"11:00"} */
esp_err_t api_batches_add_post(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char body[128] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");
    cJSON *root = cJSON_Parse(body);
    if (!root) return http_send_err(req, 400, "invalid_json");
    int cn = cJSON_GetObjectItem(root,"classnum")
           ? cJSON_GetObjectItem(root,"classnum")->valueint : 0;
    const char *batch = cJSON_IsString(cJSON_GetObjectItem(root,"batch"))
                      ? cJSON_GetObjectItem(root,"batch")->valuestring : NULL;
    esp_err_t ret;
    if (!cn || !batch || strlen(batch) < 4)
        ret = http_send_err(req, 400, "invalid_params");
    else {
        db_batch_add(cn, batch);
        ret = http_send_ok(req);
    }
    cJSON_Delete(root);
    return ret;
}

/* GET /api/batches/delete?c=7&batch=11:00 */
esp_err_t api_batches_delete_get(httpd_req_t *req)
{
    if (!auth_check(req)) return http_send_err(req, 401, "unauthorized");
    char cbuf[8]={0}, bbuf[16]={0};
    if (!http_query_param(req,"c",cbuf,sizeof(cbuf)) ||
        !http_query_param(req,"batch",bbuf,sizeof(bbuf)))
        return http_send_err(req, 400, "missing_params");
    db_batch_delete(atoi(cbuf), bbuf);
    return http_send_ok(req);
}
