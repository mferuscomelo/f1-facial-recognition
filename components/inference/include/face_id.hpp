#pragma once
#include <cstdint>

// Face recognition: turns a face picture into a "fingerprint" (embedding), a list of numbers
// that is close to the fingerprint of another picture of the same person.
// Model: MobileFaceNet, brought into Edge Impulse with "Bring your own model" and exported as a
// C++ library; its .tflite is in components/inference/ei-face-id/ (see docs/face-recognition.md).
namespace face_id {

constexpr int kSize = 96;   // the model looks at a 96x96 colour picture of one face
constexpr int kDims = 256;  // numbers in one fingerprint

// `rgb` is a kSize x kSize picture, 3 bytes per pixel (R, G, B), centred on one face.
// Writes a fingerprint of length 1 into `out` (so similarity is a dot product).
// Returns the time spent in the model in ms, or -1 on error.
int embed(const uint8_t* rgb, float* out);

// Similarity of two fingerprints: about 0.6+ for the same person, below 0.3 for strangers.
float similarity(const float* a, const float* b);
constexpr float kMatch = 0.45f;  // at or above this, two fingerprints count as the same person

}  // namespace face_id
