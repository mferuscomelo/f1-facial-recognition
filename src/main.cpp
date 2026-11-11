// F1 FaceCam: USB webcam -> Edge Impulse face detection (+ recognition) -> JSON event over LTE-M.
//
//   core 1: ai_task    grabs a frame, finds faces, works out who it is, decides when to report
//   core 0: lte        keeps the cellular link up and sends the events
//           preview    optional Wi-Fi access point + web page (live view, register people)
//
// Privacy: only the result (face yes/no, score, name) goes over LTE. Images stay on the device,
// unless you turn on the local Wi-Fi preview (CONFIG_APP_PREVIEW).
#include <cstdio>

#include "camera.hpp"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "face_id.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inference.hpp"
#include "led_strip.h"
#include "lte.hpp"
#include "people.hpp"
#include "preview.hpp"
#include "sdkconfig.h"

static const char* TAG = "app";
static led_strip_handle_t s_led;
static volatile bool s_face = false;   // someone is in view (after debouncing)
static volatile bool s_known = false;  // ... and we know who it is

// Face recognition needs a picture of the face. FOMO (the detector) finds the centre of each face,
// but not its size, so we cut out a fixed square: half the height of the camera image. That fits a
// face about an arm's length (50 cm) from the webcam.
constexpr float kFaceCrop = 0.5f;
constexpr int64_t kRecheckUs = 10 * 1000000;  // look again at who it is every 10 s

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

static void send_event(const char* event, const inference::Result& r, const people::Match& who) {
    char name[people::kNameLen + 2] = "null";  // JSON null = not recognised
    if (who.id >= 0) snprintf(name, sizeof(name), "\"%s\"", who.name);
    char json[300];
    snprintf(json, sizeof(json),
             "{\"device\":\"f1\",\"event\":\"%s\",\"name\":%s,\"similarity\":%.2f,\"confidence\":%.2f,"
             "\"faces\":%d,\"inference_ms\":%d,\"uptime_s\":%lld,\"csq\":%d}",
             event, name, who.similarity, r.confidence, r.count, r.time_ms, esp_timer_get_time() / 1000000,
             lte::signal_quality());
    lte::publish(json);
}

// Cuts the best face out of the current frame and computes its fingerprint (~2.4 s).
static bool fingerprint(const inference::Result& r, float* out) {
    static uint8_t* face = static_cast<uint8_t*>(heap_caps_malloc(face_id::kSize * face_id::kSize * 3, MALLOC_CAP_SPIRAM));
    const float size = inference::input_width();
    if (camera::crop_face(r.x / size, r.y / size, kFaceCrop, face, face_id::kSize) != ESP_OK) return false;
    const int ms = face_id::embed(face, out);
    ESP_LOGI(TAG, "fingerprint of face at (%d,%d): %d ms", r.x, r.y, ms);
    return ms >= 0;
}

// One step of registering a person: needs exactly one clear face in view.
static void enroll_step(const inference::Result& r, const char* name, float* fp) {
    static int taken = 0;
    char note[96];
    if (r.count != 1 || r.confidence < 0.8f) {
        snprintf(note, sizeof(note), "Registering %s: %s", name, r.count > 1 ? "only one face, please" : "look at the camera");
        preview::update(r, nullptr, note);
        return;
    }
    snprintf(note, sizeof(note), "Registering %s: picture %d of %d, hold still", name, taken + 1, people::kEnrollSamples);
    preview::update(r, nullptr, note);
    if (!fingerprint(r, fp) || people::add_sample(name, fp) != ESP_OK) {
        ESP_LOGE(TAG, "could not register %s (list full?)", name);
        people::enroll_stop();
        taken = 0;
        return;
    }
    if (++taken == people::kEnrollSamples) {
        ESP_LOGI(TAG, "registered %s", name);
        people::enroll_stop();
        taken = 0;
    }
}

static void ai_task(void*) {
    inference::self_test();  // known photos first: proves the models work before the camera does

    const int w = inference::input_width(), h = inference::input_height();
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM));
    static float fp[face_id::kDims];
    people::Match who;           // who the best face is (id -1: unknown or not checked)
    int64_t who_checked_us = 0;  // when we last looked (0: not yet, for this visit)
    int reported_id = -1;        // the `who.id` in the last "face" event
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

        // Registering someone (asked for on the web page)?
        char enrolling[people::kNameLen];
        if (face_id::available() && people::enroll_active(enrolling)) {
            enroll_step(r, enrolling, fp);
            continue;
        }

        // Who is it? Only worth the time if somebody is registered.
        const int64_t now = esp_timer_get_time();
        if (r.face && face_id::available() && people::count() > 0 &&
            (who_checked_us == 0 || now - who_checked_us > kRecheckUs)) {
            preview::update(r, nullptr, "Checking who it is...");
            if (fingerprint(r, fp)) who = people::identify(fp);
            who_checked_us = now;
            ESP_LOGI(TAG, "best face: %s (similarity %.2f)", who.id >= 0 ? who.name : "unknown", who.similarity);
        }
        preview::update(r, who_checked_us ? &who : nullptr, "");

        // Report a change only after 2 frames agree, and at most every 10 s.
        changed_frames = (r.face != s_face) ? changed_frames + 1 : 0;
        // Also report when the recognised person changes while someone is in view.
        const bool new_name = s_face && r.face && who.id != reported_id;
        if ((changed_frames >= 2 || new_name) && now - last_event_us >= 10 * 1000000) {
            s_face = r.face;
            changed_frames = 0;
            last_event_us = now;
            send_event(s_face ? "face" : "no_face", r, s_face ? who : people::Match{});
            reported_id = s_face ? who.id : -1;
            if (!s_face) {  // they left: next visit starts fresh
                who = {};
                who_checked_us = 0;
            }
        }
        s_known = s_face && who.id >= 0;
    }
}

extern "C" void app_main() {
    ESP_LOGI(TAG, "F1 FaceCam starting");
    led_init();
    led(16, 16, 0);  // yellow: starting
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(people::start());
    ESP_ERROR_CHECK(camera::start());
    xTaskCreatePinnedToCore(ai_task, "ai", 16384, nullptr, 3, nullptr, 1);  // the models need a deep stack
    ESP_ERROR_CHECK(lte::start());
    if (preview::start() != ESP_OK) ESP_LOGE(TAG, "live preview could not start");

    // Status LED + heartbeat. Cyan: someone you registered. Magenta: a face. Green: online.
    // Blue blink: connecting.
    bool sent_boot = false;
    int64_t last_heartbeat_us = 0;
    for (int i = 0;; ++i) {
        const bool online = lte::is_connected();
        if (s_known) led(0, 24, 24);
        else if (s_face) led(24, 0, 24);
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
