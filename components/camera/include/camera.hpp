#pragma once
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// USB webcam on the F1's bottom USB-C port (J202), running as a USB host.
// Frames only ever live in RAM: nothing is stored or sent.
namespace camera {

// Installs the USB host and webcam (UVC) drivers. The camera can be plugged in (and
// unplugged) at any time afterwards.
esp_err_t start();

// Waits up to `timeout` for the next camera frame and turns it into a `width` x `height`
// 8-bit grayscale image (centre-cropped, then scaled down).
esp_err_t capture(uint8_t* gray, int width, int height, TickType_t timeout);

}  // namespace camera
