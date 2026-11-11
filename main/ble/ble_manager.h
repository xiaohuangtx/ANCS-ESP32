#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include <host/ble_hs.h>
#include <nimble/nimble_npl.h>

#include <string>
#include <atomic>
#include <functional>

#include "ancs_client.h"

/**
 * NimBLE host wrapper.
 *
 * The ESP32 advertises as a peripheral and solicits the ANCS service, so the
 * iPhone lists it in Settings > Bluetooth. After the iPhone connects, the link
 * is bonded ("Just Works") and encrypted, then the AncsClient takes over.
 */
class BleManager {
public:
    using ConnectionCallback = std::function<void(bool connected)>;

    static BleManager& GetInstance() {
        static BleManager instance;
        return instance;
    }
    BleManager(const BleManager&) = delete;
    BleManager& operator=(const BleManager&) = delete;

    void Initialize(const std::string& device_name);
    AncsClient& GetAncsClient() { return ancs_client_; }

    bool IsConnected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    // Invoked from the NimBLE host task
    void SetConnectionCallback(ConnectionCallback callback) { connection_callback_ = std::move(callback); }

    // Thread-safe: erase all bonds and drop the current connection
    void ClearBonds();

private:
    BleManager() = default;

    void StartAdvertising();
    int HandleGapEvent(struct ble_gap_event* event);
    void PrintConnection(uint16_t conn_handle);

    static int GapEventHandler(struct ble_gap_event* event, void* arg);
    static void OnSync();
    static void OnReset(int reason);
    static void HostTask(void* param);
    static void OnClearBondsEvent(struct ble_npl_event* ev);

    AncsClient ancs_client_;
    ConnectionCallback connection_callback_;
    std::string device_name_;
    uint8_t own_addr_type_ = 0;
    std::atomic<uint16_t> conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    struct ble_npl_event clear_bonds_event_;
};

#endif // BLE_MANAGER_H
