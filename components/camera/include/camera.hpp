#pragma once
#include <cstddef>
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

// Copies the newest camera frame, still as JPEG straight from the webcam, into `dst`.
// Returns its size in bytes, or 0 if there is no frame yet. Used by the live preview.
constexpr size_t kMaxJpegBytes = 100 * 1024;
size_t latest_jpeg(uint8_t* dst, size_t capacity);

}  // namespace camera
