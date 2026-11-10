// F1 facecam - Stage 0: board bring-up (chip info, PSRAM, heap telemetry, LED blink)
#include "board.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "app";

extern "C" void app_main() {
    ESP_LOGI(TAG, "F1 facecam booting");
    board::print_chip_info();
    board::log_heap_once();
    board::start_heap_telemetry(CONFIG_APP_HEAP_LOG_PERIOD_S);

    if (!board::led_init()) ESP_LOGW(TAG, "LED unavailable");

    // Stage 0 blink: red -> green -> blue, dim
    const uint8_t colors[][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}};
    for (unsigned i = 0;; ++i) {
        const auto& c = colors[i % 3];
        board::led_set(c[0], c[1], c[2]);
        vTaskDelay(pdMS_TO_TICKS(500));
        board::led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
