#include "wifi_service.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstring>
#include <utility>

static const char* TAG = "WifiService";

// For the first connection after boot / leaving config mode
static constexpr int kConnectTimeoutSec = 60;
// The station rescans while no saved network can be reached, doubling the wait
static constexpr int kMinScanIntervalSec = 10;
static constexpr int kMaxScanIntervalSec = 300;
// Reconnects to the same network before trying the next one
static constexpr int kMaxRetries = 5;
// Refreshes the network list of the setup page
static constexpr int kConfigScanIntervalSec = 10;
static constexpr int kQueueSize = 16;
static constexpr int kTaskStackSize = 4096;

// Credential test from the setup page
static constexpr int kTestAttempts = 2;
static constexpr int kTestRetryDelayMs = 3000;
#if CONFIG_SOC_WIFI_SUPPORT_5G
static constexpr int kTestTimeoutMs = 25000;  // Scanning both bands takes longer
#else
static constexpr int kTestTimeoutMs = 15000;
#endif
static constexpr EventBits_t kTestConnected = BIT0;
static constexpr EventBits_t kTestFailed = BIT1;

static std::string IpToString(uint32_t ip) {
    esp_ip4_addr_t addr = {.addr = ip};
    char str[16];
    esp_ip4addr_ntoa(&addr, str, sizeof(str));
    return str;
}

static void SetStaCredential(wifi_config_t& config, const std::string& ssid, const std::string& password) {
    memcpy(config.sta.ssid, ssid.data(), std::min(ssid.size(), sizeof(config.sta.ssid)));
    memcpy(config.sta.password, password.data(), std::min(password.size(), sizeof(config.sta.password)));
    // Some WPA3 routers only accept hash-to-element
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

// Strongest first. Also frees the driver's copy of the results.
static std::vector<wifi_ap_record_t> FetchScanRecords() {
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    std::vector<wifi_ap_record_t> records(count);
    if (count == 0 || esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        count = 0;
    }
    records.resize(count);
    std::sort(records.begin(), records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;
    });
    return records;
}

static std::string DescribeTestFailure(int reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return WifiService::Supports5G() ? "找不到该 Wi-Fi" : "找不到该 Wi-Fi（本设备只支持 2.4 GHz）";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "加密方式不匹配，请确认该 Wi-Fi 是否需要密码";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "连接失败，请检查密码";
        default:
            return "连接失败（原因 " + std::to_string(reason) + "）";
    }
}

WifiService::WifiService() {
    test_events_ = xEventGroupCreate();
}

void WifiService::Start(const std::string& device_name) {
    device_name_ = device_name;
    queue_ = xQueueCreate(kQueueSize, sizeof(Message));

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    sta_netif_ = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif_, device_name_.c_str());

    ap_netif_ = esp_netif_create_default_wifi_ap();
    esp_netif_ip_info_t ip_info = {};
    esp_netif_str_to_ip4(ConfigPortal::kAddress, &ip_info.ip);
    ip_info.gw = ip_info.ip;
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    // Credentials are kept in WifiStore, not by the driver
    config.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, this, nullptr));

    esp_timer_create_args_t timer_args = {};
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "wifi_scan";
    timer_args.callback = [](void* arg) {
        static_cast<WifiService*>(arg)->Post(MessageType::ScanTimer);
    };
    esp_timer_create(&timer_args, &scan_timer_);
    timer_args.name = "wifi_connect";
    timer_args.callback = [](void* arg) {
        static_cast<WifiService*>(arg)->Post(MessageType::ConnectTimeout);
    };
    esp_timer_create(&timer_args, &connect_timer_);
    timer_args.name = "wifi_exit_config";
    timer_args.callback = [](void* arg) {
        static_cast<WifiService*>(arg)->Post(MessageType::ExitConfig);
    };
    esp_timer_create(&timer_args, &exit_timer_);

    xTaskCreate([](void* arg) {
        static_cast<WifiService*>(arg)->Run();
    }, "wifi", kTaskStackSize, this, 5, nullptr);
    Post(MessageType::Begin);
}

void WifiService::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiService::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "Entering config mode");
    Post(MessageType::EnterConfig);
}

void WifiService::ExitWifiConfigMode(int delay_ms) {
    if (delay_ms <= 0) {
        Post(MessageType::ExitConfig);
        return;
    }
    esp_timer_stop(exit_timer_);
    esp_timer_start_once(exit_timer_, delay_ms * 1000ULL);
}

void WifiService::Post(MessageType type, uint32_t value) {
    if (queue_ == nullptr) {
        return;
    }
    Message message = {type, value};
    if (xQueueSend(queue_, &message, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Queue full, message %d dropped", (int)type);
    }
}

void WifiService::Run() {
    Message message;
    while (true) {
        if (xQueueReceive(queue_, &message, portMAX_DELAY) == pdTRUE) {
            HandleMessage(message);
        }
    }
}

// Runs in the event loop task: hand everything over to the Wi-Fi task
void WifiService::EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiService*>(arg);
    if (base == IP_EVENT) {
        self->Post(MessageType::GotIp, static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr);
        return;
    }
    switch (id) {
        case WIFI_EVENT_STA_START:
            self->Post(MessageType::StaStart);
            break;
        case WIFI_EVENT_SCAN_DONE:
            self->Post(MessageType::ScanDone, static_cast<wifi_event_sta_scan_done_t*>(data)->status);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            self->Post(MessageType::Disconnected, static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Client " MACSTR " joined the hotspot",
                     MAC2STR(static_cast<wifi_event_ap_staconnected_t*>(data)->mac));
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Client " MACSTR " left the hotspot",
                     MAC2STR(static_cast<wifi_event_ap_stadisconnected_t*>(data)->mac));
            break;
        default:
            break;
    }
}

// Driver events are interpreted according to the current mode. Events left
// over from the previous mode are harmless: the station ignores them unless
// it is scanning / connecting, and the credential test clears them first.
void WifiService::HandleMessage(const Message& message) {
    Mode mode = mode_;
    switch (message.type) {
        case MessageType::Begin:
            Connect();
            break;
        case MessageType::EnterConfig:
            StartConfigMode();
            break;
        case MessageType::ExitConfig:
            StopConfigMode();
            break;
        case MessageType::ConnectTimeout:
            if (mode == Mode::Station && !IsConnected()) {
                ESP_LOGW(TAG, "Not connected after %d s, opening the hotspot", kConnectTimeoutSec);
                StartConfigMode();
            }
            break;
        case MessageType::StaStart:
        case MessageType::ScanTimer:
            if (mode == Mode::Station) {
                StartStationScan();
            } else if (mode == Mode::Config) {
                StartConfigScan();
            }
            break;
        case MessageType::ScanDone:
            if (mode == Mode::Station) {
                OnStationScanDone();
            } else if (mode == Mode::Config) {
                OnConfigScanDone(message.value == 0);
            } else {
                esp_wifi_clear_ap_list();
            }
            break;
        case MessageType::Disconnected:
            if (mode == Mode::Station) {
                OnStationDisconnected(message.value);
            } else if (mode == Mode::Config) {
                test_reason_ = message.value;
                xEventGroupSetBits(test_events_, kTestFailed);
            }
            break;
        case MessageType::GotIp:
            if (mode == Mode::Station) {
                OnStationGotIp(message.value);
            } else if (mode == Mode::Config) {
                test_ip_ = message.value;
                xEventGroupSetBits(test_events_, kTestConnected);
            }
            break;
    }
}

void WifiService::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiService::SetState(bool connected, const std::string& ssid, const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = connected;
    ssid_ = ssid;
    ip_ = ip;
}

// ==================== Station ====================

// Connect to the saved networks, or open the hotspot if there are none
void WifiService::Connect() {
    if (store_.IsEmpty()) {
        ESP_LOGI(TAG, "No saved network");
        StartConfigMode();
    } else {
        StartStation();
    }
}

void WifiService::StartStation() {
    ESP_LOGI(TAG, "Starting station");
    mode_ = Mode::Station;
    candidates_.clear();
    scanning_ = false;
    connecting_ = false;
    scan_interval_sec_ = kMinScanIntervalSec;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // WIFI_EVENT_STA_START starts the first scan
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    esp_timer_stop(connect_timer_);
    esp_timer_start_once(connect_timer_, kConnectTimeoutSec * 1000000ULL);
}

void WifiService::StopStation() {
    esp_timer_stop(scan_timer_);
    esp_timer_stop(connect_timer_);
    mode_ = Mode::Off;
    candidates_.clear();
    scanning_ = false;
    connecting_ = false;
    esp_wifi_stop();
    SetState(false, "", "");
    ESP_LOGI(TAG, "Station stopped");
}

void WifiService::StartStationScan() {
    if (scanning_ || connecting_ || IsConnected()) {
        return;
    }
    esp_err_t err = esp_wifi_scan_start(nullptr, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start scan: %s", esp_err_to_name(err));
        ScheduleStationScan();
        return;
    }
    scanning_ = true;
    OnNetworkEvent(NetworkEvent::Scanning);
}

void WifiService::ScheduleStationScan() {
    ESP_LOGI(TAG, "Next scan in %d s", scan_interval_sec_);
    esp_timer_stop(scan_timer_);
    esp_timer_start_once(scan_timer_, scan_interval_sec_ * 1000000ULL);
    scan_interval_sec_ = std::min(scan_interval_sec_ * 2, kMaxScanIntervalSec);
}

void WifiService::OnStationScanDone() {
    auto records = FetchScanRecords();
    if (!scanning_) {
        return;
    }
    scanning_ = false;

    // One candidate per saved network in range, strongest first
    candidates_.clear();
    auto saved = store_.GetAll();
    for (auto& record : records) {
        std::string ssid(reinterpret_cast<const char*>(record.ssid));
        auto it = std::find_if(saved.begin(), saved.end(), [&](const WifiCredential& network) {
            return network.ssid == ssid;
        });
        if (it == saved.end()) {
            continue;
        }
        ESP_LOGI(TAG, "Found %s, RSSI %d, channel %d", ssid.c_str(), record.rssi, record.primary);
        candidates_.push_back({it->ssid, it->password, record.primary});
        saved.erase(it);
    }

    if (candidates_.empty()) {
        ESP_LOGI(TAG, "No saved network in range (%u networks found)", (unsigned)records.size());
        ScheduleStationScan();
        return;
    }
    ConnectNext();
}

void WifiService::ConnectNext() {
    current_ = candidates_.front();
    candidates_.erase(candidates_.begin());
    connecting_ = true;
    retries_ = 0;
    SetState(false, current_.ssid, "");
    ESP_LOGI(TAG, "Connecting to %s", current_.ssid.c_str());
    OnNetworkEvent(NetworkEvent::Connecting, current_.ssid);

    wifi_config_t config = {};
    SetStaCredential(config, current_.ssid, current_.password);
    // The scan has just found it there
    config.sta.channel = current_.channel;
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to %s: %s", current_.ssid.c_str(), esp_err_to_name(err));
        connecting_ = false;
        ScheduleStationScan();
    }
}

void WifiService::OnStationDisconnected(int reason) {
    if (IsConnected()) {
        ESP_LOGW(TAG, "Disconnected from %s, reason %d", current_.ssid.c_str(), reason);
        SetState(false, current_.ssid, "");
        OnNetworkEvent(NetworkEvent::Disconnected);
        connecting_ = true;
        retries_ = 0;
        esp_wifi_connect();
        return;
    }
    if (!connecting_) {
        return;
    }

    if (retries_ < kMaxRetries) {
        retries_++;
        ESP_LOGW(TAG, "Failed to connect to %s, reason %d, retrying (%d/%d)", current_.ssid.c_str(), reason,
                 retries_, kMaxRetries);
        esp_wifi_connect();
        return;
    }
    ESP_LOGW(TAG, "Giving up on %s, reason %d", current_.ssid.c_str(), reason);
    if (!candidates_.empty()) {
        ConnectNext();
        return;
    }
    connecting_ = false;
    SetState(false, "", "");
    ScheduleStationScan();
}

void WifiService::OnStationGotIp(uint32_t ip) {
    bool was_connected = IsConnected();
    std::string ip_str = IpToString(ip);
    SetState(true, current_.ssid, ip_str);
    if (was_connected) {
        ESP_LOGI(TAG, "IP address changed to %s", ip_str.c_str());
        return;
    }

    esp_timer_stop(connect_timer_);
    connecting_ = false;
    retries_ = 0;
    candidates_.clear();
    scan_interval_sec_ = kMinScanIntervalSec;

    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        store_.SetChannel(current_.ssid, info.primary);
    }
    ESP_LOGI(TAG, "Connected to %s, IP %s", current_.ssid.c_str(), ip_str.c_str());
    OnNetworkEvent(NetworkEvent::Connected, current_.ssid);
}

// ==================== Config mode ====================

void WifiService::StartConfigMode() {
    if (mode_ == Mode::Config) {
        return;
    }
    if (mode_ == Mode::Station) {
        StopStation();
        // Lets the admin page release port 80 before the portal takes it
        OnNetworkEvent(NetworkEvent::Disconnected);
    }

    mode_ = Mode::Config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_results_.clear();
    }

    wifi_config_t config = {};
    size_t length = std::min(device_name_.size(), sizeof(config.ap.ssid));
    memcpy(config.ap.ssid, device_name_.data(), length);
    config.ap.ssid_len = length;
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    // WIFI_EVENT_STA_START starts the first scan
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif_, &ip_info);
    portal_.Start(ip_info.ip.addr);

    ESP_LOGW(TAG, "Config mode: join hotspot %s and open http://%s/", device_name_.c_str(), ConfigPortal::kAddress);
    OnNetworkEvent(NetworkEvent::WifiConfigModeEnter, device_name_ + " http://" + ConfigPortal::kAddress);
}

void WifiService::StopConfigMode() {
    if (mode_ != Mode::Config) {
        return;
    }
    ESP_LOGI(TAG, "Leaving config mode");
    esp_timer_stop(scan_timer_);
    portal_.Stop();
    mode_ = Mode::Off;
    esp_wifi_stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scan_results_.clear();
    }
    OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
    Connect();
}

void WifiService::StartConfigScan() {
    // While a credential test runs, try again later
    if (testing_ || esp_wifi_scan_start(nullptr, false) != ESP_OK) {
        esp_timer_stop(scan_timer_);
        esp_timer_start_once(scan_timer_, kConfigScanIntervalSec * 1000000ULL);
    }
}

void WifiService::OnConfigScanDone(bool success) {
    auto records = FetchScanRecords();
    // A scan cut short by the credential test keeps the previous list
    if (success && !testing_) {
        std::vector<ScannedNetwork> networks;
        for (auto& record : records) {
            std::string ssid(reinterpret_cast<const char*>(record.ssid));
            if (ssid.empty()) {
                continue;  // Hidden network
            }
            bool listed = std::any_of(networks.begin(), networks.end(), [&](const ScannedNetwork& network) {
                return network.ssid == ssid;
            });
            if (listed) {
                continue;  // Another, weaker AP of the same network
            }
            bool secure = record.authmode != WIFI_AUTH_OPEN && record.authmode != WIFI_AUTH_OWE;
            networks.push_back({ssid, record.rssi, secure});
        }
        std::lock_guard<std::mutex> lock(mutex_);
        scan_results_ = std::move(networks);
    }
    esp_timer_stop(scan_timer_);
    esp_timer_start_once(scan_timer_, kConfigScanIntervalSec * 1000000ULL);
}

// Runs in the HTTP server task of the portal. The station is otherwise idle
// in config mode, so it can be driven from here; the Wi-Fi task only reports
// the outcome through test_events_.
std::string WifiService::TestConnect(const std::string& ssid, const std::string& password,
                                     std::string& ip, uint8_t& channel) {
    std::lock_guard<std::mutex> lock(test_mutex_);
    if (mode_ != Mode::Config) {
        return "设备已退出配网模式";
    }
    testing_ = true;
    esp_wifi_scan_stop();

    wifi_config_t config = {};
    SetStaCredential(config, ssid, password);
    // Channel unknown: scan them all and pick the strongest AP of the network
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.failure_retry_cnt = 1;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        testing_ = false;
        return std::string("设置 Wi-Fi 失败: ") + esp_err_to_name(err);
    }

    bool connected = false;
    bool timed_out = false;
    for (int attempt = 1; attempt <= kTestAttempts && !connected; attempt++) {
        if (attempt > 1) {
            // The hotspot has just followed the router to its channel, and the
            // router often refuses the first association with "try again later"
            // (status 30), which the driver's quick retries do not get past
            vTaskDelay(pdMS_TO_TICKS(kTestRetryDelayMs));
        }
        xEventGroupClearBits(test_events_, kTestConnected | kTestFailed);
        ESP_LOGI(TAG, "Testing %s (attempt %d/%d)", ssid.c_str(), attempt, kTestAttempts);
        if (esp_wifi_connect() != ESP_OK) {
            continue;
        }
        EventBits_t bits = xEventGroupWaitBits(test_events_, kTestConnected | kTestFailed, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(kTestTimeoutMs));
        connected = (bits & kTestConnected) != 0;
        timed_out = bits == 0;
        if (timed_out) {
            // Still connecting: cancel, or the next esp_wifi_connect() fails
            esp_wifi_disconnect();
        }
    }

    std::string error;
    if (connected) {
        wifi_ap_record_t info;
        channel = esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.primary : 0;
        ip = IpToString(test_ip_);
        ESP_LOGI(TAG, "Test of %s passed, channel %u, IP %s", ssid.c_str(), channel, ip.c_str());
        esp_wifi_disconnect();
    } else {
        error = timed_out ? "连接超时，请确认 Wi-Fi 信号足够强" : DescribeTestFailure(test_reason_);
        ESP_LOGW(TAG, "Test of %s failed: %s", ssid.c_str(), error.c_str());
    }
    testing_ = false;
    // The scan stopped above may not report back; keep the list refreshing
    esp_timer_stop(scan_timer_);
    esp_timer_start_once(scan_timer_, kConfigScanIntervalSec * 1000000ULL);
    return error;
}

std::string WifiService::ProvisionNetwork(const std::string& ssid, const std::string& password, std::string& ip) {
    auto error = store_.CheckPut(ssid, password);
    if (!error.empty()) {
        return error;
    }
    uint8_t channel = 0;
    error = TestConnect(ssid, password, ip, channel);
    if (!error.empty()) {
        return error;
    }
    return store_.Put(ssid, password, channel);
}

bool WifiService::Supports5G() {
#if CONFIG_SOC_WIFI_SUPPORT_5G
    return true;
#else
    return false;
#endif
}

std::vector<ScannedNetwork> WifiService::GetScanResults() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scan_results_;
}

// ==================== Getters / saved networks ====================

bool WifiService::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool WifiService::IsInWifiConfigMode() const {
    return mode_ == Mode::Config;
}

std::string WifiService::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ip_;
}

std::string WifiService::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssid_;
}

int WifiService::GetRssi() const {
    if (!IsConnected()) {
        return 0;
    }
    wifi_ap_record_t info;
    return esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.rssi : 0;
}

std::vector<SavedNetwork> WifiService::GetSavedNetworks() const {
    std::vector<SavedNetwork> networks;
    for (auto& network : store_.GetAll()) {
        networks.push_back({network.ssid, !network.password.empty(), network.channel});
    }
    return networks;
}

std::string WifiService::AddNetwork(const std::string& ssid, const std::string& password) {
    return store_.Add(ssid, password);
}

std::string WifiService::UpdateNetwork(const std::string& ssid, const std::string& new_ssid,
                                       const std::string* password) {
    return store_.Update(ssid, new_ssid, password);
}

std::string WifiService::RemoveNetwork(const std::string& ssid) {
    return store_.Remove(ssid);
}
