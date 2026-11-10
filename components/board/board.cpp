#include "board.hpp"

#include <cinttypes>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace board {
namespace {
constexpr const char* TAG = "board";
led_strip_handle_t s_strip = nullptr;

void heap_task(void* arg) {
    const uint32_t period_s = reinterpret_cast<uintptr_t>(arg);
    for (;;) {
        log_heap_once();
        vTaskDelay(pdMS_TO_TICKS(period_s * 1000));
    }
}
}  // namespace

void print_chip_info() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    ESP_LOGI(TAG, "IDF %s, %s rev v%d.%d, %d cores, features 0x%" PRIx32,
             esp_get_idf_version(), CONFIG_IDF_TARGET, info.revision / 100,
             info.revision % 100, info.cores, info.features);
    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    ESP_LOGI(TAG, "PSRAM: %u KB (initialized: %s)",
             static_cast<unsigned>(esp_psram_get_size() / 1024),
             esp_psram_is_initialized() ? "yes" : "no");
}

bool led_init() {
    // WS2812, 1 LED, GRB (sg-sdk comps/rgbled-if/defaults.config, rgbled.c:123-128)
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kRgbLedGpio;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip init failed: %s", esp_err_to_name(err));
        return false;
    }
    led_strip_clear(s_strip);
    return true;
}

void led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void log_heap_once() {
    ESP_LOGI(TAG, "heap int free=%u largest=%u min=%u | psram free=%u largest=%u",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void start_heap_telemetry(uint32_t period_s) {
    xTaskCreate(heap_task, "heap_tlm", 3072,
                reinterpret_cast<void*>(static_cast<uintptr_t>(period_s)), 1, nullptr);
}

}  // namespace board
