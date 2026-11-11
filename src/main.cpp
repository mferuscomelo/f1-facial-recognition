// F1 FaceCam: USB webcam -> Edge Impulse face detection -> JSON event over LTE-M.
//
//   core 1: ai_task    grabs a frame, runs the model, decides "face" / "no face"
//   core 0: lte        keeps the cellular link up and sends the events
//
// Privacy: images never leave the device. Only the result (face yes/no, score) is sent.
#include <cstdio>

#include "camera.hpp"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inference.hpp"
#include "led_strip.h"
#include "lte.hpp"
#include "sdkconfig.h"

static const char* TAG = "app";
static led_strip_handle_t s_led;
static volatile bool s_face = false;

static void led(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

// The F1's RGB LED is a single WS2812 on GPIO21.
static void led_init() {
    led_strip_config_t strip = {};
    strip.strip_gpio_num = 21;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &s_led));
}

static void send_event(const char* event, const inference::Result& r) {
    char json[200];
    snprintf(json, sizeof(json),
             "{\"device\":\"f1\",\"event\":\"%s\",\"confidence\":%.2f,\"faces\":%d,"
             "\"inference_ms\":%d,\"uptime_s\":%lld,\"csq\":%d}",
             event, r.confidence, r.count, r.time_ms, esp_timer_get_time() / 1000000, lte::signal_quality());
    lte::publish(json);
}

static void ai_task(void*) {
    inference::self_test();  // known photos first: proves the model works before the camera does

    const int w = inference::input_width(), h = inference::input_height();
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM));
    int changed_frames = 0;      // frames in a row that disagree with the current state
    int64_t last_event_us = 0;
    for (;;) {
        vTaskDelay(1);  // give the idle task a moment, or the watchdog complains
        if (camera::capture(frame, w, h, pdMS_TO_TICKS(1000)) != ESP_OK) continue;
        const inference::Result r = inference::run(frame);
        if (!r.ok) continue;
        uint32_t brightness = 0;  // average pixel value: handy to spot a covered lens or a dark room
        for (int i = 0; i < w * h; ++i) brightness += frame[i];
        ESP_LOGI(TAG, "%s  score %.2f  at (%d,%d)  %d ms  brightness %lu", r.face ? "FACE   " : "no face",
                 r.confidence, r.x, r.y, r.time_ms, (unsigned long)(brightness / (w * h)));

        // Report a change only after 2 frames agree, and at most every 10 s.
        changed_frames = (r.face != s_face) ? changed_frames + 1 : 0;
        const int64_t now = esp_timer_get_time();
        if (changed_frames >= 2 && now - last_event_us >= 10 * 1000000) {
            s_face = r.face;
            changed_frames = 0;
            last_event_us = now;
            send_event(s_face ? "face" : "no_face", r);
        }
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "F1 FaceCam starting");
    led_init();
    led(16, 16, 0);  // yellow: starting
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(camera::start());
    xTaskCreatePinnedToCore(ai_task, "ai", 16384, nullptr, 3, nullptr, 1);  // the model needs a deep stack
    ESP_ERROR_CHECK(lte::start());

    // Status LED + heartbeat. Magenta: face in view. Green: online. Blue blink: connecting.
    bool sent_boot = false;
    int64_t last_heartbeat_us = 0;
    for (int i = 0;; ++i) {
        const bool online = lte::is_connected();
        if (s_face) led(24, 0, 24);
        else if (online) led(0, 24, 0);
        else led(0, 0, (i % 2) ? 24 : 0);

        const int64_t now = esp_timer_get_time();
        if (online && (!sent_boot || now - last_heartbeat_us >= CONFIG_APP_HEARTBEAT_PERIOD_S * 1000000LL)) {
            char json[160];
            snprintf(json, sizeof(json),
                     "{\"device\":\"f1\",\"event\":\"%s\",\"uptime_s\":%lld,\"csq\":%d,\"free_heap\":%u}",
                     sent_boot ? "heartbeat" : "boot", now / 1000000, lte::signal_quality(),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            lte::publish(json);
            sent_boot = true;
            last_heartbeat_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
