#pragma once
#include <cstddef>

#include "esp_err.h"

// The people the F1 knows: a name and a face fingerprint for each, stored in flash
// (the "people" partition, see partitions.csv). Safe to call from any task.
namespace people {

constexpr int kMax = 20;         // people
constexpr int kNameLen = 32;     // bytes, including the final '\0'
constexpr int kEnrollSamples = 5;  // pictures taken when someone registers

esp_err_t start();  // loads the list from flash
int count();

// Who is this fingerprint? `id` is -1 if nobody is similar enough (see face_id::kMatch).
struct Match {
    int id = -1;
    char name[kNameLen] = "";
    float similarity = 0;
};
Match identify(const float* fingerprint);

// Adds one more picture of `name` (a new person, or more pictures of a known one).
esp_err_t add_sample(const char* name, const float* fingerprint);
esp_err_t rename(int id, const char* name);
esp_err_t remove(int id);

// The list as JSON, e.g. [{"id":0,"name":"Ada","samples":5}]. Returns the length.
int to_json(char* buf, size_t len);

// Registering someone takes several pictures. The web page asks for it, the camera loop does it.
void enroll_start(const char* name);
bool enroll_active(char* name_out);  // copies the name being registered, if any
void enroll_stop();

}  // namespace people
