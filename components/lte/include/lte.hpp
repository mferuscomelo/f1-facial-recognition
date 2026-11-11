#pragma once
#include "esp_err.h"

// LTE-M (Cat-M1) over the F1's Sequans GM02S modem and the kit's pre-installed SIM.
namespace lte {

// Starts a background task that powers the modem, attaches to the network and keeps the
// data link up (reconnecting by itself if it drops).
esp_err_t start();

bool is_connected();

// Last signal quality from AT+CSQ: 0..31 (higher is better), 99 = unknown.
int signal_quality();

// Queues a JSON message for an HTTP POST to CONFIG_APP_PUBLISH_URL. Never blocks.
// Returns false if the queue is full.
bool publish(const char* json);

}  // namespace lte
