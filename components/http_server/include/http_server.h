#pragma once
#include "esp_http_server.h"

/* Start the HTTP server and register all routes.
   Returns the server handle, or NULL on failure. */
httpd_handle_t http_server_start(void);

/* Utility: send a JSON response.
   json_str must be a null-terminated C string of valid JSON. */
esp_err_t http_send_json(httpd_req_t *req, int status, const char *json_str);

/* Utility: send a simple {"ok":true} or {"error":"msg"} */
esp_err_t http_send_ok(httpd_req_t *req);
esp_err_t http_send_err(httpd_req_t *req, int http_status, const char *msg);

/* Utility: read the full request body into a caller-supplied buffer.
   Returns the number of bytes read, or -1 on error. */
int http_read_body(httpd_req_t *req, char *buf, size_t buf_len);

/* Utility: extract a query parameter value by name.
   Writes into out_buf (max out_len bytes). Returns true on success. */
bool http_query_param(httpd_req_t *req, const char *key,
                      char *out_buf, size_t out_len);
