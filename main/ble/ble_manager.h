#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <host/ble_hs.h>
#include <nimble/nimble_npl.h>

#include <string>
#include <array>
#include <vector>
#include <atomic>
#include <functional>

#include "ancs_client.h"

/**
 * NimBLE host wrapper.
 *
 * The ESP32 advertises as a peripheral and solicits the ANCS service, so the
 * iPhone lists it in Settings > Bluetooth. After the iPhone connects, the link
 * is bonded ("Just Works") and encrypted, then the AncsClient takes over.
 *
 * Up to GetMaxConnections() devices (iPhone / iPad) can be connected at the
 * same time, each with its own AncsClient. Advertising continues while a
 * connection slot is free. The limit is read from NVS (namespace "ble", key
 * "max_conn"), falling back to CONFIG_BLE_MAX_CONNECTIONS, and can never
 * exceed kMaxConnections (CONFIG_BT_NIMBLE_MAX_CONNECTIONS, fixed at build time).
 */
class BleManager {
public:
    using ConnectionCallback = std::function<void(uint16_t conn_handle, bool connected, int connection_count)>;
    // Number of connection slots compiled into NimBLE / the controller
    static constexpr int kMaxConnections = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;

    static BleManager& GetInstance() {
        static BleManager instance;
        return instance;
    }
    BleManager(const BleManager&) = delete;
    BleManager& operator=(const BleManager&) = delete;

    void Initialize(const std::string& device_name);

    // Advertised name, e.g. "ANCS-A1B2C3". Set by Initialize(), read-only afterwards.
    const std::string& GetDeviceName() const { return device_name_; }
    int GetConnectionCount() const;
    // Thread-safe: the connected devices
    std::vector<AncsClient::PeerInfo> GetPeers() const;
    int GetMaxConnections() const { return max_connections_; }
    // Thread-safe, clamped to [1, kMaxConnections], persisted to NVS. Lowering
    // the limit does not drop connected devices, it only stops advertising.
    void SetMaxConnections(int max_connections);
    // Both callbacks are invoked from the NimBLE host task. Set them before Initialize().
    void SetConnectionCallback(ConnectionCallback callback) { connection_callback_ = std::move(callback); }
    void SetNotificationCallback(AncsClient::NotificationCallback callback);

    // Thread-safe: trigger a notification action on the device identified by
    // AncsNotification::conn_handle. Returns false if that device is gone.
    bool PerformAction(uint16_t conn_handle, uint32_t uid, AncsActionId action);

    // Thread-safe: erase all bonds and drop all connections
    void ClearBonds();

private:
    BleManager() = default;

    void StartAdvertising();
    void ScheduleAdvertising();
    int HandleGapEvent(struct ble_gap_event* event);
    void PrintConnection(uint16_t conn_handle);

    // One connection slot: the connection handle and its ANCS client
    struct Link {
        std::atomic<uint16_t> conn_handle = BLE_HS_CONN_HANDLE_NONE;
        AncsClient ancs;
    };
    Link* FindLink(uint16_t conn_handle);
    Link* AllocateLink(uint16_t conn_handle);

    static int GapEventHandler(struct ble_gap_event* event, void* arg);
    static void OnSync();
    static void OnReset(int reason);
    static void HostTask(void* param);
    static void OnClearBondsEvent(struct ble_npl_event* ev);
    static void OnAdvertiseEvent(struct ble_npl_event* ev);
    static void OnAdvertiseRetry(struct ble_npl_event* ev);

    std::array<Link, kMaxConnections> links_;
    std::atomic<int> max_connections_ = kMaxConnections;
    std::atomic<bool> initialized_ = false;
    ConnectionCallback connection_callback_;
    std::string device_name_;
    uint8_t own_addr_type_ = 0;
    struct ble_npl_event clear_bonds_event_;
    struct ble_npl_event advertise_event_;
    struct ble_npl_callout advertise_retry_timer_;
};

#endif // BLE_MANAGER_H
