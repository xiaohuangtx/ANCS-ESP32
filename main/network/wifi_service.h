#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <esp_timer.h>

#include <string>
#include <vector>
#include <functional>

enum class NetworkEvent {
    Scanning,              // WiFi scanning
    Connecting,            // WiFi connecting (data: SSID)
    Connected,             // WiFi connected (data: SSID)
    Disconnected,          // WiFi disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode (data: hotspot hint)
    WifiConfigModeExit,    // Exited WiFi configuration mode
};

using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

// A saved Wi-Fi network (NVS namespace "wifi")
struct SavedNetwork {
    std::string ssid;
    bool has_password = false;
    uint8_t channel = 0;  // 0 = unknown
};

/**
 * Station connection + hotspot provisioning.
 *
 * If no SSID is stored, or connecting times out, the device opens a hotspot
 * "<prefix>-XXXXXX". Connect to it and open http://192.168.4.1 to configure.
 */
class WifiService {
public:
    static WifiService& GetInstance() {
        static WifiService instance;
        return instance;
    }
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    // ap_ssid: name of the provisioning hotspot
    void Start(const std::string& ap_ssid);
    void SetNetworkEventCallback(NetworkEventCallback callback);
    // Stop the station and open the provisioning hotspot
    void EnterWifiConfigMode();

    bool IsConnected() const;
    bool IsInWifiConfigMode() const;
    std::string GetIpAddress() const;
    std::string GetSsid() const;
    int GetRssi() const;

    // Saved networks. The station picks the strongest one found by a scan, so
    // the order does not matter. Changes apply on the next (re)connect; the
    // current connection is kept. The setters return an error message, or ""
    // on success.
    static constexpr int kMaxSavedNetworks = 10;
    std::vector<SavedNetwork> GetSavedNetworks() const;
    std::string AddNetwork(const std::string& ssid, const std::string& password);
    // password == nullptr keeps the current one
    std::string UpdateNetwork(const std::string& ssid, const std::string& new_ssid, const std::string* password);
    std::string RemoveNetwork(const std::string& ssid);

private:
    WifiService();
    ~WifiService();

    void TryWifiConnect();
    void StartWifiConfigMode();
    void ApplyApSsid();
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    static void OnWifiConnectTimeout(void* arg);

    esp_timer_handle_t connect_timer_ = nullptr;
    std::string ap_ssid_;
    NetworkEventCallback network_event_callback_;
};

#endif // WIFI_SERVICE_H
