#pragma once
#include "esp_err.h"

// GM02S power/reset via PCAL6408A IO expander (see docs/pinmap.md).
namespace board::modem_power {

esp_err_t init();          // I2C bus + expander pin setup (idempotent)
bool is_on();              // reads expander output latch for lte_power
esp_err_t power_on();      // reset_n high, then power high (sg-sdk ioexp.c:814-829)
esp_err_t power_off();     // power low
esp_err_t reset_pulse();   // reset_n low 100 ms, then high (sg-sdk ioexp.c:849-865)

}  // namespace board::modem_power
