// Thin wrapper around the Edge Impulse C++ SDK: run_classifier() on a grayscale image.
#include "inference.hpp"

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "face_id.hpp"
#include "test_images.h"

static const char* TAG = "inference";

// The SDK allocates its memory through these hooks, so we decide where it goes:
//  - every buffer is 16-byte aligned, because the ESP32-S3's vector instructions (used by the
//    ESP-NN kernels) ignore the low 4 address bits;
//  - the big tensor arena (about 150 KB) goes to PSRAM, keeping internal RAM free for USB and LTE.
static void* aligned_alloc16(size_t bytes) {
    bytes = (bytes + 15) & ~size_t(15);
    const uint32_t caps = bytes >= 32 * 1024 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT;
    return heap_caps_aligned_calloc(16, 1, bytes, caps);
}
void* ei_malloc(size_t size) { return aligned_alloc16(size); }
void* ei_calloc(size_t n, size_t size) { return aligned_alloc16(n * size); }
void ei_free(void* p) { heap_caps_free(p); }

namespace inference {

int input_width() { return EI_CLASSIFIER_INPUT_WIDTH; }
int input_height() { return EI_CLASSIFIER_INPUT_HEIGHT; }

Result run(const uint8_t* gray) {
    // The SDK pulls pixels through a callback, one float per pixel packed as 0xRRGGBB.
    // With R = G = B it computes exactly our gray value again.
    static const uint8_t* image;
    image = gray;
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = [](size_t offset, size_t length, float* out) {
        for (size_t i = 0; i < length; i++) {
            const uint32_t v = image[offset + i];
            out[i] = float((v << 16) | (v << 8) | v);
        }
        return 0;
    };

    ei_impulse_result_t res = {};
    Result r;
    r.ok = run_classifier(&signal, &res, false) == EI_IMPULSE_OK;
    r.time_ms = res.timing.classification;
    for (uint32_t i = 0; i < res.bounding_boxes_count; i++) {
        const auto& box = res.bounding_boxes[i];
        if (box.value <= 0 || r.count == 10) continue;  // empty slot (or no room left)
        r.boxes[r.count++] = {int(box.x), int(box.y), int(box.width), int(box.height), box.value};
        if (box.value > r.confidence) {
            r.confidence = box.value;
            r.x = box.x + box.width / 2;
            r.y = box.y + box.height / 2;
        }
    }
    r.face = r.count > 0;
    return r;
}

void self_test() {
    ESP_LOGI(TAG, "model '%s' (Edge Impulse project %d), input %dx%d", EI_CLASSIFIER_PROJECT_NAME,
             EI_CLASSIFIER_PROJECT_ID, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);
    struct Photo { const char* name; const uint8_t* pixels; bool has_face; };
    const Photo photos[] = {
        {"astronaut (close-up)", testimg::astronaut_face, true},
        {"astronaut (portrait)", testimg::astronaut_full, true},
        {"coffee cup", testimg::coffee, false},
    };
    for (const auto& p : photos) {
        const Result r = run(p.pixels);
        ESP_LOGI(TAG, "self-test %-21s %s  (%d faces, best %.2f, %d ms)", p.name,
                 r.ok && r.face == p.has_face ? "PASS" : "FAIL", r.count, r.confidence, r.time_ms);
    }

    // Face recognition: Eileen Collins must match another photo of herself, and nobody else.
    if (!face_id::available()) return;
    static float collins[face_id::kDims], other[face_id::kDims];
    face_id::embed(testimg::face_collins_a, collins);
    struct Face { const char* name; const uint8_t* rgb; bool same; };
    const Face faces[] = {
        {"Eileen Collins (other photo)", testimg::face_collins_b, true},
        {"Neil Armstrong", testimg::face_armstrong, false},
        {"Sally Ride", testimg::face_ride, false},
    };
    for (const auto& f : faces) {
        const int ms = face_id::embed(f.rgb, other);
        const float sim = face_id::similarity(collins, other);
        const bool pass = ms >= 0 && (sim >= face_id::kMatch) == f.same;
        ESP_LOGI(TAG, "self-test %-28s %s  (similarity to Collins %.2f, %d ms)", f.name, pass ? "PASS" : "FAIL", sim, ms);
    }
}

}  // namespace inference
