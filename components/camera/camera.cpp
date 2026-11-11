// USB webcam -> MJPEG frame -> JPEG decode -> small grayscale image.
//
// Drivers: ESP-IDF USB host, espressif/usb_host_uvc (webcam), espressif/esp_jpeg (decoder).
// Structure follows usb_host_uvc's examples/basic_uvc_stream.
#include "camera.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

namespace camera {
namespace {
const char* TAG = "camera";

// USB on the ESP32-S3 is Full-Speed (12 Mbit/s), so we ask for a small MJPEG stream.
constexpr int kWantWidth = 320, kWantHeight = 240;
constexpr size_t kRgbBytes = 320 * 240 * 3;  // largest decoded image we accept
constexpr int kCore = 1;                     // camera + AI on core 1, LTE on core 0

// The newest JPEG frame. The USB callback only fills it while `s_jpeg_full` is false,
// so frames that arrive while we are busy are simply skipped.
uint8_t* s_jpeg;
size_t s_jpeg_len;
std::atomic<bool> s_jpeg_full{false};
SemaphoreHandle_t s_frame_ready;
uint8_t* s_rgb;
uint8_t s_jpeg_work[4096];  // decoder scratch, in fast internal RAM

// A second copy of the newest JPEG for the live preview (components/preview).
uint8_t* s_preview;
size_t s_preview_len;
SemaphoreHandle_t s_preview_lock;

// Plug / unplug events, handled in camera_task (UVC calls are not allowed in callbacks).
struct Event {
    bool connected;
    uint8_t dev_addr, stream_index;
    uvc_host_stream_hdl_t stream;
};
QueueHandle_t s_events;
uvc_host_stream_hdl_t s_stream;

bool on_frame(const uvc_host_frame_t* frame, void*) {
    if (frame->data_len == 0 || frame->data_len > kMaxJpegBytes) return true;
    // Never wait here: if the preview is reading its copy, skip this frame for it.
    if (xSemaphoreTake(s_preview_lock, 0) == pdTRUE) {
        memcpy(s_preview, frame->data, frame->data_len);
        s_preview_len = frame->data_len;
        xSemaphoreGive(s_preview_lock);
    }
    if (!s_jpeg_full) {
        memcpy(s_jpeg, frame->data, frame->data_len);
        s_jpeg_len = frame->data_len;
        s_jpeg_full = true;
        xSemaphoreGive(s_frame_ready);
    }
    return true;  // hand the buffer straight back to the driver
}

void on_stream_event(const uvc_host_stream_event_data_t* event, void*) {
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        Event ev = {false, 0, 0, event->device_disconnected.stream_hdl};
        xQueueSend(s_events, &ev, 0);
    }
}

void on_driver_event(const uvc_host_driver_event_data_t* event, void*) {
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        Event ev = {true, event->device_connected.dev_addr, event->device_connected.uvc_stream_index, nullptr};
        xQueueSend(s_events, &ev, 0);
    }
}

// Pick the MJPEG format closest to 320x240.
bool pick_format(const Event& ev, uvc_host_stream_format_t* fmt) {
    static uvc_host_frame_info_t list[32];
    size_t n = 32;
    auto* arr = reinterpret_cast<uvc_host_frame_info_t(*)[]>(list);
    if (uvc_host_get_frame_list(ev.dev_addr, ev.stream_index, arr, &n) != ESP_OK) return false;
    long best = -1;
    for (size_t i = 0; i < n; ++i) {
        if (list[i].format != UVC_VS_FORMAT_MJPEG || list[i].h_res > 640 || list[i].v_res > 480) continue;
        const long diff = labs(long(list[i].h_res) * list[i].v_res - kWantWidth * kWantHeight);
        if (best < 0 || diff < best) {
            best = diff;
            *fmt = {list[i].h_res, list[i].v_res, 0, UVC_VS_FORMAT_MJPEG};  // fps 0 = camera default
        }
    }
    return best >= 0;
}

void open_stream(const Event& ev) {
    uvc_host_stream_config_t cfg = {};
    if (s_stream) return;  // one camera at a time
    if (!pick_format(ev, &cfg.vs_format)) {
        ESP_LOGE(TAG, "camera not usable (needs MJPEG up to 640x480)");
        return;
    }
    cfg.event_cb = on_stream_event;
    cfg.frame_cb = on_frame;
    cfg.usb.dev_addr = ev.dev_addr;
    cfg.usb.vid = UVC_HOST_ANY_VID;
    cfg.usb.pid = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = ev.stream_index;
    cfg.advanced.number_of_frame_buffers = 3;
    cfg.advanced.frame_size = kMaxJpegBytes;
    cfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;
    cfg.advanced.number_of_urbs = 4;
    cfg.advanced.urb_size = 10 * 1024;

    if (uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &s_stream) != ESP_OK) {
        ESP_LOGE(TAG, "could not open the camera stream");
        s_stream = nullptr;
        return;
    }
    uvc_host_stream_start(s_stream);
    ESP_LOGI(TAG, "camera streaming MJPEG %ux%u", cfg.vs_format.h_res, cfg.vs_format.v_res);
}

void camera_task(void*) {
    Event ev;
    for (;;) {
        xQueueReceive(s_events, &ev, portMAX_DELAY);
        if (ev.connected) {
            open_stream(ev);
        } else if (ev.stream && ev.stream == s_stream) {
            uvc_host_stream_close(s_stream);
            s_stream = nullptr;
            ESP_LOGW(TAG, "camera unplugged");
        }
    }
}

void usb_host_task(void* done) {
    usb_host_config_t cfg = {};
    cfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    // The S3's USB FIFO has 200 lines (800 B). The default split only allows video packets up to
    // 596 B, too small for most webcams. Video is IN-only, so we give the periodic-OUT share
    // to RX: packets up to ~724 B then fit (see MAX_MPS_IN in components/usb_host_uvc).
    cfg.fifo_settings_custom.nptx_fifo_lines = 16;
    cfg.fifo_settings_custom.ptx_fifo_lines = 0;
    cfg.fifo_settings_custom.rx_fifo_lines = 184;
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(done));
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// Centre-crop `rgb` to the output's aspect ratio, average each block of pixels and convert to
// luma (BT.601: 0.30 R + 0.59 G + 0.11 B).
void crop_resize_gray(const uint8_t* rgb, int sw, int sh, uint8_t* out, int w, int h) {
    int cw = sw, ch = sh;
    if (sw * h > sh * w) cw = sh * w / h;
    else ch = sw * h / w;
    const int x_off = (sw - cw) / 2, y_off = (sh - ch) / 2;
    for (int oy = 0; oy < h; ++oy) {
        const int y0 = y_off + oy * ch / h, y1 = std::max(y0 + 1, y_off + (oy + 1) * ch / h);
        for (int ox = 0; ox < w; ++ox) {
            const int x0 = x_off + ox * cw / w, x1 = std::max(x0 + 1, x_off + (ox + 1) * cw / w);
            uint32_t sum = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* p = rgb + (y * sw + x0) * 3;
                for (int x = x0; x < x1; ++x, p += 3) sum += 77 * p[0] + 150 * p[1] + 29 * p[2];
            }
            out[oy * w + ox] = (sum / ((y1 - y0) * (x1 - x0))) >> 8;
        }
    }
}
}  // namespace

esp_err_t start() {
    s_jpeg = static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes, MALLOC_CAP_SPIRAM));
    s_rgb = static_cast<uint8_t*>(heap_caps_malloc(kRgbBytes, MALLOC_CAP_SPIRAM));
    s_preview = static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegBytes, MALLOC_CAP_SPIRAM));
    s_frame_ready = xSemaphoreCreateBinary();
    s_preview_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(4, sizeof(Event));
    if (!s_jpeg || !s_rgb || !s_preview) return ESP_ERR_NO_MEM;

    // The USB host library is installed from its own task so its interrupt lands on kCore.
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, done, 15, nullptr, kCore);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);

    uvc_host_driver_config_t drv = {};
    drv.driver_task_stack_size = 4096;
    drv.driver_task_priority = 16;
    drv.xCoreID = kCore;
    drv.create_background_task = true;
    drv.event_cb = on_driver_event;
    if (esp_err_t e = uvc_host_install(&drv); e != ESP_OK) return e;
    xTaskCreatePinnedToCore(camera_task, "camera", 4096, nullptr, 6, nullptr, kCore);
    ESP_LOGI(TAG, "USB host ready: plug a webcam into J202");
    return ESP_OK;
}

esp_err_t capture(uint8_t* gray, int width, int height, TickType_t timeout) {
    if (xSemaphoreTake(s_frame_ready, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;

    // Let the decoder shrink the image (1/2, 1/4 or 1/8) as far as the output size allows:
    // decoding fewer pixels is much faster.
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = s_jpeg;
    cfg.indata_size = s_jpeg_len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.advanced.working_buffer = s_jpeg_work;
    cfg.advanced.working_buffer_size = sizeof(s_jpeg_work);
    esp_jpeg_image_output_t img = {};
    esp_err_t err = esp_jpeg_get_image_info(&cfg, &img);
    int scale = 3;  // 3 = 1/8, 2 = 1/4, 1 = 1/2, 0 = full size
    while (scale > 0 && ((img.width >> scale) < width || (img.height >> scale) < height)) --scale;
    cfg.out_scale = static_cast<esp_jpeg_image_scale_t>(scale);
    cfg.outbuf = s_rgb;
    cfg.outbuf_size = kRgbBytes;
    if (err == ESP_OK) err = esp_jpeg_decode(&cfg, &img);  // fails cleanly if the image is too big
    s_jpeg_full = false;  // the USB callback may fill the buffer again
    if (err == ESP_OK) crop_resize_gray(s_rgb, img.width, img.height, gray, width, height);
    return err;
}

size_t latest_jpeg(uint8_t* dst, size_t capacity) {
    xSemaphoreTake(s_preview_lock, portMAX_DELAY);
    const size_t len = s_preview_len <= capacity ? s_preview_len : 0;
    memcpy(dst, s_preview, len);
    xSemaphoreGive(s_preview_lock);
    return len;
}

}  // namespace camera
