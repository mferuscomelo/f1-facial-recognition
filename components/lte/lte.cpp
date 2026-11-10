// LTE-M via Sequans GM02S + esp_modem (plain PPP, no CMUX yet).
// Sequence mirrors sg-sdk src/platforms/F1/comps/lte/espmodem.c (see docs/pinmap.md).
#include "lte.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "cxx_include/esp_modem_api.hpp"
#include "driver/uart.h"
#include "esp_modem_config.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "modem_power.hpp"
#include "sdkconfig.h"

namespace lte {
namespace {
constexpr const char* TAG = "lte";

// sg-sdk comps/lte/espmodem.h:45-49
constexpr int kUartTx = 47, kUartRx = 48, kUartRts = 33, kUartCts = 6;

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kLostIp = BIT1;

EventGroupHandle_t s_events;
esp_netif_t* s_netif = nullptr;
std::unique_ptr<esp_modem::DCE> s_dce;
std::mutex s_mtx;
Info s_info;
volatile bool s_connected = false;

void on_ip_event(void*, esp_event_base_t, int32_t id, void* data) {
    if (id == IP_EVENT_PPP_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        esp_netif_dns_info_t dns{};
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "PPP up: ip=%s gw=" IPSTR " dns=" IPSTR, ip, IP2STR(&ev->ip_info.gw),
                 IP2STR(&dns.ip.u_addr.ip4));
        {
            std::lock_guard<std::mutex> l(s_mtx);
            s_info.ip = ip;
        }
        xEventGroupSetBits(s_events, kGotIp);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost IP");
        xEventGroupSetBits(s_events, kLostIp);
    }
}

bool at(const char* cmd, std::string& out, int timeout_ms = 5000) {
    out.clear();
    auto r = s_dce->at(cmd, out, timeout_ms);
    if (r != esp_modem::command_result::OK) {
        ESP_LOGW(TAG, "%s -> %s [%s]", cmd, r == esp_modem::command_result::TIMEOUT ? "TIMEOUT" : "FAIL",
                 out.c_str());
        return false;
    }
    return true;
}

bool sync_modem(int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (s_dce->sync() == esp_modem::command_result::OK) return true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

void refresh_signal() {
    int rssi = 99, ber = 99;
    if (s_dce->get_signal_quality(rssi, ber) == esp_modem::command_result::OK) {
        std::lock_guard<std::mutex> l(s_mtx);
        s_info.rssi = rssi;
    }
}

// Raw AT with OK/ERROR detection so error text (+CME ERROR) is visible.
std::string at_raw(const char* cmd, int timeout_ms = 5000) {
    std::string out;
    s_dce->at_raw(std::string(cmd) + "\r", out, "OK", "ERROR", timeout_ms);
    for (auto& ch : out) if (ch == '\r' || ch == '\n') ch = ' ';
    return out;
}

void log_identity() {
    std::string imei, out;
    s_dce->get_imei(imei);
    // SIM may need a few seconds after modem reset; retry (sg-sdk espmodem.c:2569)
    for (int i = 0; i < 15; ++i) {
        out = at_raw("AT+CPIN?");
        ESP_LOGI(TAG, "AT+CPIN? -> [%s]", out.c_str());
        if (out.find("READY") != std::string::npos) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    at("AT+SQNCCID?", out);  // sg-sdk espmodem.c:2782
    std::string iccid = out;
    if (auto p = iccid.find(':'); p != std::string::npos) iccid = iccid.substr(p + 1);
    iccid.erase(0, iccid.find_first_not_of(" \""));
    if (auto q = iccid.find_first_of("\",\r\n"); q != std::string::npos) iccid.resize(q);
    ESP_LOGI(TAG, "IMEI=%s ICCID=%s", imei.c_str(), iccid.c_str());
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.imei = imei;
    s_info.iccid = iccid;
}

// Returns CEREG <stat> or -1.
int cereg_stat() {
    std::string out = at_raw("AT+CEREG?");  // sg-sdk espmodem.c:1574
    if (out.find("OK") == std::string::npos) {
        ESP_LOGW(TAG, "AT+CEREG? -> [%s]", out.c_str());
        return -1;
    }
    auto p = out.find(',');
    return p == std::string::npos ? -1 : atoi(out.c_str() + p + 1);
}

bool attach(const char* apn) {
    std::string out;
    ESP_LOGI(TAG, "attaching with APN \"%s\"", apn);
    at("AT+CFUN=4", out, 10000);  // radio off before changing PDP (sg-sdk espmodem.c:1309-1320)
    esp_modem::PdpContext pdp(apn);
    if (s_dce->set_pdp_context(pdp) != esp_modem::command_result::OK) {
        ESP_LOGE(TAG, "AT+CGDCONT failed");
        return false;
    }
    if (!at("AT+CFUN=1", out, 10000)) return false;  // sg-sdk espmodem.c:1331

    const int64_t deadline = esp_log_timestamp() + CONFIG_APP_LTE_ATTACH_TIMEOUT_S * 1000LL;
    int last = -2;
    while (esp_log_timestamp() < deadline) {
        int st = cereg_stat();
        if (st != last) {
            ESP_LOGI(TAG, "CEREG stat=%d (1=home, 2=searching, 3=denied, 5=roaming) CSQ=%d", st, s_info.rssi);
            last = st;
        }
        if (st == 1 || st == 5) {
            std::lock_guard<std::mutex> l(s_mtx);
            s_info.apn = apn;
            return true;
        }
        refresh_signal();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "no registration within %d s (last CEREG=%d, CSQ=%d)", CONFIG_APP_LTE_ATTACH_TIMEOUT_S,
             last, s_info.rssi);
    return false;
}

bool bring_up_modem(bool hard) {
    namespace mp = board::modem_power;
    if (hard || !mp::is_on()) {
        if (mp::is_on()) {
            mp::reset_pulse();
        } else {
            mp::power_on();
        }
        // SG waits up to 8 s for +SYSSTART (espmodem.c:687-712); we then poll with AT.
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (!sync_modem(15)) {
        ESP_LOGE(TAG, "modem not answering AT");
        return false;
    }
    std::string out;
    at("ATE0", out);
    return true;
}

void supervisor(void*) {
    bool hard = true;  // first boot: reset the modem so we start from a known state
    for (;;) {
        s_connected = false;
        if (!bring_up_modem(hard)) {
            hard = true;
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        log_identity();

        bool ok = attach(CONFIG_APP_LTE_APN);
        if (!ok && strlen(CONFIG_APP_LTE_APN_FALLBACK) > 0) ok = attach(CONFIG_APP_LTE_APN_FALLBACK);
        if (!ok) {
            hard = true;
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        std::string op;
        int act = -1;
        s_dce->get_operator_name(op, act);
        refresh_signal();
        {
            std::lock_guard<std::mutex> l(s_mtx);
            s_info.op_name = op;
            s_info.act = act;
            ESP_LOGI(TAG, "registered: operator=\"%s\" AcT=%d CSQ=%d APN=%s", op.c_str(), act, s_info.rssi,
                     s_info.apn.c_str());
        }

        xEventGroupClearBits(s_events, kGotIp | kLostIp);
        if (!s_dce->set_mode(esp_modem::modem_mode::DATA_MODE)) {
            ESP_LOGE(TAG, "failed to enter data mode");
            hard = true;
            continue;
        }
        EventBits_t b = xEventGroupWaitBits(s_events, kGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
        if (!(b & kGotIp)) {
            ESP_LOGE(TAG, "no PPP IP within 60 s");
            s_dce->set_mode(esp_modem::modem_mode::COMMAND_MODE);
            hard = true;
            continue;
        }
        s_connected = true;
        xEventGroupWaitBits(s_events, kLostIp, pdTRUE, pdFALSE, portMAX_DELAY);
        s_connected = false;
        ESP_LOGW(TAG, "connection lost, reconnecting");
        hard = !s_dce->set_mode(esp_modem::modem_mode::COMMAND_MODE);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
}  // namespace

esp_err_t start() {
    s_events = xEventGroupCreate();
    if (esp_err_t e = board::modem_power::init(); e != ESP_OK) return e;
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_netif = esp_netif_new(&netif_cfg);

    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = UART_NUM_1;
    dte_cfg.uart_config.tx_io_num = kUartTx;
    dte_cfg.uart_config.rx_io_num = kUartRx;
    dte_cfg.uart_config.rts_io_num = kUartRts;
    dte_cfg.uart_config.cts_io_num = kUartCts;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW;  // sg-sdk espmodem.c:1042
    dte_cfg.uart_config.baud_rate = 115200;                        // GM02S factory default
    dte_cfg.uart_config.rx_buffer_size = 4096;                     // sg-sdk espmodem.c:1022-1024
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.uart_config.event_queue_size = 20;
    auto dte = esp_modem::create_uart_dte(&dte_cfg);
    if (!dte) return ESP_FAIL;

    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_APP_LTE_APN);
    s_dce = esp_modem::create_SQNGM02S_dce(&dce_cfg, dte, s_netif);
    if (!s_dce) return ESP_FAIL;

    xTaskCreatePinnedToCore(supervisor, "lte", 6144, nullptr, 5, nullptr, 0);
    return ESP_OK;
}

bool is_connected() { return s_connected; }

Info info() {
    std::lock_guard<std::mutex> l(s_mtx);
    return s_info;
}

int publish(const std::string& json) {
    if (!s_connected) return -1;
    if (strlen(CONFIG_APP_PUBLISH_URL) == 0) {
        ESP_LOGW(TAG, "CONFIG_APP_PUBLISH_URL empty, not publishing: %s", json.c_str());
        return -2;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_APP_PUBLISH_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -3;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json.data(), json.size());
    const int64_t t0 = esp_log_timestamp();
    esp_err_t err = esp_http_client_perform(c);
    int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -4;
    ESP_LOGI(TAG, "POST %s -> %d (%s) in %d ms", json.c_str(), status, esp_err_to_name(err),
             int(esp_log_timestamp() - t0));
    esp_http_client_cleanup(c);
    return status;
}

}  // namespace lte
