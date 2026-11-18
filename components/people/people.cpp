// Known people, kept in RAM and saved to the "people" flash partition (NVS).
// Each person is one NVS entry "p<id>" holding a Person record.
//
// Flashing new firmware starts from scratch: the partition remembers which firmware wrote it,
// and a different firmware wipes it (and the Wi-Fi settings in "nvs") on its first boot.
#include "people.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "face_id.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace people {
namespace {
const char* TAG = "people";
constexpr const char* kPartition = "people";

struct Person {
    char name[kNameLen];             // empty = free slot
    int samples;                     // pictures averaged into the fingerprint
    float fingerprint[face_id::kDims];  // length 1
};

Person* s_people;  // kMax entries, in PSRAM
SemaphoreHandle_t s_lock;
nvs_handle_t s_nvs;
char s_enrolling[kNameLen];  // name being registered, "" if none

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

esp_err_t save(int id) {
    char key[8];
    snprintf(key, sizeof(key), "p%d", id);
    esp_err_t err = s_people[id].name[0] ? nvs_set_blob(s_nvs, key, &s_people[id], sizeof(Person))
                                         : nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

// Wipes everything the F1 has learned if this firmware is not the one that wrote it.
esp_err_t reset_if_new_firmware() {
    uint8_t running[32], stored[32];
    size_t len = sizeof(stored);
    memcpy(running, esp_app_get_description()->app_elf_sha256, sizeof(running));
    if (nvs_get_blob(s_nvs, "firmware", stored, &len) == ESP_OK && len == sizeof(stored) &&
        memcmp(stored, running, sizeof(running)) == 0) return ESP_OK;
    ESP_LOGW(TAG, "new firmware: forgetting all registered people");
    nvs_close(s_nvs);
    nvs_flash_erase_partition(kPartition);
    nvs_flash_erase();  // the default "nvs" partition (Wi-Fi data)
    esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err == ESP_OK) err = nvs_open_from_partition(kPartition, "people", NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) err = nvs_set_blob(s_nvs, "firmware", running, sizeof(running));
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

int find(const char* name) {
    for (int i = 0; i < kMax; ++i)
        if (s_people[i].name[0] && strcasecmp(s_people[i].name, name) == 0) return i;
    return -1;
}
}  // namespace

esp_err_t start() {
    s_lock = xSemaphoreCreateMutex();
    s_people = static_cast<Person*>(heap_caps_calloc(kMax, sizeof(Person), MALLOC_CAP_SPIRAM));
    if (!s_people) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(kPartition);
        err = nvs_flash_init_partition(kPartition);
    }
    if (err == ESP_OK) err = nvs_open_from_partition(kPartition, "people", NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) err = reset_if_new_firmware();
    if (err != ESP_OK) return err;
    for (int i = 0; i < kMax; ++i) {
        char key[8];
        size_t len = sizeof(Person);
        snprintf(key, sizeof(key), "p%d", i);
        if (nvs_get_blob(s_nvs, key, &s_people[i], &len) != ESP_OK || len != sizeof(Person)) s_people[i] = {};
    }
    ESP_LOGI(TAG, "%d people known", count());
    return ESP_OK;
}

int count() {
    Lock lock;
    int n = 0;
    for (int i = 0; i < kMax; ++i) n += s_people[i].name[0] != 0;
    return n;
}

Match identify(const float* fingerprint) {
    Lock lock;
    Match best;
    for (int i = 0; i < kMax; ++i) {
        if (!s_people[i].name[0]) continue;
        const float sim = face_id::similarity(fingerprint, s_people[i].fingerprint);
        if (sim > best.similarity) {
            best.similarity = sim;
            best.id = i;
            strlcpy(best.name, s_people[i].name, kNameLen);
        }
    }
    if (best.similarity < face_id::kMatch) {
        best.id = -1;
        best.name[0] = 0;
    }
    return best;
}

esp_err_t add_sample(const char* name, const float* fingerprint) {
    Lock lock;
    int id = find(name);
    if (id < 0) {  // new person: take the first free slot
        for (id = 0; id < kMax && s_people[id].name[0]; ++id) {}
        if (id == kMax) return ESP_ERR_NO_MEM;
        s_people[id] = {};
        strlcpy(s_people[id].name, name, kNameLen);
    }
    // Running average of all pictures, scaled back to length 1.
    Person& p = s_people[id];
    float norm = 0;
    for (int i = 0; i < face_id::kDims; ++i) {
        p.fingerprint[i] = p.fingerprint[i] * p.samples + fingerprint[i];
        norm += p.fingerprint[i] * p.fingerprint[i];
    }
    norm = sqrtf(norm) + 1e-9f;
    for (int i = 0; i < face_id::kDims; ++i) p.fingerprint[i] /= norm;
    p.samples++;
    return save(id);
}

esp_err_t rename(int id, const char* name) {
    Lock lock;
    if (id < 0 || id >= kMax || !s_people[id].name[0] || !name[0]) return ESP_ERR_INVALID_ARG;
    const int other = find(name);
    if (other >= 0 && other != id) return ESP_ERR_INVALID_STATE;  // name already taken
    strlcpy(s_people[id].name, name, kNameLen);
    return save(id);
}

esp_err_t remove(int id) {
    Lock lock;
    if (id < 0 || id >= kMax || !s_people[id].name[0]) return ESP_ERR_INVALID_ARG;
    s_people[id] = {};
    return save(id);
}

int to_json(char* buf, size_t len) {
    Lock lock;
    int n = snprintf(buf, len, "[");
    for (int i = 0; i < kMax && n < int(len); ++i) {
        if (!s_people[i].name[0]) continue;
        // Names come from the web page: drop characters that would break the JSON.
        char safe[kNameLen];
        int k = 0;
        for (const char* c = s_people[i].name; *c && k < kNameLen - 1; ++c)
            if (*c != '"' && *c != '\\' && static_cast<unsigned char>(*c) >= 0x20) safe[k++] = *c;
        safe[k] = 0;
        n += snprintf(buf + n, len - n, "%s{\"id\":%d,\"name\":\"%s\",\"samples\":%d}", n > 1 ? "," : "", i, safe,
                      s_people[i].samples);
    }
    if (n < int(len)) n += snprintf(buf + n, len - n, "]");
    return n;
}

void enroll_start(const char* name) {
    Lock lock;
    strlcpy(s_enrolling, name, kNameLen);
}

bool enroll_active(char* name_out) {
    Lock lock;
    if (name_out) strlcpy(name_out, s_enrolling, kNameLen);
    return s_enrolling[0] != 0;
}

void enroll_stop() {
    Lock lock;
    s_enrolling[0] = 0;
}

}  // namespace people
