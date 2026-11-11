#pragma once
#include "esp_err.h"

// The GM02S modem's power and reset lines are not ESP32 GPIOs: they sit on a PCAL6408A
// I2C IO expander (see docs/pinmap.md).
namespace modem_power {

esp_err_t init();     // set up I2C and the two expander pins
esp_err_t restart();  // power the modem on, or pulse reset if it is already on

}  // namespace modem_power
