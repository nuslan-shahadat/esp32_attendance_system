#include "auth.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "auth";

/* ── Config ──────────────────────────────────────────────── */
#define ADMIN_PASSWORD      "1234"
#define SESSION_TIMEOUT_US  (30ULL * 60ULL * 1000000ULL)   /* 30 minutes */

/* ── State (protected by mutex) ─────────────────────────── */
static char             s_token[AUTH_TOKEN_LEN + 1] = {0};
static int64_t          s_expiry_us = 0;   /* esp_timer_get_time() epoch */
static int              s_selected_class = 0;
static SemaphoreHandle_t s_mutex = NULL;

/* ── Init (called lazily on first use) ───────────────────── */
static void ensure_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

/* ── Internal: extract "sid=" value from Cookie header ──── */
static bool extract_cookie_token(httpd_req_t *req, char *out, size_t out_len)
{
    char hdr[192] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, sizeof(hdr)) != ESP_OK)
        return false;

    char *p = strstr(hdr, "sid=");
    if (!p) return false;
    p += 4;   /* skip "sid=" */

    size_t i = 0;
    while (*p && *p != ';' && *p != ' ' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

/* ── Public API ──────────────────────────────────────────── */
bool auth_verify_password(const char *password)
{
    return password && strcmp(password, ADMIN_PASSWORD) == 0;
}

void auth_create_session(char *out_buf, size_t buf_len)
{
    ensure_init();

    /* Generate 32 random hex chars using esp_random() */
    char token[AUTH_TOKEN_LEN + 1];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < AUTH_TOKEN_LEN; i++) {
        token[i] = hex[esp_random() & 0x0F];
    }
    token[AUTH_TOKEN_LEN] = '\0';

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_token, token, sizeof(s_token) - 1);
    s_expiry_us = esp_timer_get_time() + SESSION_TIMEOUT_US;
    xSemaphoreGive(s_mutex);

    if (out_buf) snprintf(out_buf, buf_len, "%s", token);
    ESP_LOGI(TAG, "Session created");
}

void auth_destroy_session(const char *token)
{
    ensure_init();
    (void)token;   /* single-session system — just wipe the stored token */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_token, 0, sizeof(s_token));
    s_expiry_us      = 0;
    s_selected_class = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Session destroyed");
}

bool auth_check(httpd_req_t *req)
{
    ensure_init();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool token_valid = (s_token[0] != '\0') &&
                       (esp_timer_get_time() < s_expiry_us);
    xSemaphoreGive(s_mutex);

    if (!token_valid) return false;

    char req_token[AUTH_TOKEN_LEN + 1] = {0};
    if (!extract_cookie_token(req, req_token, sizeof(req_token)))
        return false;

    /* Constant-time compare to avoid timing attacks */
    bool match = true;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < AUTH_TOKEN_LEN; i++)
        if (req_token[i] != s_token[i]) match = false;
    if (match)
        s_expiry_us = esp_timer_get_time() + SESSION_TIMEOUT_US;   /* refresh */
    xSemaphoreGive(s_mutex);

    return match;
}

bool auth_check_with_class(httpd_req_t *req)
{
    if (!auth_check(req)) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool has_class = (s_selected_class > 0);
    xSemaphoreGive(s_mutex);
    return has_class;
}

void auth_refresh(void)
{
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_token[0] != '\0')
        s_expiry_us = esp_timer_get_time() + SESSION_TIMEOUT_US;
    xSemaphoreGive(s_mutex);
}

int auth_get_selected_class(void)
{
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int c = s_selected_class;
    xSemaphoreGive(s_mutex);
    return c;
}

void auth_set_selected_class(int class_num)
{
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_selected_class = class_num;
    xSemaphoreGive(s_mutex);
}
