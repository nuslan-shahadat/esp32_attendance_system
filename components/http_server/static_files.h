#pragma once
#include "esp_http_server.h"

/* Catch-all GET handler — serves files from /spiffs/<uri>.
   Must be registered LAST so named /api/... routes take priority. */
esp_err_t static_file_handler(httpd_req_t *req);
