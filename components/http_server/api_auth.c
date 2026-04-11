#include "http_server.h"
#include "auth.h"
#include "db.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "api_auth";

/* ── POST /api/login ─────────────────────────────────────────
   Body (JSON): {"password": "1234"}
   Response:    {"ok": true}   +  Set-Cookie: sid=<token>
               {"error": "wrong_password"}  on failure        */
esp_err_t api_login_post(httpd_req_t *req)
{
    char body[128] = {0};
    if (http_read_body(req, body, sizeof(body)) < 0)
        return http_send_err(req, 400, "body_too_large");

    cJSON *root = cJSON_Parse(body);
    if (!root)
        return http_send_err(req, 400, "invalid_json");

    cJSON *pw_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *password = cJSON_IsString(pw_item) ? pw_item->valuestring : "";

    esp_err_t ret;
    if (auth_verify_password(password)) {
        char token[AUTH_TOKEN_LEN + 1];
        auth_create_session(token, sizeof(token));

        /* Set-Cookie header */
        char cookie[80];
        snprintf(cookie, sizeof(cookie),
                 "sid=%s; Path=/; HttpOnly; SameSite=Strict", token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);

        ESP_LOGI(TAG, "Login OK");

        /* Re-mount SD card on every login so the DB is fresh (same as
           the admin-panel "Re-mount SD" button).  Non-fatal: if the
           remount fails we still complete the login and let the user
           retry manually from the admin panel. */
        int rm_rc = db_sd_remount();
        if (rm_rc != ESP_OK)
            ESP_LOGW(TAG, "Post-login SD remount failed (rc=%d) — continuing", rm_rc);

        ret = http_send_json(req, 200, "{\"ok\":true}");
    } else {
        ESP_LOGW(TAG, "Login failed — wrong password");
        ret = http_send_err(req, 401, "wrong_password");
    }

    cJSON_Delete(root);
    return ret;
}

/* ── POST /api/logout ────────────────────────────────────────
   Destroys the session, clears the cookie.                   */
esp_err_t api_logout_post(httpd_req_t *req)
{
    /* Extract token from Cookie header (best-effort) */
    char cookie_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie",
                                    cookie_hdr, sizeof(cookie_hdr)) == ESP_OK) {
        char *sid = strstr(cookie_hdr, "sid=");
        if (sid) {
            char token[AUTH_TOKEN_LEN + 1];
            snprintf(token, sizeof(token), "%s", sid + 4);
            /* Truncate at semicolon or space */
            char *end = strpbrk(token, "; ");
            if (end) *end = '\0';
            auth_destroy_session(token);
        }
    }

    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    return http_send_ok(req);
}

/* ── GET /api/session ────────────────────────────────────────
   Returns current session info so the navbar can show class.
   {"ok": true, "class": 7}   or  {"error": "unauthorized"} */
esp_err_t api_session_get(httpd_req_t *req)
{
    if (!auth_check(req))
        return http_send_err(req, 401, "unauthorized");

    int cls = auth_get_selected_class();
    char buf[64];
    if (cls > 0)
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"class\":%d}", cls);
    else
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"class\":null}");

    return http_send_json(req, 200, buf);
}
