// LTE-M with Espressif's esp_modem library, in CMUX mode: one UART carries both AT commands
// and the PPP data link, so we can keep checking the signal while online.
// The sequence mirrors SG Wireless's own F1 SDK (see docs/pinmap.md).
#include "lte.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "cxx_include/esp_modem_api.hpp"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem_power.hpp"
#include "sdkconfig.h"

namespace lte {
namespace {
const char* TAG = "lte";
using esp_modem::command_result;
using esp_modem::modem_mode;

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kLostIp = BIT1;

EventGroupHandle_t s_events;
std::unique_ptr<esp_modem::DCE> s_dce;
std::atomic<bool> s_connected{false};
std::atomic<int> s_csq{99};
QueueHandle_t s_outbox;  // char* (heap copies of the JSON messages)

void on_ip_event(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == IP_EVENT_PPP_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "online, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, kGotIp);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "lost IP");
        xEventGroupSetBits(s_events, kLostIp);
    }
}

bool at(const char* cmd, int timeout_ms = 10000) {
    std::string out;
    return s_dce->at(cmd, out, timeout_ms) == command_result::OK;
}

bool update_signal() {
    int rssi = 99, ber = 99;
    if (s_dce->get_signal_quality(rssi, ber) != command_result::OK) return false;
    s_csq = rssi;
    return true;
}

// AT+CEREG? answers "+CEREG: <n>,<stat>": stat 1 = home network, 5 = roaming.
bool wait_registered(int timeout_s) {
    for (int t = 0; t < timeout_s; t += 2) {
        std::string out;
        s_dce->at("AT+CEREG?", out, 5000);
        const auto comma = out.find(',');
        const int stat = comma == std::string::npos ? -1 : atoi(out.c_str() + comma + 1);
        update_signal();
        if (stat == 1 || stat == 5) {
            ESP_LOGI(TAG, "registered (%s), CSQ %d", stat == 1 ? "home" : "roaming", s_csq.load());
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "not registered after %d s", timeout_s);
    return false;
}

// Reset the modem, attach with our APN and start CMUX + PPP. Returns true once we have an IP.
bool connect() {
    // 1. Fresh start: leave CMUX if we were in it, then reset the modem.
    if (s_dce->get_mode() == modem_mode::CMUX_MODE && !s_dce->set_mode(modem_mode::COMMAND_MODE))
        s_dce->set_mode(modem_mode::UNDEF);
    modem_power::restart();
    vTaskDelay(pdMS_TO_TICKS(5000));                  // modem boot time
    s_dce->set_mode(modem_mode::RESUME_COMMAND_MODE);  // after reset the modem talks plain AT
    bool alive = false;
    for (int i = 0; i < 15 && !alive; ++i) {
        alive = s_dce->sync() == command_result::OK;
        if (!alive) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!alive) {
        ESP_LOGE(TAG, "modem not answering");
        return false;
    }
    at("ATE0");  // no command echo

    // 2. Attach: radio off, set the APN, radio on, wait for the network.
    ESP_LOGI(TAG, "attaching with APN \"%s\"", CONFIG_APP_LTE_APN);
    at("AT+CFUN=4");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_modem::PdpContext pdp(CONFIG_APP_LTE_APN);
    if (s_dce->set_pdp_context(pdp) != command_result::OK || !at("AT+CFUN=1")) return false;
    if (!wait_registered(180)) return false;

    // 3. CMUX + PPP. set_mode() sometimes reports failure although the mux is up (the SG SDK
    // notes the same), so we trust get_mode() and let the IP wait decide.
    xEventGroupClearBits(s_events, kGotIp | kLostIp);
    s_dce->set_mode(modem_mode::CMUX_MODE);
    if (s_dce->get_mode() != modem_mode::CMUX_MODE) {
        ESP_LOGE(TAG, "could not enter CMUX mode");
        return false;
    }
    return xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000)) & kGotIp;
}

// Connect, then watch the link: every 30 s ask for the signal; reconnect if the IP is lost
// or the modem stops answering.
void lte_task(void*) {
    for (;;) {
        if (connect()) {
            s_connected = true;
            int missed = 0;
            while (missed < 3) {
                const EventBits_t bits =
                    xEventGroupWaitBits(s_events, kLostIp, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
                if (bits & kLostIp) break;
                missed = update_signal() ? 0 : missed + 1;
            }
            s_connected = false;
            ESP_LOGW(TAG, "link down, reconnecting");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// One HTTP POST. Returns the HTTP status, or -1 on a network error.
int http_post(const char* json) {
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_APP_PUBLISH_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));
    const int status = esp_http_client_perform(client) == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

// Sends queued messages. Waits for the link, then tries each message up to 3 times.
void publish_task(void*) {
    for (;;) {
        char* json = nullptr;
        xQueueReceive(s_outbox, &json, portMAX_DELAY);
        while (!s_connected) vTaskDelay(pdMS_TO_TICKS(1000));
        for (int attempt = 1; attempt <= 3; ++attempt) {
            const int status = http_post(json);
            if (status >= 200 && status < 300) {
                ESP_LOGI(TAG, "sent %s", json);
                break;
            }
            ESP_LOGW(TAG, "POST failed (%d), attempt %d/3", status, attempt);
            vTaskDelay(pdMS_TO_TICKS(2000 * attempt));
        }
        free(json);
    }
}
}  // namespace

esp_err_t start() {
    if (strlen(CONFIG_APP_PUBLISH_URL) == 0) ESP_LOGW(TAG, "CONFIG_APP_PUBLISH_URL is empty: see README");
    if (esp_err_t e = modem_power::init(); e != ESP_OK) return e;
    s_events = xEventGroupCreate();
    s_outbox = xQueueCreate(8, sizeof(char*));
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr);

    // Modem UART: UART1, TX 47, RX 48, RTS 33, CTS 6, hardware flow control (SG SDK values).
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = UART_NUM_1;
    dte_cfg.uart_config.tx_io_num = 47;
    dte_cfg.uart_config.rx_io_num = 48;
    dte_cfg.uart_config.rts_io_num = 33;
    dte_cfg.uart_config.cts_io_num = 6;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW;
    dte_cfg.uart_config.baud_rate = 115200;
    dte_cfg.uart_config.rx_buffer_size = 4096;
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.uart_config.event_queue_size = 20;
    dte_cfg.dte_buffer_size = 2048;  // CMUX frames are decoded in this buffer
    dte_cfg.task_stack_size = 4096;
    dte_cfg.task_priority = 4;
    auto dte = esp_modem::create_uart_dte(&dte_cfg);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_APP_LTE_APN);
    s_dce = esp_modem::create_SQNGM02S_dce(&dce_cfg, dte, esp_netif_new(&netif_cfg));
    if (!s_dce) return ESP_FAIL;

    xTaskCreatePinnedToCore(lte_task, "lte", 6144, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(publish_task, "publish", 6144, nullptr, 4, nullptr, 0);
    return ESP_OK;
}

bool is_connected() { return s_connected; }
int signal_quality() { return s_csq; }

bool publish(const char* json) {
    char* copy = strdup(json);
    if (copy && xQueueSend(s_outbox, &copy, 0) == pdTRUE) return true;
    free(copy);
    return false;
}

}  // namespace lte
