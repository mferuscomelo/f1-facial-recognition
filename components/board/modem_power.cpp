#include "modem_power.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board::modem_power {
namespace {
constexpr const char* TAG = "modem_pwr";

// sg-sdk src/platforms/F1/comps/ioexp-if/ioexp.c:134-161, comps/lte/espmodem.h:51-57
constexpr gpio_num_t kSclGpio = GPIO_NUM_7;
constexpr gpio_num_t kSdaGpio = GPIO_NUM_8;
constexpr uint16_t kAddr = 0x21;          // 0x20 | ADDR pin tied to 3V3
constexpr uint32_t kSclHz = 400000;

// sg-sdk src/drivers/pcal6408a/pcal6408a.c:84-97
constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegConfig = 0x03;      // bit = 1 -> input, 0 -> output

constexpr uint8_t kPinLtePower = 1u << 1;  // P1
constexpr uint8_t kPinLteResetN = 1u << 3; // P3

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;

esp_err_t read_reg(uint8_t reg, uint8_t& val) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 100);
}
esp_err_t write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}
esp_err_t update_reg(uint8_t reg, uint8_t mask, bool set) {
    uint8_t v = 0;
    if (esp_err_t e = read_reg(reg, v); e != ESP_OK) return e;
    v = set ? (v | mask) : (v & ~mask);
    return write_reg(reg, v);
}
}  // namespace

esp_err_t init() {
    if (s_dev) return ESP_OK;
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = kSdaGpio;
    bus_cfg.scl_io_num = kSclGpio;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = false;  // SG disables internal pull-ups (ioexp.c:311-312)
    if (esp_err_t e = i2c_new_master_bus(&bus_cfg, &s_bus); e != ESP_OK) return e;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAddr;
    dev_cfg.scl_speed_hz = kSclHz;
    if (esp_err_t e = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev); e != ESP_OK) return e;

    uint8_t out = 0, cfg = 0;
    if (esp_err_t e = read_reg(kRegOutput, out); e != ESP_OK) {
        ESP_LOGE(TAG, "IO expander @0x%02x not responding: %s", kAddr, esp_err_to_name(e));
        return e;
    }
    read_reg(kRegConfig, cfg);
    ESP_LOGI(TAG, "IO expander ok: output=0x%02x config=0x%02x", out, cfg);

    // Preload output latch before switching pins to output so nothing glitches:
    // keep current power state, reset_n deasserted (high).
    const bool was_on = !(cfg & kPinLtePower) && (out & kPinLtePower);
    out = (out | kPinLteResetN);
    out = was_on ? (out | kPinLtePower) : (out & ~kPinLtePower);
    if (esp_err_t e = write_reg(kRegOutput, out); e != ESP_OK) return e;
    return write_reg(kRegConfig, cfg & ~(kPinLtePower | kPinLteResetN));
}

bool is_on() {
    uint8_t out = 0, cfg = 0xff;
    if (!s_dev || read_reg(kRegOutput, out) != ESP_OK || read_reg(kRegConfig, cfg) != ESP_OK) return false;
    return !(cfg & kPinLtePower) && (out & kPinLtePower);
}

esp_err_t power_on() {
    ESP_LOGI(TAG, "LTE power on");
    if (esp_err_t e = update_reg(kRegOutput, kPinLteResetN, true); e != ESP_OK) return e;
    return update_reg(kRegOutput, kPinLtePower, true);
}

esp_err_t power_off() {
    ESP_LOGI(TAG, "LTE power off");
    return update_reg(kRegOutput, kPinLtePower, false);
}

esp_err_t reset_pulse() {
    ESP_LOGI(TAG, "LTE reset pulse");
    if (esp_err_t e = update_reg(kRegOutput, kPinLteResetN, false); e != ESP_OK) return e;
    vTaskDelay(pdMS_TO_TICKS(100));
    return update_reg(kRegOutput, kPinLteResetN, true);
}

}  // namespace board::modem_power
