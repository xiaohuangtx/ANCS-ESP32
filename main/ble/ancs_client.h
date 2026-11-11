#ifndef ANCS_CLIENT_H
#define ANCS_CLIENT_H

#include <host/ble_hs.h>
#include <nimble/nimble_npl.h>

#include <string>
#include <deque>
#include <vector>
#include <map>
#include <mutex>
#include <optional>
#include <functional>

#include "ancs_defs.h"
#include "notification.h"

/**
 * ANCS (Apple Notification Center Service) GATT client.
 *
 * The iPhone is the GATT server (Notification Provider), the ESP32 is the
 * GATT client (Notification Consumer). Flow:
 *   1. Link encrypted -> read the peer's device name -> discover ANCS
 *      service and its 3 characteristics
 *   2. Subscribe Data Source, then Notification Source
 *   3. Notification Source event -> write Get Notification Attributes to the
 *      Control Point -> reassemble the response from Data Source
 *
 * One instance serves one connection; BleManager owns one per connection slot.
 * All Control Point writes go through one queue so that only one ATT request
 * is in flight at a time. Except for PerformAction(), every method must be
 * called from the NimBLE host task.
 */
class AncsClient {
public:
    using NotificationCallback = std::function<void(const AncsNotification&)>;

    AncsClient() = default;
    AncsClient(const AncsClient&) = delete;
    AncsClient& operator=(const AncsClient&) = delete;

    // Must be called after nimble_port_init()
    void Initialize();
    // The callback is invoked from the NimBLE host task, do not block in it
    void SetNotificationCallback(NotificationCallback callback);

    // GAP hooks, called by BleManager
    void OnEncrypted(uint16_t conn_handle);
    void OnDisconnected();
    // Returns true if the notification belongs to ANCS
    bool OnNotifyRx(uint16_t attr_handle, struct os_mbuf* om);

    bool IsReady() const { return state_ == State::kReady; }
    uint16_t GetConnHandle() const { return conn_handle_; }
    static const ble_uuid128_t& ServiceUuid();

    // Thread-safe: trigger the positive (e.g. answer) or negative (e.g. decline) action
    void PerformAction(uint32_t uid, AncsActionId action);

private:
    enum class State {
        kIdle,
        kReadingPeerName,
        kDiscoveringService,
        kDiscoveringCharacteristics,
        kDiscoveringDescriptors,
        kSubscribing,
        kReady,
    };

    enum class RequestType {
        kNotificationAttributes,
        kAppAttributes,
        kPerformAction,
    };

    struct Request {
        RequestType type;
        AncsNotification notification;  // kNotificationAttributes: header from Notification Source
        std::string app_id;             // kAppAttributes
        uint32_t uid = 0;               // kPerformAction
        uint8_t action = 0;             // kPerformAction
    };

    // Discovery
    void StartDiscovery();
    void ReadPeerName();
    void DiscoverService();
    void ScheduleRetry();
    void DiscoverDescriptors(uint16_t val_handle);
    void WriteCccd(uint16_t cccd_handle);
    uint16_t GetCharacteristicEndHandle(uint16_t val_handle) const;

    int HandlePeerNameRead(const struct ble_gatt_error* error, struct ble_gatt_attr* attr);
    int HandleServiceDiscovered(const struct ble_gatt_error* error, const struct ble_gatt_svc* service);
    int HandleCharacteristicDiscovered(const struct ble_gatt_error* error, const struct ble_gatt_chr* chr);
    int HandleDescriptorDiscovered(const struct ble_gatt_error* error, uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc);
    int HandleCccdWritten(const struct ble_gatt_error* error, uint16_t attr_handle);
    int HandleControlPointWritten(const struct ble_gatt_error* error);

    // Request queue
    void EnqueueRequest(Request&& request, bool front = false);
    void ProcessNextRequest();
    void FinishCurrentRequest();
    void FailCurrentRequest(const char* reason, bool emit_header);

    // Data handling
    void HandleNotificationSource(const uint8_t* data, size_t len);
    void HandleDataSource(const uint8_t* data, size_t len);
    bool TryParseNotificationAttributes();
    bool TryParseAppAttributes();
    void OnNotificationAttributesReady(AncsNotification&& notification);
    void OnAppNameReady(const std::string& app_id, const std::string& app_name);
    void Emit(const AncsNotification& notification);
    void ResetConnectionState();

    // NimBLE C callbacks
    static int OnServiceDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                   const struct ble_gatt_svc* service, void* arg);
    static int OnCharacteristicDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                          const struct ble_gatt_chr* chr, void* arg);
    static int OnDescriptorDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                      uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc, void* arg);
    static int OnPeerNameRead(uint16_t conn_handle, const struct ble_gatt_error* error,
                              struct ble_gatt_attr* attr, void* arg);
    static int OnCccdWritten(uint16_t conn_handle, const struct ble_gatt_error* error,
                             struct ble_gatt_attr* attr, void* arg);
    static int OnControlPointWritten(uint16_t conn_handle, const struct ble_gatt_error* error,
                                     struct ble_gatt_attr* attr, void* arg);
    static void OnRetryTimer(struct ble_npl_event* ev);
    static void OnRequestTimeout(struct ble_npl_event* ev);
    static void OnActionEvent(struct ble_npl_event* ev);

    NotificationCallback notification_callback_;
    State state_ = State::kIdle;
    uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    std::string peer_name_;
    std::string peer_address_;

    // GATT handles
    uint16_t service_start_handle_ = 0;
    uint16_t service_end_handle_ = 0;
    uint16_t notification_source_handle_ = 0;
    uint16_t notification_source_cccd_ = 0;
    uint16_t control_point_handle_ = 0;
    uint16_t data_source_handle_ = 0;
    uint16_t data_source_cccd_ = 0;
    std::vector<uint16_t> characteristic_def_handles_;

    // Requests
    std::deque<Request> requests_;
    std::optional<Request> current_request_;
    bool control_point_write_pending_ = false;
    std::string data_buffer_;

    // App display name cache and notifications waiting for it
    std::map<std::string, std::string> app_names_;
    std::vector<AncsNotification> waiting_for_app_name_;

    // Actions requested from other tasks
    std::mutex action_mutex_;
    std::vector<std::pair<uint32_t, uint8_t>> pending_actions_;

    struct ble_npl_callout retry_timer_;
    struct ble_npl_callout request_timer_;
    struct ble_npl_event action_event_;
};

#endif // ANCS_CLIENT_H
