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

static const char* TAG = "app";

extern "C" void app_main() {
    ESP_LOGI(TAG, "F1 facecam booting");
    board::print_chip_info();
    board::log_heap_once();
    board::start_heap_telemetry(CONFIG_APP_HEAP_LOG_PERIOD_S);
    if (!board::led_init()) ESP_LOGW(TAG, "LED unavailable");
    board::led_set(16, 16, 0);  // yellow: booting

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

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
        if (up) board::led_set(0, 24, 0);
        else board::led_set(0, 0, (i & 1) ? 24 : 0);

        const TickType_t now = xTaskGetTickCount();
        if (up && (first || now - last_pub >= pdMS_TO_TICKS(CONFIG_APP_PUBLISH_PERIOD_S * 1000))) {
            const auto inf = lte::info();
            char json[256];
            snprintf(json, sizeof(json),
                     "{\"device\":\"f1\",\"event\":\"%s\",\"uptime_s\":%lu,\"rssi\":%d,\"act\":%d,"
                     "\"heap_int\":%u,\"heap_int_min\":%u,\"heap_psram\":%u}",
                     first ? "boot" : "heartbeat", (unsigned long)(now / configTICK_RATE_HZ), inf.rssi,
                     inf.act, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            lte::publish(json);
            last_pub = now;
            first = false;
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
