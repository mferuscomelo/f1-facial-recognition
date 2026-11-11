// Runs the face recognition model (Edge Impulse "Bring your own model" export) with
// TensorFlow Lite Micro from the Edge Impulse SDK.
//
// Why not run_classifier()? One firmware can only hold one Edge Impulse "impulse" with that API,
// and the face detector already uses it. So we hand this model straight to the SDK's
// TensorFlow Lite Micro interpreter, using the list of operations Edge Impulse generated for it.
#include "face_id.hpp"

#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if !FACE_ID_MODEL
namespace face_id {
bool available() { return false; }
int embed(const uint8_t*, float*) { return -1; }
float similarity(const float*, const float*) { return 0; }
}  // namespace face_id
#else

#include "edge-impulse-sdk/tensorflow/lite/micro/micro_interpreter.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "edge-impulse-sdk/tensorflow/lite/schema/schema_generated.h"
#include "fast_prelu.hpp"
#include "tflite-resolver.h"  // from the export: EI_TFLITE_RESOLVER, the operations this model uses

// The .tflite file from the export, linked into flash as-is (the path comes from CMakeLists.txt).
asm(".section .rodata.face_id_model\n"
    ".balign 16\n"
    ".global face_id_model\n"
    "face_id_model:\n"
    ".incbin \"" FACE_ID_TFLITE "\"\n"
    ".global face_id_model_end\n"
    "face_id_model_end:\n"
    ".previous\n");
extern "C" const uint8_t face_id_model[], face_id_model_end[];

namespace face_id {
namespace {
const char* TAG = "face_id";
constexpr size_t kArenaBytes = 1024 * 1024;  // Edge Impulse estimates ~850 KB; it lives in PSRAM

tflite::MicroInterpreter* s_interpreter;


bool init() {
    if (s_interpreter) return true;
    // The F1's flash runs at DIO 40 MHz; PSRAM (quad, 80 MHz) is faster, and the model reads all
    // 1.1 MB of weights on every run. So copy the model to PSRAM once.
    const size_t model_bytes = face_id_model_end - face_id_model;
    auto* copy = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, model_bytes, MALLOC_CAP_SPIRAM));
    if (!copy) return false;
    memcpy(copy, face_id_model, model_bytes);
    const tflite::Model* model = tflite::GetModel(copy);
    auto* arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_SPIRAM));
    if (!arena) return false;
    EI_TFLITE_RESOLVER  // declares `resolver`
    static FastPreluResolver ops(resolver);
    s_interpreter = new tflite::MicroInterpreter(model, ops, arena, kArenaBytes);
    if (s_interpreter->AllocateTensors(true) != kTfLiteOk) {
        ESP_LOGE(TAG, "model does not fit / has unsupported operations");
        delete s_interpreter;
        s_interpreter = nullptr;
        heap_caps_free(arena);
        return false;
    }
    const TfLiteTensor* in = s_interpreter->input(0);
    const TfLiteTensor* out = s_interpreter->output(0);
    if (in->type != kTfLiteInt8 || in->bytes != kSize * kSize * 3 || out->bytes != kDims) {
        ESP_LOGE(TAG, "unexpected model: needs int8 input 96x96x3 and %d outputs", kDims);
        return false;
    }
    ESP_LOGI(TAG, "face recognition model ready, %u KB working memory",
             unsigned(s_interpreter->arena_used_bytes() / 1024));
    return true;
}
}  // namespace

bool available() { return true; }

int embed(const uint8_t* rgb, float* out) {
    if (!init()) return -1;
    TfLiteTensor* in = s_interpreter->input(0);
    // MobileFaceNet takes raw 0..255 pixels in B, G, R order; it normalises them itself.
    const float in_scale = in->params.scale;
    const int in_zero = in->params.zero_point;
    for (int i = 0; i < kSize * kSize; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int q = int(lroundf(rgb[i * 3 + (2 - c)] / in_scale)) + in_zero;
            in->data.int8[i * 3 + c] = int8_t(q < -128 ? -128 : q > 127 ? 127 : q);
        }
    }
    const int64_t start = esp_timer_get_time();
    const bool ok = s_interpreter->Invoke() == kTfLiteOk;
    const int ms = int((esp_timer_get_time() - start) / 1000);
    vTaskDelay(1);  // ~2.4 s of work: let the idle task run, or the watchdog complains
    if (!ok) return -1;

    const TfLiteTensor* o = s_interpreter->output(0);
    float norm = 0;
    for (int i = 0; i < kDims; ++i) {
        out[i] = (o->data.int8[i] - o->params.zero_point) * o->params.scale;
        norm += out[i] * out[i];
    }
    norm = sqrtf(norm) + 1e-9f;
    for (int i = 0; i < kDims; ++i) out[i] /= norm;
    return ms;
}

float similarity(const float* a, const float* b) {
    float dot = 0;
    for (int i = 0; i < kDims; ++i) dot += a[i] * b[i];
    return dot;
}

}  // namespace face_id
#endif
