// Stage 3 camera: USB UVC (MJPEG) -> esp_jpeg -> grayscale model-size frame. See camera.hpp.
//
// API sources (no invented names):
//  - USB Host lib: ESP-IDF 5.5.3 components/usb/include/usb/usb_host.h
//      usb_host_install(), usb_host_lib_handle_events(), usb_host_device_free_all(),
//      USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS / _ALL_FREE, usb_host_config_t
//  - UVC driver: espressif/usb_host_uvc 2.6.0 include/usb/uvc_host.h
//      uvc_host_install(), uvc_host_get_frame_list(), uvc_host_stream_open/start/close(),
//      uvc_host_desc_print(), UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED, UVC_HOST_DEVICE_DISCONNECTED ...
//    Structure follows the component's examples/basic_uvc_stream/main/basic_uvc_stream.c.
//  - JPEG: espressif/esp_jpeg 1.3.2 include/jpeg_decoder.h
//      esp_jpeg_decode(), esp_jpeg_get_image_info(), JPEG_IMAGE_SCALE_*, JPEG_IMAGE_FORMAT_RGB888
#include "camera.hpp"

#include "sdkconfig.h"

#if !CONFIG_APP_ENABLE_CAMERA

namespace camera {
esp_err_t start(FrameCallback, void*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t get_latest(uint8_t*, size_t, FrameInfo*, TickType_t) { return ESP_ERR_NOT_SUPPORTED; }
uint16_t model_width() { return 0; }
uint16_t model_height() { return 0; }
bool is_connected() { return false; }
Stats stats() { return Stats{}; }
}  // namespace camera

#else  // CONFIG_APP_ENABLE_CAMERA

#include <atomic>
#include <cinttypes>
#include <initializer_list>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

namespace camera {
namespace {

const char* TAG = "camera";

constexpr uint16_t kModelW = CONFIG_APP_CAMERA_MODEL_WIDTH;
constexpr uint16_t kModelH = CONFIG_APP_CAMERA_MODEL_HEIGHT;
constexpr size_t kModelBytes = size_t(kModelW) * kModelH;

// Full-Speed practical ceiling (HANDOFF.md §5 Stage 3).
constexpr unsigned kMaxW = 640;
constexpr unsigned kMaxH = 480;

constexpr int kCore = CONFIG_APP_CAMERA_TASK_CORE;
// Priorities as in basic_uvc_stream.c: USB lib task 15, UVC driver task 16. Decode runs far below.
constexpr UBaseType_t kUsbLibPrio = 15;
constexpr UBaseType_t kUvcDrvPrio = 16;
constexpr UBaseType_t kMgrPrio = 6;
constexpr UBaseType_t kDecodePrio = 4;

// MJPEG frame-buffer size estimate, copied from basic_uvc_stream.c (UVC_ESTIMATE_JPG_SIZE_B):
// max(w*h*bpp/8 / 6, 40000). The negotiated dwMaxVideoFrameSize is often far larger (see the
// component's docs/FAQ.md, "Frame buffer size"), so we size the buffers ourselves.
constexpr size_t estimate_jpeg_size(unsigned w, unsigned h) {
    const size_t est = (size_t(w) * h * 16 / 8) / 6;
    return est > 40000 ? est : 40000;
}
// Our single JPEG slot is sized for the largest format we ever select (640x480).
constexpr size_t kJpegSlotSize = estimate_jpeg_size(kMaxW, kMaxH);

// TJpgDec work-pool size. jpeg_decoder.h: "Default size is 3.1kB or 65kB if JD_FASTDECODE == 2".
// We pass our own buffer (cfg.advanced.working_buffer) so nothing is malloc'ed per frame.
#if defined(CONFIG_JD_FASTDECODE) && (CONFIG_JD_FASTDECODE == 2)
constexpr size_t kJpegWorkSize = 65472;
#else
constexpr size_t kJpegWorkSize = 4096;  // 3100 recommended + margin for default Huffman tables
#endif

inline float interval_to_fps(uint32_t interval_100ns) {
    // UVC frame intervals are in 100 ns units (UVC_DESC_DWFRAMEINTERVAL_TO_FPS in the example).
    return interval_100ns ? 10000000.0f / float(interval_100ns) : 0.0f;
}

const char* format_name(enum uvc_host_stream_format f) {
    switch (f) {
        case UVC_VS_FORMAT_MJPEG: return "MJPEG";
        case UVC_VS_FORMAT_YUY2: return "YUY2";
        case UVC_VS_FORMAT_H264: return "H264";
        case UVC_VS_FORMAT_H265: return "H265";
        case UVC_VS_FORMAT_NV12: return "NV12";
        default: return "other";
    }
}

// ---- events from driver callbacks to the manager task ----
enum class EvType : uint8_t { Connected, Disconnected };
struct Event {
    EvType type;
    uint8_t dev_addr;
    uint8_t stream_index;
    size_t frame_info_num;
    uvc_host_stream_hdl_t stream;
};

// ---- state ----
QueueHandle_t s_events = nullptr;
TaskHandle_t s_decode_task = nullptr;
SemaphoreHandle_t s_latest_mutex = nullptr;
SemaphoreHandle_t s_new_frame = nullptr;  // binary: "a frame newer than the last get_latest()"

FrameCallback s_cb = nullptr;
void* s_cb_ctx = nullptr;
bool s_started = false;

uvc_host_stream_hdl_t s_stream = nullptr;  // owned by the manager task

// JPEG slot: written by the UVC frame callback only while s_slot_busy == false.
uint8_t* s_jpeg = nullptr;  // PSRAM, kJpegSlotSize
size_t s_jpeg_len = 0;
int64_t s_jpeg_ts = 0;
std::atomic<bool> s_slot_busy{false};

// Decode buffers (decode task only)
uint8_t* s_jpeg_work = nullptr;  // internal RAM, TJpgDec pool (hot, small)
uint8_t* s_rgb = nullptr;        // PSRAM, decoded RGB888 at reduced scale
size_t s_rgb_cap = 0;
uint8_t* s_gray_work = nullptr;  // PSRAM, model-size output being produced
uint8_t* s_gray_latest = nullptr;  // PSRAM, published frame (guarded by s_latest_mutex)
FrameInfo s_latest_info{};

// Stats
std::atomic<bool> s_connected{false};
std::atomic<uint32_t> s_rx{0}, s_dropped{0}, s_too_big{0}, s_decoded{0}, s_dec_err{0}, s_drv_drops{0},
    s_connects{0}, s_disconnects{0};
uint16_t s_stream_w = 0, s_stream_h = 0;
float s_stream_fps = 0;

// ------------------------------------------------------------------ UVC callbacks
// Runs in the UVC driver task. Never blocks: copy into the slot if the decoder is idle, else drop.
// Always returns true (frame handed straight back to the driver), so a disconnect can close the
// stream at any time without frames still being held by us (uvc_host_stream_close() returns
// ESP_ERR_INVALID_STATE if frames were not returned).
bool frame_cb(const uvc_host_frame_t* frame, void* /*user_ctx*/) {
    s_rx.fetch_add(1, std::memory_order_relaxed);
    if (s_slot_busy.load(std::memory_order_acquire)) {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (frame->data_len == 0 || frame->data_len > kJpegSlotSize) {
        s_too_big.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    memcpy(s_jpeg, frame->data, frame->data_len);
    s_jpeg_len = frame->data_len;
    s_jpeg_ts = esp_timer_get_time();
    s_slot_busy.store(true, std::memory_order_release);
    xTaskNotifyGive(s_decode_task);
    return true;
}

void stream_event_cb(const uvc_host_stream_event_data_t* event, void* /*user_ctx*/) {
    switch (event->type) {
        case UVC_HOST_TRANSFER_ERROR:
            ESP_LOGE(TAG, "USB transfer error: %s", esp_err_to_name(event->transfer_error.error));
            break;
        case UVC_HOST_DEVICE_DISCONNECTED: {
            // Driver already paused the stream (uvc_host.c usb_event_cb, DEV_GONE). Close it from
            // the manager task rather than inside the driver callback.
            ESP_LOGW(TAG, "camera disconnected");
            s_connected = false;
            Event ev{};
            ev.type = EvType::Disconnected;
            ev.stream = event->device_disconnected.stream_hdl;
            xQueueSend(s_events, &ev, 0);
            break;
        }
        case UVC_HOST_FRAME_BUFFER_OVERFLOW:
            // Frame larger than our frame buffer (estimate too small for this scene/camera).
            s_drv_drops.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(TAG, "UVC frame buffer overflow (frame larger than buffer)");
            break;
        case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
            s_drv_drops.fetch_add(1, std::memory_order_relaxed);
            break;
        default:  // suspend/resume (UVC_HOST_SUSPEND_RESUME_API_SUPPORTED) - nothing to do
            break;
    }
}

void driver_event_cb(const uvc_host_driver_event_data_t* event, void* /*user_ctx*/) {
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        Event ev{};
        ev.type = EvType::Connected;
        ev.dev_addr = event->device_connected.dev_addr;
        ev.stream_index = event->device_connected.uvc_stream_index;
        ev.frame_info_num = event->device_connected.frame_info_num;
        xQueueSend(s_events, &ev, 0);
    }
}

// ------------------------------------------------------------------ format selection
// Exact requested MJPEG size if offered; else the MJPEG format <= 640x480 closest in pixel count,
// preferring formats at least as large as the model input.
bool pick_format(const uvc_host_frame_info_t* list, size_t n, const uvc_host_frame_info_t** out) {
    const long req_area = long(CONFIG_APP_CAMERA_REQ_WIDTH) * CONFIG_APP_CAMERA_REQ_HEIGHT;
    const uvc_host_frame_info_t* best = nullptr;
    long best_score = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& f = list[i];
        if (f.format != UVC_VS_FORMAT_MJPEG || f.h_res > kMaxW || f.v_res > kMaxH) continue;
        if (f.h_res == CONFIG_APP_CAMERA_REQ_WIDTH && f.v_res == CONFIG_APP_CAMERA_REQ_HEIGHT) {
            *out = &f;
            return true;
        }
        long score = labs(long(f.h_res) * f.v_res - req_area);
        if (f.h_res < kModelW || f.v_res < kModelH) score += 1L << 30;  // too small: last resort
        if (!best || score < best_score) {
            best = &f;
            best_score = score;
        }
    }
    *out = best;
    return best != nullptr;
}

// Snap the requested FPS to an interval the camera advertises for this frame (0 = default).
float pick_fps(const uvc_host_frame_info_t& f) {
    if (CONFIG_APP_CAMERA_REQ_FPS == 0) return 0.0f;
    const uint32_t want = 10000000u / CONFIG_APP_CAMERA_REQ_FPS;
    uint32_t chosen = f.default_interval;
    if (f.interval_type == 0) {  // continuous range
        chosen = want;
        if (f.interval_min && chosen < f.interval_min) chosen = f.interval_min;
        if (f.interval_max && chosen > f.interval_max) chosen = f.interval_max;
    } else {  // discrete list (array holds at most CONFIG_UVC_INTERVAL_ARRAY_SIZE entries)
        const unsigned cnt = f.interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE ? f.interval_type
                                                                              : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        uint32_t best_diff = UINT32_MAX;
        for (unsigned i = 0; i < cnt; ++i) {
            if (!f.interval[i]) continue;
            const uint32_t d = f.interval[i] > want ? f.interval[i] - want : want - f.interval[i];
            if (d < best_diff) {
                best_diff = d;
                chosen = f.interval[i];
            }
        }
    }
    return interval_to_fps(chosen);
}

void log_frame_list(const uvc_host_frame_info_t* list, size_t n) {
    ESP_LOGI(TAG, "camera offers %u frame formats:", unsigned(n));
    for (size_t i = 0; i < n; ++i) {
        const auto& f = list[i];
        if (f.interval_type == 0) {
            ESP_LOGI(TAG, "  [%u] %-5s %4ux%-4u default %.1f fps, range %.1f..%.1f fps", unsigned(i),
                     format_name(f.format), f.h_res, f.v_res, interval_to_fps(f.default_interval),
                     interval_to_fps(f.interval_max), interval_to_fps(f.interval_min));
        } else {
            ESP_LOGI(TAG, "  [%u] %-5s %4ux%-4u default %.1f fps, %u discrete rates", unsigned(i),
                     format_name(f.format), f.h_res, f.v_res, interval_to_fps(f.default_interval),
                     unsigned(f.interval_type));
        }
    }
}

// ------------------------------------------------------------------ manager task (hot-plug)
void open_stream(const Event& ev) {
    if (s_stream) {
        ESP_LOGW(TAG, "UVC device addr %u ignored: a camera is already open", ev.dev_addr);
        return;
    }
    ESP_LOGI(TAG, "UVC device connected: addr %u, stream index %u", ev.dev_addr, ev.stream_index);
    if (ev.frame_info_num == 0) {
        ESP_LOGE(TAG, "camera reports no frame formats");
        return;
    }
    // Frame list is small metadata; PSRAM keeps internal RAM free.
    size_t n = ev.frame_info_num;
    auto* list = static_cast<uvc_host_frame_info_t*>(
        heap_caps_calloc(n, sizeof(uvc_host_frame_info_t), MALLOC_CAP_SPIRAM));
    if (!list) {
        ESP_LOGE(TAG, "no memory for frame list");
        return;
    }
    esp_err_t err = uvc_host_get_frame_list(ev.dev_addr, ev.stream_index,
                                            reinterpret_cast<uvc_host_frame_info_t(*)[]>(list), &n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_get_frame_list: %s", esp_err_to_name(err));
        heap_caps_free(list);
        return;
    }
    log_frame_list(list, n);

    const uvc_host_frame_info_t* f = nullptr;
    if (!pick_format(list, n, &f)) {
        ESP_LOGE(TAG, "no MJPEG format <= %ux%u: camera not usable (need UVC + MJPEG)", kMaxW, kMaxH);
        heap_caps_free(list);
        return;
    }
    const unsigned w = f->h_res, h = f->v_res;
    const float fps = pick_fps(*f);
    heap_caps_free(list);

    uvc_host_stream_config_t cfg = {};
    cfg.event_cb = stream_event_cb;
    cfg.frame_cb = frame_cb;
    cfg.user_ctx = nullptr;
    cfg.usb.dev_addr = ev.dev_addr;
    cfg.usb.vid = UVC_HOST_ANY_VID;
    cfg.usb.pid = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = ev.stream_index;
    cfg.vs_format.h_res = w;
    cfg.vs_format.v_res = h;
    cfg.vs_format.fps = fps;
    cfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
    cfg.advanced.number_of_frame_buffers = CONFIG_APP_CAMERA_FRAME_BUFFERS;
    cfg.advanced.frame_size = estimate_jpeg_size(w, h);
    cfg.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;  // frame buffers in PSRAM (driver memcpy's into them)
    cfg.advanced.number_of_urbs = CONFIG_APP_CAMERA_URB_COUNT;  // URB buffers: internal DMA RAM (IDF usb_private.c)
    cfg.advanced.urb_size = CONFIG_APP_CAMERA_URB_SIZE;
    cfg.advanced.user_frame_buffers = nullptr;

    ESP_LOGI(TAG, "opening MJPEG %ux%u @ %.1f fps (frame buf %u B x %d in PSRAM)", w, h, fps,
             unsigned(cfg.advanced.frame_size), cfg.advanced.number_of_frame_buffers);
    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uvc_host_stream_hdl_t stream = nullptr;
    err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
    if (err != ESP_OK && fps != 0.0f) {
        ESP_LOGW(TAG, "open @ %.1f fps failed (%s); retrying with camera default fps", fps,
                 esp_err_to_name(err));
        cfg.vs_format.fps = 0;
        err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_open: %s (replug the camera to retry)", esp_err_to_name(err));
        return;
    }
#if CONFIG_APP_CAMERA_PRINT_DESCRIPTORS
    uvc_host_desc_print(stream);  // device + full configuration descriptor to stdout
#endif
    uvc_host_stream_format_t got = {};
    if (uvc_host_stream_format_get(stream, &got) == ESP_OK) {
        s_stream_w = got.h_res;
        s_stream_h = got.v_res;
        s_stream_fps = got.fps;
    }
    err = uvc_host_stream_start(stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_start: %s", esp_err_to_name(err));
        uvc_host_stream_close(stream);
        return;
    }
    s_stream = stream;
    s_connected = true;
    s_connects.fetch_add(1);
    ESP_LOGI(TAG, "streaming %ux%u @ %.1f fps; stream internal RAM cost ~%d B", s_stream_w, s_stream_h,
             s_stream_fps, int(int_before) - int(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void close_stream(uvc_host_stream_hdl_t stream) {
    if (!stream || stream != s_stream) return;
    const esp_err_t err = uvc_host_stream_close(stream);
    if (err != ESP_OK) ESP_LOGW(TAG, "uvc_host_stream_close: %s", esp_err_to_name(err));
    s_stream = nullptr;
    s_connected = false;
    s_stream_w = s_stream_h = 0;
    s_stream_fps = 0;
    s_disconnects.fetch_add(1);
    ESP_LOGI(TAG, "stream closed; waiting for camera");
}

void manager_task(void*) {
    Event ev;
    for (;;) {
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type == EvType::Connected) open_stream(ev);
        else close_stream(ev.stream);
    }
}

// ------------------------------------------------------------------ USB host library task
// Same loop as usb_lib_task() in basic_uvc_stream.c. usb_host_install() is called from inside this
// task (as in ESP-IDF's examples/peripherals/usb/host/usb_host_lib) so the USB ISR is allocated on
// this task's core.
struct UsbLibArgs {
    SemaphoreHandle_t done;
    esp_err_t result;
};

void usb_lib_task(void* arg) {
    auto* a = static_cast<UsbLibArgs*>(arg);
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;  // internal PHY on GPIO19/20 -> J202
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;
    a->result = usb_host_install(&host_config);
    xSemaphoreGive(a->done);
    if (a->result != ESP_OK) {
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
        // USB_HOST_LIB_EVENT_FLAGS_ALL_FREE: keep handling events so devices can reconnect.
    }
}

// ------------------------------------------------------------------ decode + resize
esp_jpeg_image_scale_t scale_enum(unsigned div) {
    switch (div) {
        case 8: return JPEG_IMAGE_SCALE_1_8;
        case 4: return JPEG_IMAGE_SCALE_1_4;
        case 2: return JPEG_IMAGE_SCALE_1_2;
        default: return JPEG_IMAGE_SCALE_0;
    }
}

// Largest TJpgDec descale factor whose output still covers the model input after the aspect crop.
unsigned pick_scale_div(unsigned w, unsigned h) {
    // crop region (source pixels) with the model's aspect ratio
    unsigned cw = w, ch = h;
    if (uint64_t(w) * kModelH > uint64_t(h) * kModelW) cw = unsigned(uint64_t(h) * kModelW / kModelH);
    else ch = unsigned(uint64_t(w) * kModelH / kModelW);
    for (unsigned d : {8u, 4u, 2u}) {
        if (cw / d >= kModelW && ch / d >= kModelH) return d;
    }
    return 1;
}

// Centre-crop rgb (sw x sh RGB888) to the model aspect, box-filter it down to kModelW x kModelH and
// convert to 8-bit luma (BT.601 weights 77/150/29 >> 8). Every source pixel is read at most once.
void crop_resize_gray(const uint8_t* rgb, unsigned sw, unsigned sh, uint8_t* out) {
    unsigned cw = sw, ch = sh;
    if (uint64_t(sw) * kModelH > uint64_t(sh) * kModelW) cw = unsigned(uint64_t(sh) * kModelW / kModelH);
    else ch = unsigned(uint64_t(sw) * kModelH / kModelW);
    const unsigned x_off = (sw - cw) / 2, y_off = (sh - ch) / 2;
    for (unsigned oy = 0; oy < kModelH; ++oy) {
        unsigned y0 = y_off + oy * ch / kModelH;
        unsigned y1 = y_off + (oy + 1) * ch / kModelH;
        if (y1 <= y0) y1 = y0 + 1;
        for (unsigned ox = 0; ox < kModelW; ++ox) {
            unsigned x0 = x_off + ox * cw / kModelW;
            unsigned x1 = x_off + (ox + 1) * cw / kModelW;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t acc = 0;
            for (unsigned y = y0; y < y1; ++y) {
                const uint8_t* p = rgb + (size_t(y) * sw + x0) * 3;
                for (unsigned x = x0; x < x1; ++x, p += 3) acc += 77u * p[0] + 150u * p[1] + 29u * p[2];
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            out[size_t(oy) * kModelW + ox] = uint8_t((acc / n) >> 8);
        }
    }
}

bool ensure_rgb(size_t need) {
    if (need <= s_rgb_cap) return true;
    heap_caps_free(s_rgb);
    s_rgb = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM));
    s_rgb_cap = s_rgb ? need : 0;
    if (!s_rgb) ESP_LOGE(TAG, "no PSRAM for %u B RGB buffer", unsigned(need));
    return s_rgb != nullptr;
}

void decode_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_slot_busy.load(std::memory_order_acquire)) continue;

        FrameInfo fi{};
        fi.timestamp_us = s_jpeg_ts;
        fi.jpeg_bytes = s_jpeg_len;
        fi.width = kModelW;
        fi.height = kModelH;

        esp_jpeg_image_cfg_t jcfg = {};
        jcfg.indata = s_jpeg;
        jcfg.indata_size = s_jpeg_len;
        jcfg.out_format = JPEG_IMAGE_FORMAT_RGB888;  // byte order R,G,B (tjpgd output, no swap)
        jcfg.out_scale = JPEG_IMAGE_SCALE_0;
        jcfg.advanced.working_buffer = s_jpeg_work;
        jcfg.advanced.working_buffer_size = kJpegWorkSize;

        // Source size: parse SOF0 without decoding; fall back to the negotiated stream size.
        esp_jpeg_image_output_t info = {};
        unsigned sw = s_stream_w, sh = s_stream_h;
        if (esp_jpeg_get_image_info(&jcfg, &info) == ESP_OK && info.width && info.height) {
            sw = info.width;
            sh = info.height;
        }
        fi.src_width = sw;
        fi.src_height = sh;
        const unsigned div = pick_scale_div(sw, sh);
        const size_t need = size_t(sw / div) * (sh / div) * 3;

        bool ok = sw && sh && ensure_rgb(need);
        const int64_t t0 = esp_timer_get_time();
        if (ok) {
            jcfg.out_scale = scale_enum(div);
            jcfg.outbuf = s_rgb;
            jcfg.outbuf_size = s_rgb_cap;
            esp_jpeg_image_output_t out = {};
            ok = esp_jpeg_decode(&jcfg, &out) == ESP_OK;
            if (ok) {
                const int64_t t1 = esp_timer_get_time();
                crop_resize_gray(s_rgb, out.width, out.height, s_gray_work);
                fi.decode_us = uint32_t(t1 - t0);
                fi.resize_us = uint32_t(esp_timer_get_time() - t1);
            }
        }
        // Slot is free again only now: frames that arrived meanwhile were dropped, not queued.
        s_slot_busy.store(false, std::memory_order_release);

        if (!ok) {
            s_dec_err.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        fi.seq = s_decoded.fetch_add(1, std::memory_order_relaxed) + 1;

        xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
        memcpy(s_gray_latest, s_gray_work, kModelBytes);
        s_latest_info = fi;
        xSemaphoreGive(s_latest_mutex);
        xSemaphoreGive(s_new_frame);

        if (s_cb) s_cb(s_gray_work, fi, s_cb_ctx);
    }
}

}  // namespace

// ------------------------------------------------------------------ public API
uint16_t model_width() { return kModelW; }
uint16_t model_height() { return kModelH; }
bool is_connected() { return s_connected; }

Stats stats() {
    Stats s{};
    s.connected = s_connected;
    s.stream_width = s_stream_w;
    s.stream_height = s_stream_h;
    s.stream_fps = s_stream_fps;
    s.frames_received = s_rx;
    s.frames_dropped = s_dropped;
    s.frames_too_big = s_too_big;
    s.frames_decoded = s_decoded;
    s.decode_errors = s_dec_err;
    s.driver_drops = s_drv_drops;
    s.connects = s_connects;
    s.disconnects = s_disconnects;
    return s;
}

esp_err_t get_latest(uint8_t* dst, size_t dst_size, FrameInfo* info, TickType_t timeout) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (!dst || dst_size < kModelBytes) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_new_frame, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
    memcpy(dst, s_gray_latest, kModelBytes);
    if (info) *info = s_latest_info;
    xSemaphoreGive(s_latest_mutex);
    return ESP_OK;
}

esp_err_t start(FrameCallback cb, void* ctx) {
    if (s_started) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    s_cb_ctx = ctx;

    // Buffers: everything big in PSRAM; only the 4 KB TJpgDec pool is internal (hot path).
    s_jpeg = static_cast<uint8_t*>(heap_caps_malloc(kJpegSlotSize, MALLOC_CAP_SPIRAM));
    s_gray_work = static_cast<uint8_t*>(heap_caps_malloc(kModelBytes, MALLOC_CAP_SPIRAM));
    s_gray_latest = static_cast<uint8_t*>(heap_caps_calloc(1, kModelBytes, MALLOC_CAP_SPIRAM));
    s_jpeg_work = static_cast<uint8_t*>(heap_caps_malloc(kJpegWorkSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_events = xQueueCreate(4, sizeof(Event));
    s_latest_mutex = xSemaphoreCreateMutex();
    s_new_frame = xSemaphoreCreateBinary();
    if (!s_jpeg || !s_gray_work || !s_gray_latest || !s_jpeg_work || !s_events || !s_latest_mutex ||
        !s_new_frame) {
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    // Pre-size the RGB buffer for the requested format so steady state never allocates.
    {
        const unsigned w = CONFIG_APP_CAMERA_REQ_WIDTH, h = CONFIG_APP_CAMERA_REQ_HEIGHT;
        const unsigned div = pick_scale_div(w, h);
        ensure_rgb(size_t(w / div) * (h / div) * 3);
    }

    // Decode task first (frame_cb notifies it). Stack in internal RAM: JDEC lives on the stack.
    if (xTaskCreatePinnedToCore(decode_task, "cam_decode", 4096, nullptr, kDecodePrio, &s_decode_task,
                                kCore) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(manager_task, "cam_mgr", 4096, nullptr, kMgrPrio, nullptr, kCore) != pdPASS)
        return ESP_ERR_NO_MEM;

    // USB Host library (installed from inside its own task, see usb_lib_task()).
    UsbLibArgs args{xSemaphoreCreateBinary(), ESP_FAIL};
    if (!args.done) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, &args, kUsbLibPrio, nullptr, kCore) != pdPASS) {
        vSemaphoreDelete(args.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(args.done, portMAX_DELAY);
    vSemaphoreDelete(args.done);
    if (args.result != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(args.result));
        return args.result;
    }

    // UVC driver with its own background task (create_background_task = true, as in the example).
    uvc_host_driver_config_t drv = {};
    drv.driver_task_stack_size = 4 * 1024;
    drv.driver_task_priority = kUvcDrvPrio;
    drv.xCoreID = kCore;
    drv.create_background_task = true;
    drv.event_cb = driver_event_cb;
    drv.user_ctx = nullptr;
    const esp_err_t err = uvc_host_install(&drv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB host + UVC driver ready (J202). Request %dx%d MJPEG @ %d fps -> %ux%u gray",
             CONFIG_APP_CAMERA_REQ_WIDTH, CONFIG_APP_CAMERA_REQ_HEIGHT, CONFIG_APP_CAMERA_REQ_FPS, kModelW,
             kModelH);
    return ESP_OK;
}

}  // namespace camera

#endif  // CONFIG_APP_ENABLE_CAMERA
