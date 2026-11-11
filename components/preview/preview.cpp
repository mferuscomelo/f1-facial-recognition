// Live preview: Wi-Fi access point + a tiny web server.
//
//   GET /           the page (index.html, built into the firmware)
//   GET /frame.jpg  the newest camera frame, with the latest detections in the
//                   "X-Detections" header, so the page can draw the boxes on that frame
//
// The JPEG comes straight from the webcam, so the F1 never re-encodes an image.
#include "preview.hpp"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if !CONFIG_APP_PREVIEW
namespace preview {
esp_err_t start() { return ESP_OK; }
void update(const inference::Result&) {}
}  // namespace preview
#else

#include "camera.hpp"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

extern const char index_html_start[] asm("_binary_index_html_start");

namespace preview {
namespace {
const char* TAG = "preview";

SemaphoreHandle_t s_lock;
char s_detections[400] = "{\"size\":96,\"faces\":[]}";  // guarded by s_lock
uint8_t* s_jpeg;                                          // only used by the web server task

esp_err_t start_access_point() {
    if (strlen(CONFIG_APP_PREVIEW_PASSWORD) < 8) {
        ESP_LOGE(TAG, "CONFIG_APP_PREVIEW_PASSWORD needs at least 8 characters: preview off");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nvs_flash_init();  // Wi-Fi keeps calibration data in flash
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    esp_netif_create_default_wifi_ap();  // gives phones an address via DHCP; the F1 is 192.168.4.1
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;

    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char*>(cfg.ap.ssid), CONFIG_APP_PREVIEW_SSID, sizeof(cfg.ap.ssid));
    strlcpy(reinterpret_cast<char*>(cfg.ap.password), CONFIG_APP_PREVIEW_PASSWORD, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(CONFIG_APP_PREVIEW_SSID);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    return esp_wifi_start();
}

esp_err_t page(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, index_html_start);  // EMBED_TXTFILES adds a trailing '\0' 
}

esp_err_t frame(httpd_req_t* req) {
    const size_t len = camera::latest_jpeg(s_jpeg, camera::kMaxJpegBytes);
    if (len == 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "no camera frame yet");
    }
    char detections[sizeof(s_detections)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(detections, s_detections, sizeof(detections));
    xSemaphoreGive(s_lock);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Detections", detections);
    return httpd_resp_send(req, reinterpret_cast<const char*>(s_jpeg), len);
}
}  // namespace

esp_err_t start() {
    s_lock = xSemaphoreCreateMutex();
    s_jpeg = static_cast<uint8_t*>(heap_caps_malloc(camera::kMaxJpegBytes, MALLOC_CAP_SPIRAM));
    if (!s_jpeg) return ESP_ERR_NO_MEM;
    if (esp_err_t e = start_access_point(); e != ESP_OK) return e;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0;               // core 1 is busy with the camera and the model
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;   // phones leave idle connections open
    httpd_handle_t server;
    if (esp_err_t e = httpd_start(&server, &cfg); e != ESP_OK) return e;
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, page, nullptr},
        {"/frame.jpg", HTTP_GET, frame, nullptr},
    };
    for (const auto& r : routes) httpd_register_uri_handler(server, &r);
    ESP_LOGI(TAG, "join Wi-Fi \"%s\" and open http://192.168.4.1", CONFIG_APP_PREVIEW_SSID);
    return ESP_OK;
}

void update(const inference::Result& r) {
    if (!s_lock) return;  // preview not running
    // {"size":96,"faces":[[x,y,w,h,score],...]}  (input pixels)
    char json[sizeof(s_detections)];
    int n = snprintf(json, sizeof(json), "{\"size\":%d,\"ms\":%d,\"faces\":[", inference::input_width(), r.time_ms);
    for (int i = 0; i < r.count && n < int(sizeof(json)) - 40; ++i) {
        const auto& b = r.boxes[i];
        n += snprintf(json + n, sizeof(json) - n, "%s[%d,%d,%d,%d,%.2f]", i ? "," : "", b.x, b.y, b.width,
                      b.height, b.score);
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_detections, json, sizeof(json));
    xSemaphoreGive(s_lock);
}

}  // namespace preview
#endif
