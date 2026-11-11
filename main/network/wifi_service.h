#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include <esp_timer.h>

#include <string>
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

/**
 * Station connection + hotspot provisioning.
 *
 * If no SSID is stored, or connecting times out, the device opens a hotspot
 * "<prefix>-XXXX". Connect to it and open http://192.168.4.1 to configure.
 */
class WifiService {
public:
    static WifiService& GetInstance() {
        static WifiService instance;
        return instance;
    }
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    void Start();
    void SetNetworkEventCallback(NetworkEventCallback callback);
    // Stop the station and open the provisioning hotspot
    void EnterWifiConfigMode();

    bool IsConnected() const;
    bool IsInWifiConfigMode() const;
    std::string GetIpAddress() const;

private:
    WifiService();
    ~WifiService();

    void TryWifiConnect();
    void StartWifiConfigMode();
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    static void OnWifiConnectTimeout(void* arg);

    esp_timer_handle_t connect_timer_ = nullptr;
    NetworkEventCallback network_event_callback_;
};

#endif // WIFI_SERVICE_H
