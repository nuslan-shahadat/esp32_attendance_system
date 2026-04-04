#include "static_files.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "static_files";

#define SPIFFS_BASE  "/spiffs"
#define CHUNK_SIZE   2048     /* bytes sent per httpd_resp_send_chunk call */

/* ── MIME type map ─────────────────────────────────────────── */
static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    if (strcmp(dot, ".woff2")== 0) return "font/woff2";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}

/* ── Catch-all GET handler ─────────────────────────────────── */
esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* Redirect bare root to login page */
    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login.html");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    /* Security: block path traversal */
    if (strstr(uri, "..")) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Bad path");
        return ESP_OK;
    }

    /* Build SPIFFS path: /spiffs<uri>
       e.g.  /login.html  →  /spiffs/login.html  */
    /* Buffer must hold SPIFFS_BASE (7 bytes) + full URI (up to CONFIG_HTTPD_MAX_URI_LEN=512) + NUL */
    char path[520];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE, uri);

    /* Strip any query string that slipped through (shouldn't for static files) */
    char *qs = strchr(path, '?');
    if (qs) *qs = '\0';

    /* Open file — binary mode is required; text mode can corrupt binary assets
       and causes platform-specific newline translation that breaks CSS/JS on
       some toolchain builds. */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", path);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<!DOCTYPE html><html><body style='font-family:monospace;padding:2rem'>"
            "<h2 style='color:#e05c5c'>404 — File not found</h2>"
            "<p>Run <code>pio run -t uploadfs</code> to flash the frontend files.</p>"
            "<p><a href='/login.html'>Go to login</a></p>"
            "</body></html>");
        return ESP_OK;
    }

    /* Send MIME type and cache headers */
    httpd_resp_set_type(req, mime_for(path));
    /* No-cache during development — change to "max-age=3600" for production */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    /* Stream file in chunks (SPIFFS files can be large) */
    char *chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }

    size_t bytes_read;
    size_t total_bytes = 0;
    esp_err_t ret = ESP_OK;
    while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
        total_bytes += bytes_read;
        if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            ESP_LOGW(TAG, "Client disconnected while sending %s", path);
            ret = ESP_FAIL;
            break;
        }
    }
    ESP_LOGI(TAG, "Served %s — %zu bytes sent", path, total_bytes);

    fclose(f);
    free(chunk);

    /* Terminate chunked transfer */
    httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}
