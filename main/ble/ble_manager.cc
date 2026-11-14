#include "ble_manager.h"
#include "settings.h"

#include <esp_log.h>
#include <algorithm>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#define TAG "BleManager"

extern "C" void ble_store_config_init(void);

void BleManager::Initialize(const std::string& device_name) {
    device_name_ = device_name;
    {
        Settings settings("ble");
        max_connections_ = std::clamp((int)settings.GetInt("max_conn", CONFIG_BLE_MAX_CONNECTIONS), 1, kMaxConnections);
    }
    ESP_LOGI(TAG, "Up to %d device(s) at the same time", GetMaxConnections());

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE, err=%d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // ANCS requires an encrypted link. The ESP32 has no display or keyboard,
    // so use "Just Works" pairing with bonding and LE Secure Connections.
    // The identity key (IRK) is required to recognize the iPhone, which uses
    // a resolvable private address that changes periodically.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(device_name_.c_str());
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name, rc=%d", rc);
    }
    ble_svc_gap_device_appearance_set(BLE_SVC_GAP_APPEARANCE_GEN_UNKNOWN);

    // Bonds are persisted in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST)
    ble_store_config_init();

    for (auto& link : links_) {
        link.ancs.Initialize();
    }
    ble_npl_event_init(&clear_bonds_event_, OnClearBondsEvent, this);
    ble_npl_event_init(&advertise_event_, OnAdvertiseEvent, this);
    ble_npl_callout_init(&advertise_retry_timer_, nimble_port_get_dflt_eventq(), OnAdvertiseRetry, this);
    initialized_ = true;

    nimble_port_freertos_init(HostTask);
}

void BleManager::HostTask(void* param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    // Returns only when nimble_port_stop() is called
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleManager::OnReset(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

void BleManager::OnSync() {
    auto& self = GetInstance();

    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "No usable BLE address, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &self.own_addr_type_);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to determine address type, rc=%d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(self.own_addr_type_, addr, nullptr);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x, name \"%s\"",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], self.device_name_.c_str());

    self.StartAdvertising();
}

void BleManager::StartAdvertising() {
    if (!ble_hs_synced()) {
        return;  // OnSync() starts advertising
    }
    if (GetConnectionCount() >= GetMaxConnections()) {
        // All slots taken, or the limit was lowered from the admin page
        ble_npl_callout_stop(&advertise_retry_timer_);
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
            ESP_LOGI(TAG, "Advertising stopped (%d/%d connected)", GetConnectionCount(), GetMaxConnections());
        }
        return;
    }
    if (ble_gap_adv_active()) {
        return;
    }

    // Advertising data: flags + ANCS service solicitation (3 + 18 bytes),
    // plus as much of the name as fits in the remaining 10 bytes.
    // Soliciting ANCS makes iOS show the device in Settings > Bluetooth and
    // offer "Share System Notifications". The name is also put here because
    // iOS may not use the scan response when listing devices.
    static constexpr size_t kMaxAdvNameLen = 31 - 3 - 18 - 2;
    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.sol_uuids128 = &AncsClient::ServiceUuid();
    fields.sol_num_uuids128 = 1;
    fields.name = reinterpret_cast<const uint8_t*>(device_name_.c_str());
    fields.name_len = std::min(device_name_.size(), kMaxAdvNameLen);
    fields.name_is_complete = device_name_.size() <= kMaxAdvNameLen;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising data, rc=%d", rc);
        return;
    }

    // Scan response: complete device name
    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.name = reinterpret_cast<const uint8_t*>(device_name_.c_str());
    rsp_fields.name_len = device_name_.size();
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan response data, rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER, &adv_params, GapEventHandler, this);
    if (rc == BLE_HS_ENOMEM) {
        // A previous link is still being released. Do not rely on a later
        // disconnect event to restart advertising: retry until it succeeds.
        if (GetConnectionCount() < GetMaxConnections()) {
            ESP_LOGI(TAG, "Connection slot busy, retrying advertising in 500 ms");
            ble_npl_callout_reset(&advertise_retry_timer_, ble_npl_time_ms_to_ticks32(500));
        }
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising, rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising started (%d/%d connected)", GetConnectionCount(), GetMaxConnections());
}

void BleManager::ScheduleAdvertising() {
    // Run after the current GAP event, once NimBLE has released the connection.
    // Calling ble_gap_adv_start() directly from a failed connect / disconnect
    // event fails with BLE_HS_ENOMEM because the connection is not freed yet.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &advertise_event_);
}

void BleManager::OnAdvertiseEvent(struct ble_npl_event* ev) {
    static_cast<BleManager*>(ble_npl_event_get_arg(ev))->StartAdvertising();
}

void BleManager::OnAdvertiseRetry(struct ble_npl_event* ev) {
    auto* self = static_cast<BleManager*>(ble_npl_event_get_arg(ev));
    self->StartAdvertising();
}

BleManager::Link* BleManager::FindLink(uint16_t conn_handle) {
    for (auto& link : links_) {
        if (link.conn_handle == conn_handle) {
            return &link;
        }
    }
    return nullptr;
}

BleManager::Link* BleManager::AllocateLink(uint16_t conn_handle) {
    Link* link = FindLink(BLE_HS_CONN_HANDLE_NONE);
    if (link != nullptr) {
        link->conn_handle = conn_handle;
    }
    return link;
}

void BleManager::SetMaxConnections(int max_connections) {
    max_connections_ = std::clamp(max_connections, 1, kMaxConnections);
    Settings settings("ble", true);
    settings.SetInt("max_conn", max_connections_);
    ESP_LOGI(TAG, "Up to %d device(s) at the same time", GetMaxConnections());
    // Start or stop advertising to match the new limit
    if (initialized_) {
        ScheduleAdvertising();
    }
}

int BleManager::GetConnectionCount() const {
    int count = 0;
    for (auto& link : links_) {
        if (link.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            count++;
        }
    }
    return count;
}

std::vector<AncsClient::PeerInfo> BleManager::GetPeers() const {
    std::vector<AncsClient::PeerInfo> peers;
    for (auto& link : links_) {
        if (link.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            peers.push_back(link.ancs.GetPeerInfo());
        }
    }
    return peers;
}

void BleManager::SetNotificationCallback(AncsClient::NotificationCallback callback) {
    for (auto& link : links_) {
        link.ancs.SetNotificationCallback(callback);
    }
}

bool BleManager::PerformAction(uint16_t conn_handle, uint32_t uid, AncsActionId action) {
    Link* link = FindLink(conn_handle);
    if (link == nullptr) {
        return false;
    }
    link->ancs.PerformAction(uid, action);
    return true;
}

void BleManager::PrintConnection(uint16_t conn_handle) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return;
    }
    const uint8_t* a = desc.peer_id_addr.val;
    ESP_LOGI(TAG, "conn=%d peer=%02x:%02x:%02x:%02x:%02x:%02x itvl=%d latency=%d timeout=%d "
                  "encrypted=%d authenticated=%d bonded=%d",
             conn_handle, a[5], a[4], a[3], a[2], a[1], a[0],
             desc.conn_itvl, desc.conn_latency, desc.supervision_timeout,
             desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);
}

int BleManager::GapEventHandler(struct ble_gap_event* event, void* arg) {
    return static_cast<BleManager*>(arg)->HandleGapEvent(event);
}

int BleManager::HandleGapEvent(struct ble_gap_event* event) {
    struct ble_gap_conn_desc desc;
    Link* link;
    int rc;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                // In the ESP-IDF NimBLE port, a peripheral link that drops before the remote
                // version/features are read reports the HCI reason here (e.g. 0x13: the iPhone
                // disconnected), and a DISCONNECT event may follow
                ESP_LOGW(TAG, "Connection failed, status=0x%x", event->connect.status);
                ScheduleAdvertising();
                return 0;
            }
            link = AllocateLink(event->connect.conn_handle);
            if (link == nullptr) {
                // NimBLE never accepts more than CONFIG_BT_NIMBLE_MAX_CONNECTIONS, so this should not happen
                ESP_LOGE(TAG, "No free connection slot");
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_CONN_LIMIT);
                return 0;
            }
            ESP_LOGI(TAG, "Connected (%d/%d)", GetConnectionCount(), GetMaxConnections());
            PrintConnection(event->connect.conn_handle);
            if (connection_callback_) {
                connection_callback_(event->connect.conn_handle, true, GetConnectionCount());
            }
            // Advertising stops when a connection is established; keep accepting more devices
            if (GetConnectionCount() < GetMaxConnections()) {
                ScheduleAdvertising();
            } else {
                ble_npl_callout_stop(&advertise_retry_timer_);
            }

            // A bonded iPhone may restore encryption before this event is reported (the
            // ESP-IDF port delays it until the remote features are read), so the earlier
            // BLE_GAP_EVENT_ENC_CHANGE found no link. Start ANCS now; initiating security
            // again would only send a Security Request the iPhone ignores, and SMP times out.
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 && desc.sec_state.encrypted) {
                link->ancs.OnEncrypted(event->connect.conn_handle);
                return 0;
            }

            // Pair on first connection, or restore encryption with the stored bond.
            // ANCS discovery starts after BLE_GAP_EVENT_ENC_CHANGE.
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to initiate security, rc=%d", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            link = FindLink(event->disconnect.conn.conn_handle);
            if (link != nullptr) {
                link->ancs.OnDisconnected();
                link->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            }
            // No link means the connection failed before BLE_GAP_EVENT_CONNECT succeeded
            ESP_LOGI(TAG, "Disconnected, conn=%d reason=0x%x (%d/%d)", event->disconnect.conn.conn_handle,
                     event->disconnect.reason, GetConnectionCount(), GetMaxConnections());
            if (link != nullptr && connection_callback_) {
                connection_callback_(event->disconnect.conn.conn_handle, false, GetConnectionCount());
            }
            ScheduleAdvertising();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ScheduleAdvertising();
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status == BLE_HS_ENOTCONN) {
                // The link dropped before pairing/encryption finished; see the following disconnect reason
                ESP_LOGW(TAG, "Connection lost before encryption completed");
                return 0;
            }
            if (event->enc_change.status != 0) {
                ESP_LOGE(TAG, "Encryption failed, status=0x%x", event->enc_change.status);
                if (event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
                    event->enc_change.status == BLE_HS_ENOENT) {
                    ESP_LOGW(TAG, "The pairing keys do not match. On the iPhone open Settings > Bluetooth, "
                                  "tap (i) next to \"%s\", choose \"Forget This Device\" and connect again",
                             device_name_.c_str());
                }
                return 0;
            }
            ESP_LOGI(TAG, "Link encrypted");
            PrintConnection(event->enc_change.conn_handle);
            link = FindLink(event->enc_change.conn_handle);
            if (link != nullptr) {
                link->ancs.OnEncrypted(event->enc_change.conn_handle);
            }
            // Otherwise BLE_GAP_EVENT_CONNECT has not been reported yet and starts ANCS itself
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            // The iPhone forgot us and pairs again: drop the old bond and accept the new one
            ESP_LOGI(TAG, "Repeat pairing, deleting the old bond");
            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            // Not expected with NO_IO, but accept numeric comparison to be safe
            struct ble_sm_io pkey = {};
            pkey.action = event->passkey.params.action;
            if (pkey.action == BLE_SM_IOACT_NUMCMP) {
                pkey.numcmp_accept = 1;
                ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            } else {
                ESP_LOGW(TAG, "Unsupported passkey action %d", pkey.action);
            }
            return 0;
        }

        case BLE_GAP_EVENT_NOTIFY_RX:
            link = FindLink(event->notify_rx.conn_handle);
            if (link == nullptr || !link->ancs.OnNotifyRx(event->notify_rx.attr_handle, event->notify_rx.om)) {
                ESP_LOGD(TAG, "Notification from unknown handle %u", event->notify_rx.attr_handle);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated to %d", event->mtu.value);
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE:
            PrintConnection(event->conn_update.conn_handle);
            return 0;

        default:
            return 0;
    }
}

void BleManager::ClearBonds() {
    // Run on the NimBLE host task to avoid racing with the stack
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &clear_bonds_event_);
}

void BleManager::OnClearBondsEvent(struct ble_npl_event* ev) {
    auto* self = static_cast<BleManager*>(ble_npl_event_get_arg(ev));
    int rc = ble_store_clear();
    ESP_LOGW(TAG, "All BLE bonds erased (rc=%d). Also choose \"Forget This Device\" on the iPhone", rc);
    for (auto& link : self->links_) {
        uint16_t conn_handle = link.conn_handle;
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}
