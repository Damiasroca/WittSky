#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

#include <stdbool.h>
#include <stddef.h>

bool guest_json(httpd_req_t *req);
bool www_session_ok(httpd_req_t *req);
/* Starts a session and writes its Set-Cookie value into cookie. */
void www_session_new(char *cookie, size_t n);
esp_err_t send_json(httpd_req_t *req, cJSON *o);
char *recv_body(httpd_req_t *req);
void register_get(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *));
void register_post(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *));

/* True while a stream is open or a still was just taken. Background
 * sampling stays off the camera until it clears. */
bool hp10_www_cam_busy(void);

void www_api_register(httpd_handle_t http, httpd_handle_t stream);
void www_sky_register(httpd_handle_t http);
void www_overlay_register(httpd_handle_t http);
