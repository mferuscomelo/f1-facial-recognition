// Stage 3: USB UVC webcam -> MJPEG decode -> grayscale model-size frame.
//
// Pipeline (all frame buffers in PSRAM, see README.md for the internal-RAM budget):
//   USB Host lib + espressif/usb_host_uvc (MJPEG, Full-Speed)
//     -> frame callback copies the JPEG into one PSRAM slot *only if the decoder is idle*
//        (otherwise the frame is dropped: nothing ever queues up)
//     -> decode task: espressif/esp_jpeg (TJpgDec, RGB888, with 1/2..1/8 descaling)
//     -> centre-crop to the model aspect ratio, box-filter downscale + luma conversion
//     -> "latest frame" mailbox (+ optional per-frame callback)
//
// Hot-plug: the camera can be unplugged and re-plugged at any time; the stream is closed on
// disconnect and re-opened on the next UVC connect event, no reboot needed.
//
// Privacy: frames live only in RAM and are overwritten by the next one. Nothing is stored or sent.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

namespace camera {

struct FrameInfo {
    uint32_t seq;           // monotonically increasing frame number (decoded frames)
    uint16_t width;         // output (model) width
    uint16_t height;        // output (model) height
    uint16_t src_width;     // camera frame width (before decode scaling/crop)
    uint16_t src_height;    // camera frame height
    uint32_t jpeg_bytes;    // size of the source MJPEG frame
    uint32_t decode_us;     // JPEG decode time
    uint32_t resize_us;     // crop + downscale + grayscale time
    int64_t timestamp_us;   // esp_timer_get_time() when the JPEG frame was received
};

struct Stats {
    bool connected;           // a UVC stream is open and running
    uint16_t stream_width;    // negotiated stream resolution (0 if none)
    uint16_t stream_height;
    float stream_fps;         // negotiated frame rate
    uint32_t frames_received; // complete MJPEG frames delivered by the UVC driver
    uint32_t frames_dropped;  // dropped because the decoder was still busy (by design)
    uint32_t frames_too_big;  // dropped because they did not fit the JPEG slot
    uint32_t frames_decoded;  // successfully decoded + resized
    uint32_t decode_errors;   // esp_jpeg_decode() failures (e.g. corrupt frames)
    uint32_t driver_drops;    // UVC driver frame-buffer overflow/underflow events
    uint32_t connects;        // number of successful stream opens since boot
    uint32_t disconnects;
};

// Called from the camera decode task for every decoded frame. `gray` is model_w*model_h bytes
// (row-major, 8-bit luma) and is only valid during the call. Keep it short or copy the data:
// while the callback runs, incoming frames are dropped (not queued).
using FrameCallback = void (*)(const uint8_t* gray, const FrameInfo& info, void* ctx);

// Install the USB Host library + UVC driver and start the camera tasks. Non-blocking: returns as
// soon as the drivers are installed; the camera may be plugged in at any time afterwards.
// Output size comes from CONFIG_APP_CAMERA_MODEL_WIDTH/HEIGHT. `cb` is optional.
// Returns ESP_ERR_NOT_SUPPORTED when built without CONFIG_APP_ENABLE_CAMERA.
esp_err_t start(FrameCallback cb = nullptr, void* ctx = nullptr);

// Copy the most recent decoded frame into a caller-provided buffer (>= model_w*model_h bytes).
// Blocks up to `timeout` for a frame newer than the one returned by the previous call, so each
// frame is returned at most once and the caller always gets the freshest one (intermediate
// frames are dropped). Intended for a single consumer task.
// Returns ESP_OK, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE (not started).
esp_err_t get_latest(uint8_t* dst, size_t dst_size, FrameInfo* info, TickType_t timeout);

// Model input size the pipeline produces.
uint16_t model_width();
uint16_t model_height();

bool is_connected();
Stats stats();

}  // namespace camera
