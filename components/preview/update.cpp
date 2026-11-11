// Firmware update from the web page ("over the air", OTA).
//
//   GET  /info     {"version":"...","built":"...","slot":"ota_0"}
//   POST /update   body = firmware.bin from `pio run` (.pio/build/f1/firmware.bin)
//
// The flash has two firmware slots (partitions.csv). The new firmware goes into the slot that
// is not running; if it is complete and valid, the F1 restarts into it. The new firmware then
// has to confirm it works (main.cpp, after its self-test). If it crashes before that, the
// bootloader goes back to the old one.
#include "update.hpp"

#include "sdkconfig.h"
#if CONFIG_APP_PREVIEW

#include <cstdio>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace preview {
namespace {
const char* TAG = "update";
}

esp_err_t handle_info(httpd_req_t* req) {
    const esp_app_desc_t* app = esp_app_get_description();
    char json[200];
    snprintf(json, sizeof(json), "{\"version\":\"%s\",\"built\":\"%s %s\",\"slot\":\"%s\"}", app->version, app->date,
             app->time, esp_ota_get_running_partition()->label);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

esp_err_t handle_update(httpd_req_t* req) {
    const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t ota = 0;
    esp_err_t err = slot ? esp_ota_begin(slot, req->content_len, &ota) : ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, slot ? slot->label : "?");

    static char buf[4096];
    for (int left = req->content_len, timeouts = 0; err == ESP_OK && left > 0;) {
        const int n = httpd_req_recv(req, buf, left < int(sizeof(buf)) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;  // slow phone: wait a bit longer
        if (n <= 0) err = ESP_ERR_TIMEOUT;
        else err = esp_ota_write(ota, buf, n);  // also checks that this is an ESP32-S3 firmware
        left -= n > 0 ? n : 0;
    }
    if (ota) {
        // esp_ota_end() checks the whole image (checksum, hash) before we may boot it.
        const esp_err_t end = esp_ota_end(ota);
        if (err == ESP_OK) err = end;
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "update OK, restarting into %s", slot->label);
    httpd_resp_sendstr(req, "ok, restarting");
    xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); }, "restart", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

}  // namespace preview
#endif
