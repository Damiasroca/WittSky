#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

#include <stdbool.h>

bool guest_json(httpd_req_t *req);
esp_err_t send_json(httpd_req_t *req, cJSON *o);
char *recv_body(httpd_req_t *req);
void register_get(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *));
void register_post(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *));

void www_api_register(httpd_handle_t http, httpd_handle_t stream);
void www_sky_register(httpd_handle_t http);
