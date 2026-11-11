#include "wifi_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cstring>
#include <utility>

#include <wifi_manager.h>
#include <ssid_manager.h>

static const char *TAG = "WifiService";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiService::WifiService() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiService::~WifiService() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

void WifiService::Start(const std::string& ap_ssid) {
    ap_ssid_ = ap_ssid;
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = CONFIG_WIFI_AP_SSID_PREFIX;
    config.language = "zh-CN";
    wifi_manager.Initialize(config);

    // Forward WiFi events to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this, &wifi_manager](WifiEvent event, const std::string&) {
        std::string ssid = wifi_manager.GetSsid();
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, ssid);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, ssid);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                ApplyApSsid();
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter,
                               ap_ssid_ + " " + wifi_manager.GetApWebUrl());
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiService::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        StartWifiConfigMode();
    }
}

void WifiService::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
            ESP_LOGI(TAG, "Connected to WiFi: %s, IP: %s", data.c_str(), GetIpAddress().c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGW(TAG, "WiFi config mode: connect to hotspot %s to configure WiFi", data.c_str());
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiService::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiService::OnWifiConnectTimeout(void* arg) {
    auto* service = static_cast<WifiService*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    service->StartWifiConfigMode();
}

void WifiService::StartWifiConfigMode() {
    WifiManager::GetInstance().StartConfigAp();
}

// The Wi-Fi component always names the hotspot "<prefix>-XXXX" (last 2 bytes of
// the softAP MAC), so rename it right after the AP has been started
void WifiService::ApplyApSsid() {
    wifi_config_t wifi_config = {};
    esp_wifi_get_config(WIFI_IF_AP, &wifi_config);
    size_t len = std::min(ap_ssid_.size(), sizeof(wifi_config.ap.ssid));
    memset(wifi_config.ap.ssid, 0, sizeof(wifi_config.ap.ssid));
    memcpy(wifi_config.ap.ssid, ap_ssid_.data(), len);
    wifi_config.ap.ssid_len = len;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set hotspot SSID, err=%s", esp_err_to_name(err));
    }
}

void WifiService::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    if (IsInWifiConfigMode()) {
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiService::IsConnected() const {
    return WifiManager::GetInstance().IsConnected();
}

bool WifiService::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

std::string WifiService::GetIpAddress() const {
    return WifiManager::GetInstance().GetIpAddress();
}
