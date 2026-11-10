#pragma once
#include <string>
#include "esp_err.h"

namespace lte {

struct Info {
    std::string imei, iccid, op_name, apn, ip;
    int act = -1;      // +COPS AcT (3GPP 27.007: 7 = E-UTRAN, 8 = E-UTRAN Cat-M1 / eMTC, 9 = NB-IoT)
    int rssi = 99;     // +CSQ raw (0..31, 99 = unknown)
};

// Starts the LTE supervisor task (power-on, attach, PPP, reconnect).
esp_err_t start();
bool is_connected();
Info info();  // snapshot

// Plain HTTP POST of a JSON body to CONFIG_APP_PUBLISH_URL. Returns HTTP status or <0 on error.
int publish(const std::string& json);

}  // namespace lte
