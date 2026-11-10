// Edge Impulse wrapper. API names taken from the bundled SDK headers:
//   run_classifier(), signal_t, ei_impulse_result_t   (classifier/ei_run_classifier.h, dsp/numpy_types.h)
//   ei_malloc/ei_calloc/ei_free are weak in porting/espressif/ei_classifier_porting.cpp -> overridden here.
#include "inference.hpp"

#include <cstring>

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

static const char* TAG = "inference";

namespace {
inference::ArenaPolicy g_policy = inference::ArenaPolicy::Auto;
volatile bool g_last_big_psram = false;
constexpr size_t kBigAlloc = 32 * 1024;       // the tensor arena is the only allocation this large
constexpr size_t kInternalReserve = 64 * 1024; // Auto: leave this much internal RAM for LTE/lwIP

const uint8_t* g_img = nullptr;

// Pixel packing per SDK: classifier/ei_run_dsp.h extract_image_features() reads one float per
// pixel as 0xRRGGBB and converts to grayscale itself (ITU-R 601: .299+.587+.114 = 1, so R=G=B=v
// gives exactly v). Same packing as EI's firmware-espressif-esp32 ei_run_camera_impulse.cpp:84.
int get_gray_as_rgb888(size_t offset, size_t length, float* out) {
    for (size_t i = 0; i < length; i++) {
        const uint32_t v = g_img[offset + i];
        out[i] = static_cast<float>((v << 16) | (v << 8) | v);
    }
    return 0;
}
}  // namespace

// ESP-NN wants 16-byte aligned buffers (see SDK porting comment, esp-nn issue #7).
void* ei_malloc(size_t size) { return heap_caps_aligned_alloc(16, size, MALLOC_CAP_DEFAULT); }
void ei_free(void* p) { heap_caps_free(p); }
void* ei_calloc(size_t n, size_t size) {
    const size_t bytes = n * size;
    if (bytes < kBigAlloc) return heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
    void* p = nullptr;
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    switch (g_policy) {
        case inference::ArenaPolicy::Internal: p = heap_caps_calloc(n, size, internal); break;
        case inference::ArenaPolicy::Psram: p = heap_caps_calloc(n, size, psram); break;
        default:
            if (heap_caps_get_largest_free_block(internal) >= bytes + 16 &&
                heap_caps_get_free_size(internal) >= bytes + kInternalReserve)
                p = heap_caps_calloc(n, size, internal);
            if (!p) p = heap_caps_calloc(n, size, psram);
    }
    if (p) g_last_big_psram = esp_ptr_external_ram(p);
    ESP_LOGD(TAG, "arena %u B -> %s", (unsigned)bytes, p ? (g_last_big_psram ? "PSRAM" : "internal") : "FAILED");
    return p;
}

namespace inference {

void set_arena_policy(ArenaPolicy p) { g_policy = p; }
int input_width() { return EI_CLASSIFIER_INPUT_WIDTH; }
int input_height() { return EI_CLASSIFIER_INPUT_HEIGHT; }

esp_err_t init() {
    ESP_LOGI(TAG, "EI project '%s' (id %d, deploy v%d), input %dx%d, labels %d, arena(metadata) %d B",
             EI_CLASSIFIER_PROJECT_NAME, EI_CLASSIFIER_PROJECT_ID, EI_CLASSIFIER_PROJECT_DEPLOY_VERSION,
             EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT, EI_CLASSIFIER_LABEL_COUNT,
             EI_CLASSIFIER_TFLITE_LARGEST_ARENA_SIZE);
#if EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN && EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN_S3
    ESP_LOGI(TAG, "ESP-NN enabled (S3 optimized kernels)");
#else
    ESP_LOGW(TAG, "ESP-NN NOT enabled - inference will be slow");
#endif
#if EI_CLASSIFIER_INPUT_WIDTH == 0
    ESP_LOGE(TAG, "model is not an image model");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    return ESP_OK;
}

Result run(const uint8_t* gray, int w, int h) {
    Result r;
    if (w != EI_CLASSIFIER_INPUT_WIDTH || h != EI_CLASSIFIER_INPUT_HEIGHT ||
        (size_t)(w * h) != EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
        r.err = EI_IMPULSE_INVALID_SIZE;
        return r;
    }
    g_img = gray;
    signal_t sig;
    sig.total_length = (size_t)w * h;
    sig.get_data = &get_gray_as_rgb888;

    ei_impulse_result_t res;
    memset(&res, 0, sizeof(res));
    const EI_IMPULSE_ERROR e = run_classifier(&sig, &res, false);
    g_img = nullptr;
    r.err = e;
    r.dsp_ms = res.timing.dsp;
    r.inference_ms = res.timing.classification;
    r.arena_in_psram = g_last_big_psram;
    if (e != EI_IMPULSE_OK) return r;
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
        const auto& b = res.bounding_boxes[i];
        if (b.value <= 0) continue;
        r.boxes.push_back({b.label ? b.label : "", b.value, b.x, b.y, b.width, b.height});
        if (b.value > r.confidence) { r.confidence = b.value; r.label = b.label ? b.label : ""; }
    }
#else
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (res.classification[i].value > r.confidence) {
            r.confidence = res.classification[i].value;
            r.label = res.classification[i].label;
        }
    }
#endif
    return r;
}

}  // namespace inference
