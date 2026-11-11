#pragma once
#include "esp_err.h"
#include "inference.hpp"
#include "people.hpp"

// Optional live preview (CONFIG_APP_PREVIEW): the F1 opens its own Wi-Fi network and serves a
// web page with the camera image, the detected faces and who they are. The page is also where
// you register people. Without CONFIG_APP_PREVIEW these functions do nothing.
namespace preview {

esp_err_t start();

// Latest result for the page. `who`: the best face's identity, or nullptr if not checked.
// `note`: a status line, e.g. "Registering Ada: picture 2 of 5" ("" for none).
void update(const inference::Result& r, const people::Match* who, const char* note);

}  // namespace preview
