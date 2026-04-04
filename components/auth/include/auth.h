#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

#define AUTH_TOKEN_LEN  32

/* Verify the admin password. Returns true if correct. */
bool auth_verify_password(const char *password);

/* Generate a new session token and store it internally.
   Writes the token string into out_buf (must be AUTH_TOKEN_LEN+1 bytes). */
void auth_create_session(char *out_buf, size_t buf_len);

/* Invalidate a session token. */
void auth_destroy_session(const char *token);

/* Check if the incoming request has a valid, non-expired session cookie.
   Returns true on success. Does NOT send any HTTP response — caller decides. */
bool auth_check(httpd_req_t *req);

/* Same as auth_check but also verifies a class is selected.
   Returns false if either check fails. */
bool auth_check_with_class(httpd_req_t *req);

/* Refresh the session expiry (call on every authenticated request). */
void auth_refresh(void);

/* Get/set the currently selected class (0 = none). */
int  auth_get_selected_class(void);
void auth_set_selected_class(int class_num);
