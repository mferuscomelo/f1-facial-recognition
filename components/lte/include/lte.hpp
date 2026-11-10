#pragma once
#include <string>
#include "esp_err.h"

namespace lte {

struct Info {
    std::string imei, iccid, op_name, apn, ip;
    int act = -1;        // +COPS AcT (3GPP 27.007: 7 = E-UTRAN; GM02S reports 7 for both Cat-M1 and NB-IoT)
    int sqn_mode = -1;   // AT+SQNMODEACTIVE? raw: 1 = Cat-M1, 2 = NB-IoT (sg-sdk espmodem.c:2587-2611)
    const char* rat = "?";  // "Cat-M1" / "NB-IoT" / "?" derived from sqn_mode (static string)
    int rssi = 99;       // +CSQ raw (0..31, 99 = unknown)
    int rssi_dbm = -999; // -113 + 2*rssi, -999 if unknown (sg-sdk espmodem.c:1670)
    int cereg = -1;      // last +CEREG <stat> (1 = home, 5 = roaming)
    bool cmux = false;   // true if built for CMUX (AT available while PPP is up)
    unsigned reconnects = 0;  // number of times the data link was lost and re-established
    unsigned publish_ok = 0, publish_fail = 0;  // publish() results (after retries)
};

// Starts the LTE supervisor task (power-on, attach, PPP/CMUX, health checks, reconnect).
esp_err_t start();
bool is_connected();
Info info();  // snapshot (thread-safe)

// Blocking HTTP POST of a JSON body to CONFIG_APP_PUBLISH_URL, up to 3 attempts (10 s timeout each,
// 2 s / 4 s backoff; 4xx other than 408/429 is not retried).
// Returns the HTTP status (2xx = delivered), or <0: -1 not connected, -2 no URL configured,
// -3 client init failed, -4 transport error (esp_http_client_perform failed).
// Thread-safe: does not touch the modem DCE, only the PPP netif via lwIP sockets.
int publish(const std::string& json);

// Non-blocking: queue the event for the "lte_pub" task (queue depth 6). must_deliver=true retries
// (waiting for the link, backoff 10 s .. 5 min) until a 2xx; otherwise the event is dropped after
// publish()'s attempts fail. Returns false if the queue is full.
bool publish_async(const std::string& json, bool must_deliver = false);

}  // namespace lte
