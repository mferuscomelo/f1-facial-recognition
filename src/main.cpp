// F1 facecam - app entry. Stages are gated by Kconfig (CONFIG_APP_ENABLE_*).
#include <cstdio>
#include <string>

#include "board.hpp"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_APP_ENABLE_LTE
#include "lte.hpp"
#endif
#if CONFIG_APP_ENABLE_CAMERA
#include "camera.hpp"
#include "esp_timer.h"
#endif
#if CONFIG_APP_ENABLE_INFERENCE
#include <atomic>
#include "inference.hpp"
#endif

static const char* TAG = "app";

#if CONFIG_APP_ENABLE_CAMERA && CONFIG_APP_ENABLE_INFERENCE
static std::atomic<bool> s_face_present{false};

// Stage 2+3: camera frame -> Edge Impulse -> debounced face/no_face events over LTE.
// Privacy: frames stay in RAM; only label/confidence/timing leave the device.
static void inference_task(void*) {
    // Arena (~150 KB) in PSRAM: ~240 ms/inference is plenty (>3/s) and keeps internal RAM for
    // USB host + PPP/lwIP (+ TLS later). See docs/memory-budget.md.
    inference::set_arena_policy(inference::ArenaPolicy::Psram);
    if (inference::init() != ESP_OK) {
        ESP_LOGE(TAG, "inference::init failed");
        vTaskDelete(nullptr);
        return;
    }
    const int w = camera::model_width(), h = camera::model_height();
    if (w != inference::input_width() || h != inference::input_height()) {
        ESP_LOGE(TAG, "camera model size %dx%d != model input %dx%d", w, h, inference::input_width(),
                 inference::input_height());
        vTaskDelete(nullptr);
        return;
    }
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(size_t(w) * h, MALLOC_CAP_SPIRAM));
    if (!frame) {
        ESP_LOGE(TAG, "no PSRAM for inference frame");
        vTaskDelete(nullptr);
        return;
    }
    constexpr int kDebounce = 2;             // consecutive frames before a state change counts
    constexpr int64_t kMinEventGapUs = 10'000'000;  // max 1 event / 10 s
    bool state = false;
    int streak = 0;
    int64_t last_event = -kMinEventGapUs, t_stats = esp_timer_get_time();
    uint32_t n = 0, n_face = 0;
    uint64_t dsp = 0, inf = 0;
    float best_conf = 0;
    bool psram = false;
    for (;;) {
        camera::FrameInfo fi{};
        if (camera::get_latest(frame, size_t(w) * h, &fi, pdMS_TO_TICKS(1000)) == ESP_OK) {
            const auto r = inference::run(frame, w, h);
            if (r.err == 0) {
                ++n;
                dsp += r.dsp_ms;
                inf += r.inference_ms;
                psram = r.arena_in_psram;
                const bool face = !r.boxes.empty();
                if (face) {
                    ++n_face;
                    if (r.confidence > best_conf) best_conf = r.confidence;
                }
                streak = (face != state) ? streak + 1 : 0;
                const int64_t now = esp_timer_get_time();
                if (streak >= kDebounce && now - last_event >= kMinEventGapUs) {
                    state = face;
                    streak = 0;
                    last_event = now;
                    s_face_present = state;
                    ESP_LOGI(TAG, "EVENT %s (conf %.2f, %u boxes, inf %d ms)", state ? "face" : "no_face",
                             r.confidence, unsigned(r.boxes.size()), r.inference_ms);
#if CONFIG_APP_ENABLE_LTE
                    char json[256];
                    snprintf(json, sizeof(json),
                             "{\"device\":\"f1\",\"event\":\"%s\",\"ts\":%lld,\"label\":\"%s\","
                             "\"confidence\":%.2f,\"boxes\":%u,\"inference_ms\":%d,\"rssi\":%d}",
                             state ? "face" : "no_face", (long long)(now / 1000000), r.label.c_str(),
                             r.confidence, unsigned(r.boxes.size()), r.inference_ms, lte::info().rssi);
                    lte::publish_async(json);
#endif
                }
            } else {
                ESP_LOGW(TAG, "run_classifier error %d", r.err);
            }
        }
        const int64_t now = esp_timer_get_time();
        if (now - t_stats >= 5'000'000) {
            const float dt = float(now - t_stats) / 1e6f;
            ESP_LOGI(TAG, "infer %.1f/s | dsp %.1f ms, inf %.1f ms (arena %s) | face in %lu/%lu, best %.2f | state %s | heap int %u min %u",
                     n / dt, n ? double(dsp) / n : 0.0, n ? double(inf) / n : 0.0, psram ? "PSRAM" : "internal",
                     (unsigned long)n_face, (unsigned long)n, best_conf, state ? "FACE" : "none",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            t_stats = now;
            n = n_face = 0;
            dsp = inf = 0;
            best_conf = 0;
        }
    }
}
#endif

#if CONFIG_APP_ENABLE_CAMERA && !CONFIG_APP_ENABLE_INFERENCE
// Stage 3 hook: pull the latest grayscale model-size frame and log frame rate, decode time and
// heap. Frames stay in RAM only (privacy: nothing is stored or uploaded). Stage 4 replaces this
// with the inference consumer.
static void camera_stats_task(void*) {
    const size_t n = size_t(camera::model_width()) * camera::model_height();
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
    if (!frame) {
        ESP_LOGE(TAG, "camera hook: no PSRAM for frame buffer");
        vTaskDelete(nullptr);
        return;
    }
    const int64_t period_us = int64_t(CONFIG_APP_CAMERA_STATS_PERIOD_S) * 1000000;
    int64_t t_last = esp_timer_get_time();
    uint32_t got = 0, last_rx = 0;
    uint64_t dec_us = 0, rsz_us = 0;
    uint32_t mean = 0;
    for (;;) {
        camera::FrameInfo fi{};
        if (camera::get_latest(frame, n, &fi, pdMS_TO_TICKS(1000)) == ESP_OK) {
            ++got;
            dec_us += fi.decode_us;
            rsz_us += fi.resize_us;
            uint32_t sum = 0;  // mean luma: cheap sanity check that the image is not black/blank
            for (size_t i = 0; i < n; ++i) sum += frame[i];
            mean = sum / n;
        }
        const int64_t now = esp_timer_get_time();
        if (now - t_last < period_us) continue;
        const float dt = float(now - t_last) / 1e6f;
        const auto s = camera::stats();
        ESP_LOGI(TAG,
                 "cam %s %ux%u@%.1f | rx %.1f fps, out %.1f fps | decode %.1f ms, resize %.1f ms | "
                 "drop %lu big %lu err %lu drv %lu | mean %lu",
                 s.connected ? "UP" : "--", s.stream_width, s.stream_height, s.stream_fps,
                 (s.frames_received - last_rx) / dt, got / dt, got ? dec_us / 1000.0 / got : 0.0,
                 got ? rsz_us / 1000.0 / got : 0.0, (unsigned long)s.frames_dropped,
                 (unsigned long)s.frames_too_big, (unsigned long)s.decode_errors,
                 (unsigned long)s.driver_drops, (unsigned long)mean);
        ESP_LOGI(TAG, "cam heap int free %u (min %u, largest %u) | psram free %u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        t_last = now;
        last_rx = s.frames_received;
        got = 0;
        dec_us = rsz_us = 0;
    }
}
#endif

extern "C" void app_main() {
    ESP_LOGI(TAG, "F1 facecam booting");
    board::print_chip_info();
    board::log_heap_once();
    board::start_heap_telemetry(CONFIG_APP_HEAP_LOG_PERIOD_S);
    if (!board::led_init()) ESP_LOGW(TAG, "LED unavailable");
    board::led_set(16, 16, 0);  // yellow: booting

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_APP_ENABLE_CAMERA
    if (esp_err_t e = camera::start(); e != ESP_OK) {
        ESP_LOGE(TAG, "camera::start failed: %s", esp_err_to_name(e));
    } else {
#if CONFIG_APP_ENABLE_INFERENCE
        // Core 1 = camera + inference, core 0 = LTE (HANDOFF Stage 4.1). EI needs a deep stack.
        xTaskCreatePinnedToCore(inference_task, "infer", 16384, nullptr, 3, nullptr, 1);
#else
        xTaskCreatePinnedToCore(camera_stats_task, "cam_stats", 3072, nullptr, 3, nullptr,
                                CONFIG_APP_CAMERA_TASK_CORE);
#endif
    }
#endif

#if CONFIG_APP_ENABLE_LTE
    if (esp_err_t e = lte::start(); e != ESP_OK) {
        ESP_LOGE(TAG, "lte::start failed: %s", esp_err_to_name(e));
        board::led_set(32, 0, 0);
    }
    TickType_t last_pub = 0;
    bool first = true;
    for (unsigned i = 0;; ++i) {
        const bool up = lte::is_connected();
        // LED: blue blink = attaching, solid green = connected
#if CONFIG_APP_ENABLE_CAMERA && CONFIG_APP_ENABLE_INFERENCE
        if (s_face_present) board::led_set(24, 0, 24);  // magenta: face detected
        else
#endif
        if (up) board::led_set(0, 24, 0);
        else board::led_set(0, 0, (i & 1) ? 24 : 0);

        const TickType_t now = xTaskGetTickCount();
        if (up && (first || now - last_pub >= pdMS_TO_TICKS(CONFIG_APP_PUBLISH_PERIOD_S * 1000))) {
            const auto inf = lte::info();
            char json[320];
            snprintf(json, sizeof(json),
                     "{\"device\":\"f1\",\"event\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d,\"rat\":\"%s\","
                     "\"reconnects\":%u,\"pub_fail\":%u,\"heap_int\":%u,\"heap_int_min\":%u,\"heap_psram\":%u}",
                     first ? "boot" : "heartbeat", (unsigned long)(now / configTICK_RATE_HZ), inf.rssi,
                     inf.rat, inf.reconnects, inf.publish_fail, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            // Non-blocking; the boot event is must-deliver (retried by lte_pub until a 2xx).
            if (lte::publish_async(json, /*must_deliver=*/first)) first = false;
            last_pub = now;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#else
    const uint8_t colors[][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}};
    for (unsigned i = 0;; ++i) {
        const auto& c = colors[i % 3];
        board::led_set(c[0], c[1], c[2]);
        vTaskDelay(pdMS_TO_TICKS(500));
        board::led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#endif
}
