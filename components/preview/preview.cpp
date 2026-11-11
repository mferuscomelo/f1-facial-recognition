// Live preview: Wi-Fi access point + a tiny web server.
//
//   GET  /                    the page (index.html, built into the firmware)
//   GET  /frame.jpg           the newest camera frame, with the latest detections in the
//                             "X-Detections" header, so the page can draw the boxes on that frame
//   GET  /people              who the F1 knows: {"people":[{"id":0,"name":"Ada","samples":5}],"enrolling":""}
//   POST /people/add?name=    register someone (the camera loop takes the pictures)
//   POST /people/cancel       stop registering
//   POST /people/rename?id=&name=
//   POST /people/delete?id=
//   GET  /info, POST /update  firmware version and firmware update (update.cpp)
//
// The JPEG comes straight from the webcam, so the F1 never re-encodes an image.
#include "preview.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sdkconfig.h"

#if !CONFIG_APP_PREVIEW
namespace preview {
esp_err_t start() { return ESP_OK; }
void update(const inference::Result&, const people::Match*, const char*) {}
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
#include "update.hpp"

extern const char index_html_start[] asm("_binary_index_html_start");

namespace preview {
namespace {
const char* TAG = "preview";

SemaphoreHandle_t s_lock;
char s_detections[800] = "{\"size\":96,\"faces\":[]}";  // guarded by s_lock
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
// Percent-encodes text for the X-Detections header (headers must be plain ASCII).
void url_encode(const char* in, char* out, size_t len) {
    size_t n = 0;
    for (; *in && n + 4 < len; ++in) {
        const unsigned char c = *in;
        if (isalnum(c) || c == ' ' || c == '.' || c == ',' || c == ':' || c == '-') out[n++] = c;
        else n += snprintf(out + n, len - n, "%%%02X", c);
    }
    out[n] = 0;
}

// Reads ?key=value from the URL and undoes the browser's percent-encoding.
bool query(httpd_req_t* req, const char* key, char* out, size_t len) {
    char q[160], raw[100];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, raw, sizeof(raw)) != ESP_OK) return false;
    size_t n = 0;
    for (const char* p = raw; *p && n + 1 < len; ++p) {
        if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
            const char hex[3] = {p[1], p[2], 0};
            out[n++] = char(strtol(hex, nullptr, 16));
            p += 2;
        } else {
            out[n++] = *p == '+' ? ' ' : *p;
        }
    }
    out[n] = 0;
    return true;
}

// A name from the page: 1-31 bytes, no quotes, backslashes or control characters.
bool valid_name(const char* name) {
    const size_t len = strlen(name);
    if (len == 0 || len >= people::kNameLen) return false;
    for (const char* c = name; *c; ++c)
        if (*c == '"' || *c == '\\' || *c == '<' || *c == '>' || static_cast<unsigned char>(*c) < 0x20) return false;
    return true;
}

esp_err_t reply(httpd_req_t* req, esp_err_t result) {
    if (result != ESP_OK) httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, result == ESP_OK ? "ok" : esp_err_to_name(result));
}

esp_err_t people_list(httpd_req_t* req) {
    static char json[people::kMax * 80 + 100];
    char enrolling[people::kNameLen];
    people::enroll_active(enrolling);
    int n = snprintf(json, sizeof(json), "{\"people\":");
    n += people::to_json(json + n, sizeof(json) - n - 40);
    snprintf(json + n, sizeof(json) - n, ",\"enrolling\":\"%s\"}", enrolling);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t people_add(httpd_req_t* req) {
    char name[people::kNameLen];
    if (!query(req, "name", name, sizeof(name)) || !valid_name(name)) return reply(req, ESP_ERR_INVALID_ARG);
    people::enroll_start(name);
    return reply(req, ESP_OK);
}

esp_err_t people_cancel(httpd_req_t* req) {
    people::enroll_stop();
    return reply(req, ESP_OK);
}

esp_err_t people_rename(httpd_req_t* req) {
    char id[8], name[people::kNameLen];
    if (!query(req, "id", id, sizeof(id)) || !query(req, "name", name, sizeof(name)) || !valid_name(name))
        return reply(req, ESP_ERR_INVALID_ARG);
    return reply(req, people::rename(atoi(id), name));
}

esp_err_t people_delete(httpd_req_t* req) {
    char id[8];
    if (!query(req, "id", id, sizeof(id))) return reply(req, ESP_ERR_INVALID_ARG);
    return reply(req, people::remove(atoi(id)));
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
    cfg.max_uri_handlers = 12;
    httpd_handle_t server;
    if (esp_err_t e = httpd_start(&server, &cfg); e != ESP_OK) return e;
    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, page, nullptr},
        {"/frame.jpg", HTTP_GET, frame, nullptr},
        {"/people", HTTP_GET, people_list, nullptr},
        {"/people/add", HTTP_POST, people_add, nullptr},
        {"/people/cancel", HTTP_POST, people_cancel, nullptr},
        {"/people/rename", HTTP_POST, people_rename, nullptr},
        {"/people/delete", HTTP_POST, people_delete, nullptr},
        {"/info", HTTP_GET, handle_info, nullptr},
        {"/update", HTTP_POST, handle_update, nullptr},
    };
    for (const auto& r : routes) httpd_register_uri_handler(server, &r);
    ESP_LOGI(TAG, "join Wi-Fi \"%s\" and open http://192.168.4.1", CONFIG_APP_PREVIEW_SSID);
    return ESP_OK;
}

void update(const inference::Result& r, const people::Match* who, const char* note) {
    if (!s_lock) return;  // preview not running
    // {"size":96,"ms":157,"faces":[[x,y,w,h,score],...],"who":{"name":"Ada","sim":0.71},"note":"..."}
    // Boxes are in model input pixels; names and notes are percent-encoded.
    char json[sizeof(s_detections)], text[240];
    int n = snprintf(json, sizeof(json), "{\"size\":%d,\"ms\":%d,\"faces\":[", inference::input_width(), r.time_ms);
    for (int i = 0; i < r.count && n < int(sizeof(json)) - 40; ++i) {
        const auto& b = r.boxes[i];
        n += snprintf(json + n, sizeof(json) - n, "%s[%d,%d,%d,%d,%.2f]", i ? "," : "", b.x, b.y, b.width,
                      b.height, b.score);
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"best\":[%d,%d]", r.x, r.y);
    if (who) {
        url_encode(who->name, text, sizeof(text));
        n += snprintf(json + n, sizeof(json) - n, ",\"who\":{\"name\":\"%s\",\"sim\":%.2f}", text, who->similarity);
    }
    url_encode(note, text, sizeof(text));
    snprintf(json + n, sizeof(json) - n, ",\"note\":\"%s\"}", text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_detections, json, sizeof(json));
    xSemaphoreGive(s_lock);
}

}  // namespace preview
#endif
