#pragma once
#include "esp_http_server.h"

// Web handlers for firmware updates (update.cpp), used by preview.cpp.
namespace preview {
esp_err_t handle_info(httpd_req_t* req);
esp_err_t handle_update(httpd_req_t* req);
}  // namespace preview
