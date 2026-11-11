#include "wifi_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
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

void WifiService::Start() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = CONFIG_WIFI_AP_SSID_PREFIX;
    config.language = "zh-CN";
    wifi_manager.Initialize(config);

    // Forward WiFi events to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this, &wifi_manager](WifiEvent event) {
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
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter,
                               wifi_manager.GetApSsid() + " " + wifi_manager.GetApWebUrl());
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
