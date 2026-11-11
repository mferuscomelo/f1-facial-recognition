#pragma once
#include "esp_err.h"
#include "inference.hpp"

// Optional live preview: the F1 opens its own Wi-Fi network and serves a web page with the
// camera image and the detected faces. Turned on with CONFIG_APP_PREVIEW (see README).
// Frames are only sent to the browser that asks for them; nothing is stored.
namespace preview {

// Starts the Wi-Fi access point and the web server. Does nothing if the preview is off.
esp_err_t start();

// Hands the latest model result to the web page.
void update(const inference::Result& result);

}  // namespace preview
