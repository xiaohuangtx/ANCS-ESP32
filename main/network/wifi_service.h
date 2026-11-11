#ifndef WIFI_SERVICE_H
#define WIFI_SERVICE_H

#include "wifi_store.h"
#include "config_portal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi_types.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

enum class NetworkEvent {
    Scanning,              // WiFi scanning
    Connecting,            // WiFi connecting (data: SSID)
    Connected,             // WiFi connected (data: SSID)
    Disconnected,          // WiFi disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode (data: hotspot hint)
    WifiConfigModeExit,    // Exited WiFi configuration mode
};

using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

// A saved network as shown on the admin page; passwords are never handed out
struct SavedNetwork {
    std::string ssid;
    bool has_password = false;
    uint8_t channel = 0;  // 0 = unknown
};

// A network found by the scan of the provisioning hotspot
struct ScannedNetwork {
    std::string ssid;
    int rssi = 0;
    bool secure = false;
};

/**
 * Station connection + hotspot provisioning.
 *
 * The station scans and connects to the strongest saved network in range,
 * retrying it a few times before trying the next one, and rescans with a
 * growing interval (10 s up to 5 min) while none can be reached.
 *
 * If no network is saved, or the first connection does not succeed within
 * 60 s, the device opens an open hotspot named after the device. Joining it
 * pops up the setup page (ConfigPortal, http://192.168.4.1).
 *
 * A hotspot opened by that timeout closes again by itself once a saved
 * network shows up in its scans while nobody is on the hotspot, e.g. when the
 * router comes up after the device. If that network still cannot be
 * connected, the next hotspot stays open for at least 1 min, then 2, 4 ... up
 * to 10 min before closing by itself. A hotspot opened on request
 * (EnterWifiConfigMode) stays open until it is left from the setup page.
 *
 * All Wi-Fi state changes run in one task: driver events, timers and the
 * public methods only post messages to it. The network event callback is
 * invoked from that task.
 */
class WifiService {
public:
    static WifiService& GetInstance() {
        static WifiService instance;
        return instance;
    }
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    // device_name: hostname of the station and name of the provisioning hotspot
    void Start(const std::string& device_name);
    // Must be set before Start()
    void SetNetworkEventCallback(NetworkEventCallback callback);
    // Stop the station and open the provisioning hotspot. Asynchronous.
    void EnterWifiConfigMode();
    // Close the hotspot and connect to the saved networks (the hotspot opens
    // again if there are none). Asynchronous, after delay_ms.
    void ExitWifiConfigMode(int delay_ms = 0);

    bool IsConnected() const;
    bool IsInWifiConfigMode() const;
    std::string GetIpAddress() const;
    std::string GetSsid() const;
    int GetRssi() const;
    const std::string& GetDeviceName() const { return device_name_; }

    // Provisioning page (config mode only)
    static bool Supports5G();
    // Strongest first, one entry per SSID; refreshed every 10 s
    std::vector<ScannedNetwork> GetScanResults() const;
    // Checks the credentials by connecting (blocks for up to ~35 s, ~55 s on
    // 5 GHz capable chips), then saves the network, or its new password.
    // Returns an error message, or "" and the IP address the router handed out.
    std::string ProvisionNetwork(const std::string& ssid, const std::string& password, std::string& ip);

    // Saved networks. The station picks the strongest one found by a scan, so
    // the order does not matter. Changes apply on the next (re)connect; the
    // current connection is kept. The setters return an error message, or ""
    // on success.
    static constexpr int kMaxSavedNetworks = WifiStore::kMaxNetworks;
    std::vector<SavedNetwork> GetSavedNetworks() const;
    std::string AddNetwork(const std::string& ssid, const std::string& password);
    // password == nullptr keeps the current one
    std::string UpdateNetwork(const std::string& ssid, const std::string& new_ssid, const std::string* password);
    std::string RemoveNetwork(const std::string& ssid);

private:
    enum class Mode { Off, Station, Config };

    enum class MessageType : uint8_t {
        Begin,           // Start(): connect, or open the hotspot
        EnterConfig,
        ExitConfig,
        ConnectTimeout,  // The first connection took too long
        ScanTimer,
        StaStart,
        ScanDone,        // value: status, 0 = success
        Disconnected,    // value: reason
        GotIp,           // value: IPv4 address
    };

    struct Message {
        MessageType type;
        uint32_t value;
    };

    WifiService();
    ~WifiService() = default;

    void Post(MessageType type, uint32_t value = 0);
    void Run();
    void HandleMessage(const Message& message);
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    // Station
    void Connect();
    void StartStation();
    void StopStation();
    void StartStationScan();
    void ScheduleStationScan();
    void OnStationScanDone();
    void ConnectNext();
    void OnStationDisconnected(int reason);
    void OnStationGotIp(uint32_t ip);
    void SetState(bool connected, const std::string& ssid, const std::string& ip);

    // Config mode
    // auto_exit: leave again by itself once a saved network can be seen
    void StartConfigMode(bool auto_exit = false);
    void StopConfigMode();
    void StartConfigScan();
    void OnConfigScanDone(bool success);
    bool ShouldAutoExit(const std::vector<wifi_ap_record_t>& records) const;
    std::string TestConnect(const std::string& ssid, const std::string& password,
                            std::string& ip, uint8_t& channel);

    static void EventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    std::string device_name_;
    NetworkEventCallback network_event_callback_;
    WifiStore store_;
    ConfigPortal portal_;

    QueueHandle_t queue_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    esp_timer_handle_t scan_timer_ = nullptr;
    esp_timer_handle_t connect_timer_ = nullptr;
    esp_timer_handle_t exit_timer_ = nullptr;
    std::atomic<Mode> mode_{Mode::Off};

    // Station state, only used by the Wi-Fi task
    std::vector<WifiCredential> candidates_;  // Saved networks in range, strongest first
    WifiCredential current_;
    bool scanning_ = false;
    bool connecting_ = false;
    int retries_ = 0;
    int scan_interval_sec_ = 0;

    // Config mode state, only used by the Wi-Fi task
    bool auto_exit_ = false;
    int64_t config_start_us_ = 0;
    // The station was started by an auto exit and has not connected yet
    bool auto_exited_ = false;
    // Auto exits in a row that did not lead to a connection
    int auto_exit_failures_ = 0;

    // Read by the getters
    mutable std::mutex mutex_;
    bool connected_ = false;
    std::string ssid_;
    std::string ip_;
    std::vector<ScannedNetwork> scan_results_;

    // Credential test from the provisioning page
    std::mutex test_mutex_;
    std::atomic<bool> testing_{false};
    EventGroupHandle_t test_events_ = nullptr;
    std::atomic<uint32_t> test_ip_{0};
    std::atomic<int> test_reason_{0};
};

#endif // WIFI_SERVICE_H
