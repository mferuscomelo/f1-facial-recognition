// LTE-M via Sequans GM02S + esp_modem 1.4.3, CMUX (default) or plain PPP (Kconfig APP_LTE_MODE_*).
// Sequence mirrors sg-sdk src/platforms/F1/comps/lte/espmodem.c / ppp_bridge.c (see docs/pinmap.md).
//
// Threading: s_dce is used ONLY by the supervisor task. esp_modem additionally serialises every AT
// command and mode switch on the DTE's internal recursive mutex (esp_modem_dte.cpp DTE::command,
// esp_modem_dce.cpp DCE_Mode::set). Other tasks only read the Info snapshot (s_mtx) and use the
// PPP netif through lwIP (publish()).
#include "lte.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>

#include "cxx_include/esp_modem_api.hpp"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem_power.hpp"
#include "sdkconfig.h"

namespace lte {
namespace {
constexpr const char* TAG = "lte";

#if CONFIG_APP_LTE_MODE_CMUX
constexpr bool kUseCmux = true;
constexpr esp_modem::modem_mode kDataMode = esp_modem::modem_mode::CMUX_MODE;
#else
constexpr bool kUseCmux = false;
constexpr esp_modem::modem_mode kDataMode = esp_modem::modem_mode::DATA_MODE;
#endif

// sg-sdk comps/lte/espmodem.h:45-49
constexpr int kUartTx = 47, kUartRx = 48, kUartRts = 33, kUartCts = 6;

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kLostIp = BIT1;
constexpr EventBits_t kPppError = BIT2;    // NETIF_PPP_STATUS error event (PPP state machine gave up)
constexpr EventBits_t kModemBoot = BIT3;   // +SYSSTART / +SHUTDOWN URC seen in command mode
constexpr EventBits_t kCheckNow = BIT4;    // publish() failed: run a CMUX health check right away

constexpr int kHealthFailLimit = 3;   // consecutive failed AT polls -> modem considered hung
constexpr int kDeregLimit = 2;        // consecutive "not registered" polls -> reconnect
constexpr int kSoftReregTimeoutS = 90;

enum class Level { kSoft, kRadio, kHard };
enum class LinkEnd { kLinkLost, kDetached, kModemDead };

EventGroupHandle_t s_events;
esp_netif_t* s_netif = nullptr;
std::shared_ptr<esp_modem::DTE> s_dte;
std::unique_ptr<esp_modem::DCE> s_dce;
std::mutex s_mtx;  // guards s_info
Info s_info;
std::atomic<bool> s_connected{false};
std::atomic<unsigned> s_term_errors{0};

struct PubItem {
    std::string json;
    bool must_deliver;
};
QueueHandle_t s_pub_q = nullptr;  // PubItem*
constexpr int kPubQueueLen = 6;
constexpr int kPubAttempts = 3;
constexpr int kPubAttemptTimeoutMs = 10000;
void publisher_task(void*);

const char* mode_name(esp_modem::modem_mode m) {
    using esp_modem::modem_mode;
    switch (m) {
        case modem_mode::UNDEF: return "UNDEF";
        case modem_mode::COMMAND_MODE: return "COMMAND";
        case modem_mode::DATA_MODE: return "DATA";
        case modem_mode::CMUX_MODE: return "CMUX";
        default: return "other";
    }
}

const char* level_name(Level l) {
    return l == Level::kSoft ? "soft" : l == Level::kRadio ? "radio" : "hard";
}

Level escalate(Level l) { return l == Level::kSoft ? Level::kRadio : Level::kHard; }

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
        {
            std::lock_guard<std::mutex> l(s_mtx);
            s_info.ip.clear();
        }
        xEventGroupSetBits(s_events, kLostIp);
    }
}

// Same filter as esp_modem's own handler (esp_modem_netif.cpp Netif::on_ppp_changed):
// ids between NETIF_PPP_ERRORNONE and NETIF_PP_PHASE_OFFSET are PPP error/state events.
void on_ppp_status(void*, esp_event_base_t, int32_t id, void*) {
    if (id > NETIF_PPP_ERRORNONE && id < NETIF_PP_PHASE_OFFSET) {
        ESP_LOGW(TAG, "PPP status error event %d", (int)id);
        xEventGroupSetBits(s_events, kPppError);
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

// Raw AT with OK/ERROR detection so error text (+CME ERROR) is visible. Empty string on timeout.
std::string at_raw(const char* cmd, int timeout_ms = 5000) {
    std::string out;
    s_dce->at_raw(std::string(cmd) + "\r", out, "OK", "ERROR", timeout_ms);
    for (auto& ch : out)
        if (ch == '\r' || ch == '\n') ch = ' ';
    return out;
}

// Extract the value after "<prefix>" with surrounding spaces/quotes removed (up to , " or end).
std::string field_after(const std::string& s, const char* prefix) {
    auto p = s.find(prefix);
    if (p == std::string::npos) return {};
    p += strlen(prefix);
    p = s.find_first_not_of(" \"", p);
    if (p == std::string::npos) return {};
    auto q = s.find_first_of("\", ", p);
    return s.substr(p, q == std::string::npos ? std::string::npos : q - p);
}

bool sync_modem(int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (s_dce->sync() == esp_modem::command_result::OK) return true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

// Returns true if the modem answered.
bool refresh_signal() {
    int rssi = 99, ber = 99;
    if (s_dce->get_signal_quality(rssi, ber) != esp_modem::command_result::OK) return false;
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.rssi = rssi;
    s_info.rssi_dbm = (rssi == 99) ? -999 : (-113 + rssi * 2);  // sg-sdk espmodem.c:1670
    return true;
}

// Returns CEREG <stat>, or -1 if the modem did not answer OK.
int cereg_stat() {
    std::string out = at_raw("AT+CEREG?");  // sg-sdk espmodem.c:1574
    if (out.find("OK") == std::string::npos) {
        ESP_LOGW(TAG, "AT+CEREG? -> [%s]", out.c_str());
        return -1;
    }
    auto p = out.find("+CEREG:");
    if (p == std::string::npos) return -1;
    p = out.find(',', p);  // +CEREG: <n>,<stat>[,...]  (sg-sdk espmodem.c:1581)
    const int st = p == std::string::npos ? -1 : atoi(out.c_str() + p + 1);
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.cereg = st;
    return st;
}

bool is_registered(int st) { return st == 1 || st == 5; }

void read_imei() {
    std::string imei;
    if (s_dce->get_imei(imei) == esp_modem::command_result::OK) {
        std::lock_guard<std::mutex> l(s_mtx);
        s_info.imei = imei;
    }
    ESP_LOGI(TAG, "IMEI=%s", imei.c_str());
}

// SIM queries only work once the radio is on (CFUN=1) or in CFUN=4; we run this after
// registration. Sequence: AT+CPIN? until READY, then AT+SQNCCID? (sg-sdk espmodem.c:2540-2582,
// 2764-2810).
void read_iccid() {
    {
        std::lock_guard<std::mutex> l(s_mtx);
        if (!s_info.iccid.empty()) return;
    }
    bool ready = false;
    for (int i = 0; i < 15 && !ready; ++i) {  // 15 tries x 250 ms (sg-sdk espmodem.c:2561-2566)
        if (i) vTaskDelay(pdMS_TO_TICKS(250));
        ready = at_raw("AT+CPIN?").find("+CPIN: READY") != std::string::npos;
    }
    if (!ready) {
        ESP_LOGW(TAG, "SIM not READY (AT+CPIN?)");
        return;
    }
    const std::string out = at_raw("AT+SQNCCID?");  // +SQNCCID: "8988..." (sg-sdk espmodem.c:2791)
    const std::string iccid = field_after(out, "+SQNCCID:");
    ESP_LOGI(TAG, "ICCID=%s", iccid.empty() ? ("? [" + out + "]").c_str() : iccid.c_str());
    if (!iccid.empty()) {
        std::lock_guard<std::mutex> l(s_mtx);
        s_info.iccid = iccid;
    }
}

// RAT: +COPS AcT is 7 for both Cat-M1 and NB-IoT on GM02S, so ask the modem which mode is active.
// AT+SQNMODEACTIVE? -> "+SQNMODEACTIVE: <1|2>", 1 = Cat-M1, 2 = NB-IoT (sg-sdk espmodem.c:1242-1256,
// 2587-2611).
void read_rat() {
    const std::string out = at_raw("AT+SQNMODEACTIVE?");
    const std::string v = field_after(out, "+SQNMODEACTIVE:");
    const int mode = v.empty() ? -1 : atoi(v.c_str());
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.sqn_mode = mode;
    s_info.rat = mode == 1 ? "Cat-M1" : mode == 2 ? "NB-IoT" : "?";
    if (mode < 0) ESP_LOGW(TAG, "AT+SQNMODEACTIVE? -> [%s]", out.c_str());
}

void read_network_info() {
    read_iccid();
    read_rat();
    std::string op;
    int act = -1;
    s_dce->get_operator_name(op, act);
    refresh_signal();
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.op_name = op;
    s_info.act = act;
    ESP_LOGI(TAG, "registered: operator=\"%s\" AcT=%d RAT=%s CSQ=%d (%d dBm) APN=%s", op.c_str(), act,
             s_info.rat, s_info.rssi, s_info.rssi_dbm, s_info.apn.c_str());
}

// Polls CEREG until registered or timeout. Aborts early if the modem rebooted (+SYSSTART URC).
bool wait_registered(int timeout_s) {
    const int64_t deadline = esp_log_timestamp() + timeout_s * 1000LL;
    int last = -2;
    while (esp_log_timestamp() < deadline) {
        const int st = cereg_stat();
        refresh_signal();
        if (st != last) {
            ESP_LOGI(TAG, "CEREG stat=%d (1=home, 2=searching, 3=denied, 5=roaming) CSQ=%d", st, info().rssi);
            last = st;
        }
        if (is_registered(st)) return true;
        if (xEventGroupGetBits(s_events) & kModemBoot) {
            ESP_LOGW(TAG, "modem rebooted while waiting for registration");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "no registration within %d s (last CEREG=%d, CSQ=%d)", timeout_s, last, info().rssi);
    return false;
}

// Full radio cycle with APN: CFUN=4 -> CGDCONT -> CFUN=1 -> wait CEREG (sg-sdk espmodem.c:1290-1340).
bool attach(const char* apn) {
    std::string out;
    ESP_LOGI(TAG, "attaching with APN \"%s\"", apn);
    at("AT+CFUN=4", out, 10000);  // radio off before changing PDP (sg-sdk espmodem.c:1309)
    vTaskDelay(pdMS_TO_TICKS(500));  // sg-sdk espmodem.c:1314
    esp_modem::PdpContext pdp(apn);
    if (s_dce->set_pdp_context(pdp) != esp_modem::command_result::OK) {
        ESP_LOGE(TAG, "AT+CGDCONT failed");
        return false;
    }
    if (!at("AT+CFUN=1", out, 10000)) return false;  // sg-sdk espmodem.c:1331
    if (!wait_registered(CONFIG_APP_LTE_ATTACH_TIMEOUT_S)) return false;
    std::lock_guard<std::mutex> l(s_mtx);
    s_info.apn = apn;
    return true;
}

bool attach_any() {
    if (attach(CONFIG_APP_LTE_APN)) return true;
    return strlen(CONFIG_APP_LTE_APN_FALLBACK) > 0 && attach(CONFIG_APP_LTE_APN_FALLBACK);
}

// Bring esp_modem back to COMMAND mode from DATA/CMUX. Returns false if the library could not
// exit cleanly; the caller must then hard-reset the modem. Note DCE_Mode::set_unsafe
// (esp_modem_dce.cpp): a failed CMUX->COMMAND leaves the DCE in CMUX_MODE (so a later
// set_mode(CMUX) is refused) and a failed DATA->COMMAND leaves it UNDEF. In both cases the
// netif has already been stopped, so we force UNDEF and resync after the modem reset.
bool leave_data_mode() {
    const auto m = s_dce->get_mode();
    if (m == esp_modem::modem_mode::COMMAND_MODE) return true;
    if (m == esp_modem::modem_mode::DATA_MODE || m == esp_modem::modem_mode::CMUX_MODE) {
        ESP_LOGI(TAG, "leaving %s mode", mode_name(m));
        if (s_dce->set_mode(esp_modem::modem_mode::COMMAND_MODE)) return true;
        ESP_LOGW(TAG, "set_mode(COMMAND) from %s failed (now %s)", mode_name(m), mode_name(s_dce->get_mode()));
    } else {
        ESP_LOGW(TAG, "esp_modem in %s mode", mode_name(m));
    }
    s_dce->set_mode(esp_modem::modem_mode::UNDEF);
    return false;
}

// Reset (or power-cycle) the modem and resync esp_modem's mode to COMMAND.
bool restart_modem(bool power_cycle) {
    namespace mp = board::modem_power;
    s_dce->set_mode(esp_modem::modem_mode::UNDEF);  // always allowed; drops any stale DATA/CMUX state
    if (power_cycle && mp::is_on()) {
        mp::power_off();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (mp::is_on()) {
        mp::reset_pulse();
    } else {
        mp::power_on();
    }
    // SG waits up to 8 s for +SYSSTART (espmodem.c:687-712); we wait 5 s, then poll with AT.
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Modem is in plain AT mode after reset: tell the DTE/DCE without sending anything.
    s_dce->set_mode(esp_modem::modem_mode::RESUME_COMMAND_MODE);
    if (!sync_modem(15)) {
        ESP_LOGE(TAG, "modem not answering AT");
        return false;
    }
    std::string out;
    at("ATE0", out);
    xEventGroupClearBits(s_events, kModemBoot);  // +SYSSTART from this reset is expected
    return true;
}

// Enter DATA (plain PPP) or CMUX mode and wait for an IP.
bool start_data() {
    xEventGroupClearBits(s_events, kGotIp | kLostIp | kPppError);
    ESP_LOGI(TAG, "entering %s mode", mode_name(kDataMode));
    const bool ok = s_dce->set_mode(kDataMode);
    const auto now = s_dce->get_mode();
    if (!ok) {
        // SG observed set_mode(CMUX) reporting failure although the mux was up; they trust
        // get_mode() (sg-sdk espmodem.c:1402-1419). Do the same, the IP wait below decides.
        if (now != kDataMode) {
            ESP_LOGE(TAG, "set_mode(%s) failed (now %s)", mode_name(kDataMode), mode_name(now));
            return false;
        }
        ESP_LOGW(TAG, "set_mode(%s) returned false but mode is %s, waiting for IP anyway",
                 mode_name(kDataMode), mode_name(now));
    }
    const EventBits_t b =
        xEventGroupWaitBits(s_events, kGotIp | kPppError, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    if (!(b & kGotIp)) {
        ESP_LOGE(TAG, "no PPP IP (%s)", (b & kPppError) ? "PPP error" : "60 s timeout");
        return false;
    }
    return true;
}

// Runs while the link is up. In CMUX mode polls CSQ/CEREG every CONFIG_APP_LTE_HEALTH_PERIOD_S.
LinkEnd monitor() {
    int at_fails = 0, dereg = 0;
    xEventGroupClearBits(s_events, kCheckNow);
    for (;;) {
        const EventBits_t b = xEventGroupWaitBits(s_events, kLostIp | kPppError | kCheckNow, pdTRUE, pdFALSE,
                                                  pdMS_TO_TICKS(CONFIG_APP_LTE_HEALTH_PERIOD_S * 1000));
        if (b & (kLostIp | kPppError)) {
            ESP_LOGW(TAG, "link lost (%s)", (b & kLostIp) ? "PPP lost IP" : "PPP error");
            return LinkEnd::kLinkLost;
        }
        if (!kUseCmux) continue;  // plain PPP: UART is owned by PPP, nothing to poll
        if (b & kCheckNow) ESP_LOGI(TAG, "health check requested after publish failure");

        const bool sig_ok = refresh_signal();
        const int st = cereg_stat();
        if (!sig_ok && st < 0) {
            ESP_LOGW(TAG, "health: modem not answering (%d/%d), term_errors=%u", at_fails + 1,
                     kHealthFailLimit, s_term_errors.load());
            if (++at_fails >= kHealthFailLimit) return LinkEnd::kModemDead;
            continue;
        }
        at_fails = 0;
        if (st >= 0 && !is_registered(st)) {
            ESP_LOGW(TAG, "health: not registered, CEREG stat=%d (%d/%d)", st, dereg + 1, kDeregLimit);
            if (++dereg >= kDeregLimit) return LinkEnd::kDetached;
        } else {
            dereg = 0;
        }
        const Info i = info();
        ESP_LOGI(TAG, "health: CEREG=%d CSQ=%d (%d dBm) RAT=%s ip=%s", i.cereg, i.rssi, i.rssi_dbm, i.rat,
                 i.ip.c_str());
    }
}

void supervisor(void*) {
    Level level = Level::kHard;  // first boot: reset the modem so we start from a known state
    int hard_fails = 0;
    bool was_up = false;
    for (;;) {
        s_connected = false;
        if (!leave_data_mode()) level = Level::kHard;
        ESP_LOGI(TAG, "connect cycle: level=%s", level_name(level));

        if (level == Level::kHard) {
            // Every 3rd consecutive failed hard attempt: power-cycle instead of reset pin.
            const bool cycle = hard_fails > 0 && hard_fails % 3 == 0;
            if (!restart_modem(cycle)) {
                ++hard_fails;
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            read_imei();
        } else if (!sync_modem(5)) {
            level = Level::kHard;
            continue;
        }

        // Soft: modem kept its radio state, just wait for (re)registration.
        const bool registered =
            (level == Level::kSoft) ? wait_registered(kSoftReregTimeoutS) : attach_any();
        if (!registered) {
            if (level == Level::kHard) ++hard_fails;
            level = escalate(level);
            vTaskDelay(pdMS_TO_TICKS(level == Level::kHard ? 30000 : 5000));
            continue;
        }
        read_network_info();

        if (!start_data()) {
            if (level == Level::kHard) ++hard_fails;
            level = escalate(level);
            continue;  // leave_data_mode() at loop top cleans up DATA/CMUX
        }
        hard_fails = 0;
        if (was_up) {
            std::lock_guard<std::mutex> l(s_mtx);
            ++s_info.reconnects;
        }
        was_up = true;
        s_connected = true;
        const LinkEnd end = monitor();
        s_connected = false;
        switch (end) {
            case LinkEnd::kLinkLost: level = Level::kSoft; break;
            case LinkEnd::kDetached: level = Level::kSoft; break;
            case LinkEnd::kModemDead: level = Level::kHard; break;
        }
        ESP_LOGW(TAG, "connection lost, reconnecting (%s)", level_name(level));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
}  // namespace

esp_err_t start() {
    s_events = xEventGroupCreate();
    {
        std::lock_guard<std::mutex> l(s_mtx);
        s_info.cmux = kUseCmux;
    }
    if (esp_err_t e = board::modem_power::init(); e != ESP_OK) return e;
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_status, nullptr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_netif = esp_netif_new(&netif_cfg);

    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num = UART_NUM_1;
    dte_cfg.uart_config.tx_io_num = kUartTx;
    dte_cfg.uart_config.rx_io_num = kUartRx;
    dte_cfg.uart_config.rts_io_num = kUartRts;
    dte_cfg.uart_config.cts_io_num = kUartCts;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_HW;  // sg-sdk espmodem.c:1042
    dte_cfg.uart_config.baud_rate = 115200;  // GM02S factory default (SG switches to 921600, we don't yet)
    dte_cfg.uart_config.rx_buffer_size = 4096;  // sg-sdk espmodem.c:1022-1024
    dte_cfg.uart_config.tx_buffer_size = 2048;
    dte_cfg.uart_config.event_queue_size = 20;
    dte_cfg.dte_buffer_size = 2048;  // sg-sdk espmodem.c:1027-1029 (CMUX frames are decoded in this buffer)
    dte_cfg.task_stack_size = 4096;
    dte_cfg.task_priority = 4;
    s_dte = esp_modem::create_uart_dte(&dte_cfg);
    if (!s_dte) return ESP_FAIL;
    // Terminal errors (UART break/overflow/CRC); DTE already calls CMUX recover() itself.
    // Counted only, reported by the health check (sg-sdk espmodem.c:1118, 2849-2940).
    s_dte->set_error_cb([](esp_modem::terminal_error) { s_term_errors.fetch_add(1); });

    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_APP_LTE_APN);
    s_dce = esp_modem::create_SQNGM02S_dce(&dce_cfg, s_dte, s_netif);
    if (!s_dce) return ESP_FAIL;
#ifdef CONFIG_ESP_MODEM_URC_HANDLER
    // Modem boot/shutdown URCs (sg-sdk espmodem.c:401-435, 1139-1149). Runs in the UART task;
    // the buffer can be re-delivered until the next AT command, so only set an idempotent bit.
    s_dce->set_urc([](uint8_t* data, size_t len) {
        const std::string_view v(reinterpret_cast<const char*>(data), len);
        if (v.find("+SYSSTART") != std::string_view::npos || v.find("+SHUTDOWN") != std::string_view::npos) {
            xEventGroupSetBits(s_events, kModemBoot);
        }
        return esp_modem::command_result::TIMEOUT;
    });
#endif
    ESP_LOGI(TAG, "mode: %s", kUseCmux ? "CMUX (AT + PPP)" : "plain PPP");

    s_pub_q = xQueueCreate(kPubQueueLen, sizeof(PubItem*));
    if (!s_pub_q) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(supervisor, "lte", 6144, nullptr, 5, nullptr, 0);
    xTaskCreatePinnedToCore(publisher_task, "lte_pub", 6144, nullptr, 4, nullptr, 0);
    return ESP_OK;
}

bool is_connected() { return s_connected; }

Info info() {
    std::lock_guard<std::mutex> l(s_mtx);
    return s_info;
}

namespace {
// One HTTP POST attempt. Returns HTTP status or <0 on transport error.
int post_once(const std::string& json, esp_err_t& err) {
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_APP_PUBLISH_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kPubAttemptTimeoutMs;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        err = ESP_ERR_NO_MEM;
        return -3;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json.data(), json.size());
    err = esp_http_client_perform(c);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(c) : -4;
    esp_http_client_cleanup(c);
    return status;
}

bool is_success(int status) { return status >= 200 && status < 300; }

void publisher_task(void*) {
    for (;;) {
        PubItem* it = nullptr;
        if (xQueueReceive(s_pub_q, &it, portMAX_DELAY) != pdTRUE || !it) continue;
        for (unsigned round = 0;; ++round) {
            // must_deliver items wait for the link instead of failing
            while (it->must_deliver && !s_connected) vTaskDelay(pdMS_TO_TICKS(1000));
            const int st = publish(it->json);
            if (is_success(st) || !it->must_deliver || st == -2 /* no URL configured */) break;
            const uint32_t backoff_s = round < 5 ? (10u << round) : 300u;  // 10,20,..,160,300 s
            ESP_LOGW(TAG, "must-deliver event not delivered, retry in %lu s", (unsigned long)backoff_s);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
        }
        delete it;
    }
}
}  // namespace

int publish(const std::string& json) {
    if (!s_connected) return -1;
    if (strlen(CONFIG_APP_PUBLISH_URL) == 0) {
        ESP_LOGW(TAG, "CONFIG_APP_PUBLISH_URL empty, not publishing: %s", json.c_str());
        return -2;
    }
    int status = -1;
    for (int attempt = 1; attempt <= kPubAttempts; ++attempt) {
        if (!s_connected) return -1;
        esp_err_t err = ESP_OK;
        const int64_t t0 = esp_log_timestamp();
        status = post_once(json, err);
        const int ms = int(esp_log_timestamp() - t0);
        if (is_success(status)) {
            ESP_LOGI(TAG, "POST %s -> %d in %d ms (attempt %d)", json.c_str(), status, ms, attempt);
            std::lock_guard<std::mutex> l(s_mtx);
            ++s_info.publish_ok;
            return status;
        }
        // Radio vs server: log the latest CEREG/CSQ and (CMUX) ask the supervisor for a fresh poll.
        const Info i = info();
        ESP_LOGW(TAG, "POST attempt %d/%d -> %d (%s) in %d ms; link: CEREG=%d CSQ=%d (%d dBm) ip=%s%s", attempt,
                 kPubAttempts, status, esp_err_to_name(err), ms, i.cereg, i.rssi, i.rssi_dbm, i.ip.c_str(),
                 kUseCmux ? "" : " (plain PPP: CEREG/CSQ frozen since connect)");
        if (kUseCmux) xEventGroupSetBits(s_events, kCheckNow);
        if (status > 0 && status < 500 && status != 408 && status != 429) break;  // client error: no retry
        if (attempt < kPubAttempts) vTaskDelay(pdMS_TO_TICKS(2000 * attempt));  // 2 s, 4 s
    }
    std::lock_guard<std::mutex> l(s_mtx);
    ++s_info.publish_fail;
    return status;
}

bool publish_async(const std::string& json, bool must_deliver) {
    if (!s_pub_q) return false;
    auto* it = new (std::nothrow) PubItem{json, must_deliver};
    if (!it) return false;
    if (xQueueSend(s_pub_q, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "publish queue full, dropping: %s", json.c_str());
        delete it;
        return false;
    }
    return true;
}

}  // namespace lte
