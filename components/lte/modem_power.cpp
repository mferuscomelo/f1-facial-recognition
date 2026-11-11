// PCAL6408A IO expander driving the modem's power (P1) and reset_n (P3) lines.
// Values from the SG Wireless SDK, see docs/pinmap.md.
#include "modem_power.hpp"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace modem_power {
namespace {
const char* TAG = "modem_power";

constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegConfig = 0x03;        // bit = 1: input, 0: output
constexpr uint8_t kPinPower = 1u << 1;      // P1: lte_power
constexpr uint8_t kPinResetN = 1u << 3;     // P3: lte_reset_n (active low)

i2c_master_dev_handle_t s_dev = nullptr;

uint8_t read_reg(uint8_t reg) {
    uint8_t val = 0;
    i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 100);
    return val;
}

esp_err_t write_reg(uint8_t reg, uint8_t val) {
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t set_pin(uint8_t pin, bool high) {
    const uint8_t out = read_reg(kRegOutput);
    return write_reg(kRegOutput, high ? (out | pin) : (out & ~pin));
}
}  // namespace

esp_err_t init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = GPIO_NUM_8;
    bus_cfg.scl_io_num = GPIO_NUM_7;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;  // the board has external pull-ups
    i2c_master_bus_handle_t bus;
    if (esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus); e != ESP_OK) return e;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = 0x21;
    dev_cfg.scl_speed_hz = 400000;
    if (esp_err_t e = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev); e != ESP_OK) return e;

    // Keep reset released (high), then make both pins outputs.
    if (esp_err_t e = set_pin(kPinResetN, true); e != ESP_OK) {
        ESP_LOGE(TAG, "IO expander not responding: %s", esp_err_to_name(e));
        return e;
    }
    return write_reg(kRegConfig, read_reg(kRegConfig) & ~(kPinPower | kPinResetN));
}

esp_err_t restart() {
    if (read_reg(kRegOutput) & kPinPower) {
        ESP_LOGI(TAG, "modem reset");
        set_pin(kPinResetN, false);
        vTaskDelay(pdMS_TO_TICKS(100));
        return set_pin(kPinResetN, true);
    }
    ESP_LOGI(TAG, "modem power on");
    set_pin(kPinResetN, true);
    return set_pin(kPinPower, true);
}

}  // namespace modem_power
