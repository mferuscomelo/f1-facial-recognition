#pragma once
#include <cstdint>

namespace board {

// Pins: see docs/pinmap.md for sources.
inline constexpr int kRgbLedGpio = 21;  // sg-sdk comps/rgbled-if/rgbled.c:59

void print_chip_info();
bool led_init();
void led_set(uint8_t r, uint8_t g, uint8_t b);
void start_heap_telemetry(uint32_t period_s);
void log_heap_once();

}  // namespace board
