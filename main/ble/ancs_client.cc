#include "ancs_client.h"
#include "time_sync.h"

#include <esp_log.h>
#include <nimble/nimble_port.h>
#include <services/gap/ble_svc_gap.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#define TAG "AncsClient"

// Service UUID: 7905F431-B5CE-4E99-A40F-4B1E122D00D0
static const ble_uuid128_t kAncsServiceUuid = BLE_UUID128_INIT(
    0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
// Notification Source UUID: 9FBF120D-6301-42D9-8C58-25E699A21DBD (notifiable)
static const ble_uuid128_t kNotificationSourceUuid = BLE_UUID128_INIT(
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
// Control Point UUID: 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 (writeable with response)
static const ble_uuid128_t kControlPointUuid = BLE_UUID128_INIT(
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
// Data Source UUID: 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB (notifiable)
static const ble_uuid128_t kDataSourceUuid = BLE_UUID128_INIT(
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);
static const ble_uuid16_t kCccdUuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);
// GAP Device Name of the peer, e.g. "John's iPhone"
static const ble_uuid16_t kDeviceNameUuid = BLE_UUID16_INIT(BLE_SVC_GAP_CHR_UUID16_DEVICE_NAME);
// Current Time Service (0x1805), offered by iOS to bonded devices
static const ble_uuid16_t kCurrentTimeUuid = BLE_UUID16_INIT(0x2A2B);
static const ble_uuid16_t kLocalTimeInfoUuid = BLE_UUID16_INIT(0x2A0F);

// Attributes requested for every notification, in this order
struct RequestedAttribute {
    uint8_t id;
    uint16_t max_len;  // 0 means the attribute takes no length parameter
};
static const RequestedAttribute kNotificationAttributes[] = {
    {kAncsAttrAppIdentifier, 0},
    {kAncsAttrTitle, CONFIG_ANCS_MAX_TITLE_LEN},
    {kAncsAttrSubtitle, CONFIG_ANCS_MAX_SUBTITLE_LEN},
    {kAncsAttrMessage, CONFIG_ANCS_MAX_MESSAGE_LEN},
    {kAncsAttrMessageSize, 0},
    {kAncsAttrDate, 0},
    {kAncsAttrPositiveActionLabel, 0},
    {kAncsAttrNegativeActionLabel, 0},
};
static constexpr size_t kNotificationAttributeCount = sizeof(kNotificationAttributes) / sizeof(kNotificationAttributes[0]);

static constexpr uint32_t kDiscoveryRetryMs = 5000;
static constexpr uint32_t kRequestTimeoutMs = 5000;
static constexpr size_t kMaxQueuedRequests = 48;
static constexpr size_t kMaxDataBufferSize = 4096;
static constexpr size_t kMaxAppNameCache = 32;

static inline uint16_t ReadLe16(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

static inline uint32_t ReadLe32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void AppendLe32(std::string& s, uint32_t v) {
    s.push_back(v & 0xff);
    s.push_back((v >> 8) & 0xff);
    s.push_back((v >> 16) & 0xff);
    s.push_back((v >> 24) & 0xff);
}

// "yyyyMMdd'T'HHmmSS" -> "yyyy-MM-ddTHH:mm:SS"
static std::string FormatAncsDate(const std::string& date) {
    if (date.size() != 15 || date[8] != 'T') {
        return date;
    }
    return date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) + "T" +
           date.substr(9, 2) + ":" + date.substr(11, 2) + ":" + date.substr(13, 2);
}

const ble_uuid128_t& AncsClient::ServiceUuid() {
    return kAncsServiceUuid;
}

void AncsClient::Initialize() {
    ble_npl_callout_init(&retry_timer_, nimble_port_get_dflt_eventq(), OnRetryTimer, this);
    ble_npl_callout_init(&request_timer_, nimble_port_get_dflt_eventq(), OnRequestTimeout, this);
    ble_npl_event_init(&action_event_, OnActionEvent, this);
}

void AncsClient::SetNotificationCallback(NotificationCallback callback) {
    notification_callback_ = std::move(callback);
}

void AncsClient::OnEncrypted(uint16_t conn_handle) {
    if (conn_handle == conn_handle_ && state_ != State::kIdle) {
        return;  // Encryption refreshed, discovery already running or done
    }
    ResetConnectionState();
    conn_handle_ = conn_handle;

    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        // Identity address: stable for a bonded iPhone even though it uses private addresses over the air
        const uint8_t* a = desc.peer_id_addr.val;
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
        peer_address_ = addr;
    }
    StartDiscovery();
}

void AncsClient::OnDisconnected() {
    ResetConnectionState();
}

void AncsClient::ResetConnectionState() {
    // Report notifications already fetched, even though their app name is unknown
    for (auto& notification : waiting_for_app_name_) {
        Emit(notification);
    }
    waiting_for_app_name_.clear();

    ble_npl_callout_stop(&retry_timer_);
    ble_npl_callout_stop(&request_timer_);
    state_ = State::kIdle;
    conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
    service_start_handle_ = 0;
    service_end_handle_ = 0;
    notification_source_handle_ = 0;
    notification_source_cccd_ = 0;
    control_point_handle_ = 0;
    data_source_handle_ = 0;
    data_source_cccd_ = 0;
    characteristic_def_handles_.clear();
    requests_.clear();
    current_request_.reset();
    control_point_write_pending_ = false;
    data_buffer_.clear();
    peer_name_.clear();
    peer_address_.clear();
    time_read_ = false;
    current_time_.clear();
    local_time_info_.clear();
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void AncsClient::StartDiscovery() {
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    service_start_handle_ = 0;
    service_end_handle_ = 0;
    notification_source_handle_ = 0;
    control_point_handle_ = 0;
    data_source_handle_ = 0;
    notification_source_cccd_ = 0;
    data_source_cccd_ = 0;
    characteristic_def_handles_.clear();

    if (peer_name_.empty()) {
        ReadPeerName();
    } else {
        OnPeerInfoRead();
    }
}

void AncsClient::ReadPeerName() {
    state_ = State::kReadingPeerName;
    int rc = ble_gattc_read_by_uuid(conn_handle_, 1, 0xffff, &kDeviceNameUuid.u, OnPeerNameRead, this);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to read peer name, rc=%d", rc);
        OnPeerInfoRead();
    }
}

int AncsClient::HandlePeerNameRead(const struct ble_gatt_error* error, struct ble_gatt_attr* attr) {
    if (error->status == 0 && attr != nullptr) {
        char name[64];
        uint16_t len = std::min<uint16_t>(OS_MBUF_PKTLEN(attr->om), sizeof(name));
        os_mbuf_copydata(attr->om, 0, len, name);
        peer_name_.assign(name, len);
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Failed to read peer name, status=%d", error->status);
    }
    ESP_LOGI(TAG, "conn=%d peer \"%s\" (%s)", conn_handle_, peer_name_.c_str(), peer_address_.c_str());
    OnPeerInfoRead();
    return 0;
}

// Next step after the peer name: the current time once per connection (not on
// the retries while "Share System Notifications" is off), then ANCS
void AncsClient::OnPeerInfoRead() {
    if (time_read_) {
        DiscoverService();
        return;
    }
    time_read_ = true;
    ReadTime(State::kReadingCurrentTime);
}

void AncsClient::ReadTime(State state) {
    state_ = state;
    auto* uuid = state == State::kReadingCurrentTime ? &kCurrentTimeUuid.u : &kLocalTimeInfoUuid.u;
    int rc = ble_gattc_read_by_uuid(conn_handle_, 1, 0xffff, uuid, OnTimeRead, this);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to read current time, rc=%d", rc);
        DiscoverService();
    }
}

int AncsClient::HandleTimeRead(const struct ble_gatt_error* error, struct ble_gatt_attr* attr) {
    bool current_time = state_ == State::kReadingCurrentTime;
    if (error->status == 0 && attr != nullptr) {
        uint8_t value[16];
        uint16_t len = std::min<uint16_t>(OS_MBUF_PKTLEN(attr->om), sizeof(value));
        os_mbuf_copydata(attr->om, 0, len, value);
        (current_time ? current_time_ : local_time_info_).assign(reinterpret_cast<char*>(value), len);
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        // e.g. attribute not found: an iPad without the service, or not iOS
        ESP_LOGI(TAG, "conn=%d %s not available, status=%d", conn_handle_,
                 current_time ? "Current Time" : "Local Time Information", error->status);
    }
    if (current_time && !current_time_.empty()) {
        ReadTime(State::kReadingLocalTimeInfo);
        return 0;
    }
    if (!current_time_.empty()) {
        TimeSync::GetInstance().OnIphoneTime(current_time_, local_time_info_, peer_name_);
    }
    DiscoverService();
    return 0;
}

void AncsClient::DiscoverService() {
    ESP_LOGI(TAG, "conn=%d discovering ANCS service", conn_handle_);
    state_ = State::kDiscoveringService;
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle_, &kAncsServiceUuid.u, OnServiceDiscovered, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start service discovery, rc=%d", rc);
        ScheduleRetry();
    }
}

void AncsClient::ScheduleRetry() {
    state_ = State::kIdle;
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ble_npl_callout_reset(&retry_timer_, ble_npl_time_ms_to_ticks32(kDiscoveryRetryMs));
}

void AncsClient::OnRetryTimer(struct ble_npl_event* ev) {
    auto* self = static_cast<AncsClient*>(ble_npl_event_get_arg(ev));
    self->StartDiscovery();
}

int AncsClient::HandleServiceDiscovered(const struct ble_gatt_error* error, const struct ble_gatt_svc* service) {
    if (error->status == 0 && service != nullptr) {
        service_start_handle_ = service->start_handle;
        service_end_handle_ = service->end_handle;
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Service discovery failed, status=%d", error->status);
        ScheduleRetry();
        return 0;
    }

    if (service_start_handle_ == 0) {
        ESP_LOGW(TAG, "ANCS service not found on \"%s\". On the iPhone open Settings > Bluetooth > (i) next to "
                      "this device and enable \"Share System Notifications\". Retrying in %lu ms",
                 peer_name_.c_str(), (unsigned long)kDiscoveryRetryMs);
        ScheduleRetry();
        return 0;
    }

    ESP_LOGI(TAG, "ANCS service found, handles %u-%u", service_start_handle_, service_end_handle_);
    state_ = State::kDiscoveringCharacteristics;
    int rc = ble_gattc_disc_all_chrs(conn_handle_, service_start_handle_, service_end_handle_,
                                     OnCharacteristicDiscovered, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start characteristic discovery, rc=%d", rc);
        ScheduleRetry();
    }
    return 0;
}

int AncsClient::HandleCharacteristicDiscovered(const struct ble_gatt_error* error, const struct ble_gatt_chr* chr) {
    if (error->status == 0 && chr != nullptr) {
        characteristic_def_handles_.push_back(chr->def_handle);
        if (ble_uuid_cmp(&chr->uuid.u, &kNotificationSourceUuid.u) == 0) {
            notification_source_handle_ = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kControlPointUuid.u) == 0) {
            control_point_handle_ = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kDataSourceUuid.u) == 0) {
            data_source_handle_ = chr->val_handle;
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Characteristic discovery failed, status=%d", error->status);
        ScheduleRetry();
        return 0;
    }

    ESP_LOGI(TAG, "Notification Source=%u, Control Point=%u, Data Source=%u",
             notification_source_handle_, control_point_handle_, data_source_handle_);
    if (notification_source_handle_ == 0 || control_point_handle_ == 0 || data_source_handle_ == 0) {
        ESP_LOGE(TAG, "ANCS characteristics incomplete");
        ScheduleRetry();
        return 0;
    }

    // Data Source must be subscribed first, so discover its CCCD first
    state_ = State::kDiscoveringDescriptors;
    DiscoverDescriptors(data_source_handle_);
    return 0;
}

uint16_t AncsClient::GetCharacteristicEndHandle(uint16_t val_handle) const {
    uint16_t end = service_end_handle_;
    for (auto def_handle : characteristic_def_handles_) {
        if (def_handle > val_handle && def_handle - 1 < end) {
            end = def_handle - 1;
        }
    }
    return end;
}

void AncsClient::DiscoverDescriptors(uint16_t val_handle) {
    int rc = ble_gattc_disc_all_dscs(conn_handle_, val_handle, GetCharacteristicEndHandle(val_handle),
                                     OnDescriptorDiscovered, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start descriptor discovery, rc=%d", rc);
        ScheduleRetry();
    }
}

int AncsClient::HandleDescriptorDiscovered(const struct ble_gatt_error* error, uint16_t chr_val_handle,
                                           const struct ble_gatt_dsc* dsc) {
    if (error->status == 0 && dsc != nullptr) {
        if (ble_uuid_cmp(&dsc->uuid.u, &kCccdUuid.u) == 0) {
            if (chr_val_handle == data_source_handle_) {
                data_source_cccd_ = dsc->handle;
            } else if (chr_val_handle == notification_source_handle_) {
                notification_source_cccd_ = dsc->handle;
            }
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Descriptor discovery failed, status=%d", error->status);
        ScheduleRetry();
        return 0;
    }

    if (chr_val_handle == data_source_handle_) {
        if (data_source_cccd_ == 0) {
            ESP_LOGE(TAG, "Data Source CCCD not found");
            ScheduleRetry();
            return 0;
        }
        DiscoverDescriptors(notification_source_handle_);
        return 0;
    }

    if (notification_source_cccd_ == 0) {
        ESP_LOGE(TAG, "Notification Source CCCD not found");
        ScheduleRetry();
        return 0;
    }

    state_ = State::kSubscribing;
    WriteCccd(data_source_cccd_);
    return 0;
}

void AncsClient::WriteCccd(uint16_t cccd_handle) {
    static const uint8_t kEnableNotify[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn_handle_, cccd_handle, kEnableNotify, sizeof(kEnableNotify),
                                  OnCccdWritten, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to write CCCD %u, rc=%d", cccd_handle, rc);
        ScheduleRetry();
    }
}

int AncsClient::HandleCccdWritten(const struct ble_gatt_error* error, uint16_t attr_handle) {
    if (error->status != 0) {
        ESP_LOGE(TAG, "Subscribe failed, handle=%u status=%d", attr_handle, error->status);
        ScheduleRetry();
        return 0;
    }

    if (attr_handle == data_source_cccd_) {
        WriteCccd(notification_source_cccd_);
        return 0;
    }

    // Subscribing the Notification Source makes iOS replay existing notifications
    state_ = State::kReady;
    ESP_LOGI(TAG, "conn=%d ANCS ready on \"%s\", waiting for notifications", conn_handle_, peer_name_.c_str());
    ProcessNextRequest();
    return 0;
}

// ---------------------------------------------------------------------------
// Request queue (one Control Point request in flight at a time)
// ---------------------------------------------------------------------------

void AncsClient::EnqueueRequest(Request&& request, bool front) {
    if (requests_.size() >= kMaxQueuedRequests) {
        ESP_LOGW(TAG, "Request queue full, dropping the oldest request");
        requests_.pop_front();
    }
    if (front) {
        requests_.push_front(std::move(request));
    } else {
        requests_.push_back(std::move(request));
    }
    ProcessNextRequest();
}

void AncsClient::ProcessNextRequest() {
    if (state_ != State::kReady || current_request_ || control_point_write_pending_ || requests_.empty()) {
        return;
    }

    current_request_ = std::move(requests_.front());
    requests_.pop_front();
    data_buffer_.clear();

    auto& request = *current_request_;
    std::string cmd;
    switch (request.type) {
        case RequestType::kNotificationAttributes:
            cmd.push_back(kAncsCommandGetNotificationAttributes);
            AppendLe32(cmd, request.notification.uid);
            for (auto& attr : kNotificationAttributes) {
                cmd.push_back(attr.id);
                if (attr.max_len > 0) {
                    cmd.push_back(attr.max_len & 0xff);
                    cmd.push_back(attr.max_len >> 8);
                }
            }
            break;
        case RequestType::kAppAttributes:
            cmd.push_back(kAncsCommandGetAppAttributes);
            cmd.append(request.app_id);
            cmd.push_back('\0');
            cmd.push_back(kAncsAppAttrDisplayName);
            break;
        case RequestType::kPerformAction:
            cmd.push_back(kAncsCommandPerformNotificationAction);
            AppendLe32(cmd, request.uid);
            cmd.push_back(request.action);
            break;
    }

    int rc = ble_gattc_write_flat(conn_handle_, control_point_handle_, cmd.data(), cmd.size(),
                                  OnControlPointWritten, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to write Control Point, rc=%d", rc);
        current_request_.reset();
        return;
    }
    control_point_write_pending_ = true;
    ble_npl_callout_reset(&request_timer_, ble_npl_time_ms_to_ticks32(kRequestTimeoutMs));
}

int AncsClient::HandleControlPointWritten(const struct ble_gatt_error* error) {
    control_point_write_pending_ = false;
    if (!current_request_) {
        // The response already arrived and the request finished
        ProcessNextRequest();
        return 0;
    }

    if (error->status != 0) {
        bool is_att_error = error->status > BLE_HS_ERR_ATT_BASE && error->status < BLE_HS_ERR_HCI_BASE;
        ESP_LOGW(TAG, "Control Point write failed, status=0x%x (%s)", error->status,
                 is_att_error ? AncsErrorName(error->status - BLE_HS_ERR_ATT_BASE) : "host error");
        // InvalidParameter usually means the notification was removed in the meantime,
        // so there is nothing worth reporting for it
        FailCurrentRequest("rejected by iPhone", false);
        return 0;
    }

    if (current_request_->type == RequestType::kPerformAction) {
        ESP_LOGI(TAG, "Action %u performed on uid=%lu", current_request_->action, (unsigned long)current_request_->uid);
        FinishCurrentRequest();
    }
    return 0;
}

void AncsClient::FinishCurrentRequest() {
    ble_npl_callout_stop(&request_timer_);
    current_request_.reset();
    data_buffer_.clear();
    ProcessNextRequest();
}

void AncsClient::FailCurrentRequest(const char* reason, bool emit_header) {
    if (!current_request_) {
        return;
    }
    ESP_LOGW(TAG, "Request failed: %s", reason);
    ble_npl_callout_stop(&request_timer_);
    auto request = std::move(*current_request_);
    current_request_.reset();
    data_buffer_.clear();

    if (request.type == RequestType::kNotificationAttributes && emit_header) {
        // Still report what we know from the Notification Source
        Emit(request.notification);
    } else if (request.type == RequestType::kAppAttributes) {
        OnAppNameReady(request.app_id, "");
    }
    ProcessNextRequest();
}

void AncsClient::OnRequestTimeout(struct ble_npl_event* ev) {
    auto* self = static_cast<AncsClient*>(ble_npl_event_get_arg(ev));
    // Unblock the queue even if the write response never arrived
    self->control_point_write_pending_ = false;
    self->FailCurrentRequest("timeout", true);
}

void AncsClient::PerformAction(uint32_t uid, AncsActionId action) {
    {
        std::lock_guard<std::mutex> lock(action_mutex_);
        pending_actions_.emplace_back(uid, action);
    }
    // Hand over to the NimBLE host task
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &action_event_);
}

void AncsClient::OnActionEvent(struct ble_npl_event* ev) {
    auto* self = static_cast<AncsClient*>(ble_npl_event_get_arg(ev));
    std::vector<std::pair<uint32_t, uint8_t>> actions;
    {
        std::lock_guard<std::mutex> lock(self->action_mutex_);
        actions.swap(self->pending_actions_);
    }
    if (self->state_ != State::kReady) {
        ESP_LOGW(TAG, "ANCS not ready, dropping %u action(s)", (unsigned)actions.size());
        return;
    }
    for (auto& [uid, action] : actions) {
        Request request{RequestType::kPerformAction};
        request.uid = uid;
        request.action = action;
        // Actions are time sensitive (e.g. declining a call), put them first
        self->EnqueueRequest(std::move(request), true);
    }
}

// ---------------------------------------------------------------------------
// Notification Source / Data Source
// ---------------------------------------------------------------------------

bool AncsClient::OnNotifyRx(uint16_t attr_handle, struct os_mbuf* om) {
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE ||
        (attr_handle != notification_source_handle_ && attr_handle != data_source_handle_)) {
        return false;
    }

    uint8_t buf[512];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf), &len) != 0) {
        ESP_LOGW(TAG, "Notification too long (%u bytes), dropped", OS_MBUF_PKTLEN(om));
        return true;
    }

    if (attr_handle == notification_source_handle_) {
        HandleNotificationSource(buf, len);
    } else {
        HandleDataSource(buf, len);
    }
    return true;
}

void AncsClient::HandleNotificationSource(const uint8_t* data, size_t len) {
    if (len < 8) {
        ESP_LOGW(TAG, "Invalid Notification Source length %u", (unsigned)len);
        return;
    }

    AncsNotification notification;
    notification.event_id = data[0];
    notification.event_flags = data[1];
    notification.category_id = data[2];
    notification.category_count = data[3];
    notification.uid = ReadLe32(data + 4);
    ESP_LOGD(TAG, "Notification Source: event=%s uid=%lu category=%s flags=0x%02x",
             AncsEventName(notification.event_id), (unsigned long)notification.uid,
             AncsCategoryName(notification.category_id), notification.event_flags);

    // Requests still queued for this uid are obsolete now
    auto uid = notification.uid;
    auto is_same_uid = [uid](const Request& r) {
        return r.type == RequestType::kNotificationAttributes && r.notification.uid == uid;
    };

    if (notification.event_id == kAncsEventRemoved) {
        // Nothing left to fetch; consumers only get the header
        std::erase_if(requests_, is_same_uid);
        Emit(notification);
        return;
    }

#if CONFIG_ANCS_IGNORE_PREEXISTING
    if (notification.event_flags & kAncsFlagPreExisting) {
        return;
    }
#endif

    for (auto& r : requests_) {
        if (is_same_uid(r)) {
            // Not fetched yet: keep the newer header, fetch once
            r.notification = notification;
            return;
        }
    }

    Request request{RequestType::kNotificationAttributes};
    request.notification = notification;
    EnqueueRequest(std::move(request));
}

void AncsClient::HandleDataSource(const uint8_t* data, size_t len) {
    if (!current_request_ || current_request_->type == RequestType::kPerformAction) {
        ESP_LOGW(TAG, "Unexpected Data Source data (%u bytes), dropped", (unsigned)len);
        return;
    }

    data_buffer_.append(reinterpret_cast<const char*>(data), len);
    if (data_buffer_.size() > kMaxDataBufferSize) {
        FailCurrentRequest("response too large", true);
        return;
    }

    // A response may span several notifications; parse once it is complete
    bool complete = current_request_->type == RequestType::kNotificationAttributes
                        ? TryParseNotificationAttributes()
                        : TryParseAppAttributes();
    if (complete) {
        FinishCurrentRequest();
    }
}

bool AncsClient::TryParseNotificationAttributes() {
    auto* p = reinterpret_cast<const uint8_t*>(data_buffer_.data());
    size_t size = data_buffer_.size();
    if (size < 5) {
        return false;
    }

    auto& request = *current_request_;
    if (p[0] != kAncsCommandGetNotificationAttributes || ReadLe32(p + 1) != request.notification.uid) {
        FailCurrentRequest("mismatched notification attributes response", true);
        return false;
    }

    AncsNotification notification = request.notification;
    size_t pos = 5;
    for (size_t i = 0; i < kNotificationAttributeCount; i++) {
        if (pos + 3 > size) {
            return false;
        }
        uint8_t id = p[pos];
        uint16_t attr_len = ReadLe16(p + pos + 1);
        pos += 3;
        if (pos + attr_len > size) {
            return false;
        }
        std::string value(reinterpret_cast<const char*>(p + pos), attr_len);
        pos += attr_len;

        switch (id) {
            case kAncsAttrAppIdentifier: notification.app_id = std::move(value); break;
            case kAncsAttrTitle: notification.title = std::move(value); break;
            case kAncsAttrSubtitle: notification.subtitle = std::move(value); break;
            case kAncsAttrMessage: notification.message = std::move(value); break;
            case kAncsAttrMessageSize: notification.message_size = atoi(value.c_str()); break;
            case kAncsAttrDate: notification.date = FormatAncsDate(value); break;
            case kAncsAttrPositiveActionLabel: notification.positive_action = std::move(value); break;
            case kAncsAttrNegativeActionLabel: notification.negative_action = std::move(value); break;
            default: break;
        }
    }
    notification.has_attributes = true;

    // May enqueue an app attributes request, which is sent once this request finishes
    OnNotificationAttributesReady(std::move(notification));
    return true;
}

bool AncsClient::TryParseAppAttributes() {
    auto* p = reinterpret_cast<const uint8_t*>(data_buffer_.data());
    size_t size = data_buffer_.size();
    if (size < 1) {
        return false;
    }
    if (p[0] != kAncsCommandGetAppAttributes) {
        FailCurrentRequest("mismatched app attributes response", false);
        return false;
    }

    auto terminator = data_buffer_.find('\0', 1);
    if (terminator == std::string::npos) {
        return false;
    }
    std::string app_id = data_buffer_.substr(1, terminator - 1);

    size_t pos = terminator + 1;
    if (pos + 3 > size) {
        return false;
    }
    uint16_t attr_len = ReadLe16(p + pos + 1);
    pos += 3;
    if (pos + attr_len > size) {
        return false;
    }
    std::string app_name(reinterpret_cast<const char*>(p + pos), attr_len);

    OnAppNameReady(app_id, app_name);
    return true;
}

void AncsClient::OnNotificationAttributesReady(AncsNotification&& notification) {
#if CONFIG_ANCS_FETCH_APP_NAME
    if (!notification.app_id.empty()) {
        auto it = app_names_.find(notification.app_id);
        if (it != app_names_.end()) {
            notification.app_name = it->second;
        } else {
            bool already_requested = false;
            for (auto& waiting : waiting_for_app_name_) {
                if (waiting.app_id == notification.app_id) {
                    already_requested = true;
                    break;
                }
            }
            if (!already_requested) {
                Request request{RequestType::kAppAttributes};
                request.app_id = notification.app_id;
                EnqueueRequest(std::move(request), true);
            }
            waiting_for_app_name_.push_back(std::move(notification));
            return;
        }
    }
#endif
    Emit(notification);
}

void AncsClient::OnAppNameReady(const std::string& app_id, const std::string& app_name) {
    if (app_names_.size() >= kMaxAppNameCache) {
        app_names_.clear();
    }
    // Cache empty names too, so a failing app is not queried again and again
    app_names_[app_id] = app_name;

    for (auto it = waiting_for_app_name_.begin(); it != waiting_for_app_name_.end();) {
        if (it->app_id == app_id) {
            it->app_name = app_name;
            Emit(*it);
            it = waiting_for_app_name_.erase(it);
        } else {
            ++it;
        }
    }
}

void AncsClient::Emit(const AncsNotification& notification) {
    if (!notification_callback_) {
        return;
    }
    // Tag every notification with the device it came from
    AncsNotification tagged = notification;
    tagged.conn_handle = conn_handle_;
    tagged.peer_name = peer_name_;
    tagged.peer_address = peer_address_;
    notification_callback_(tagged);
}

// ---------------------------------------------------------------------------
// NimBLE C callbacks
// ---------------------------------------------------------------------------

int AncsClient::OnServiceDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                    const struct ble_gatt_svc* service, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleServiceDiscovered(error, service);
}

int AncsClient::OnCharacteristicDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                           const struct ble_gatt_chr* chr, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleCharacteristicDiscovered(error, chr);
}

int AncsClient::OnDescriptorDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                       uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleDescriptorDiscovered(error, chr_val_handle, dsc);
}

int AncsClient::OnPeerNameRead(uint16_t conn_handle, const struct ble_gatt_error* error,
                               struct ble_gatt_attr* attr, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandlePeerNameRead(error, attr);
}

int AncsClient::OnTimeRead(uint16_t conn_handle, const struct ble_gatt_error* error,
                           struct ble_gatt_attr* attr, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleTimeRead(error, attr);
}

int AncsClient::OnCccdWritten(uint16_t conn_handle, const struct ble_gatt_error* error,
                              struct ble_gatt_attr* attr, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleCccdWritten(error, attr ? attr->handle : 0);
}

int AncsClient::OnControlPointWritten(uint16_t conn_handle, const struct ble_gatt_error* error,
                                      struct ble_gatt_attr* attr, void* arg) {
    auto* self = static_cast<AncsClient*>(arg);
    if (conn_handle != self->conn_handle_) {
        return 0;  // Stale procedure from a previous connection
    }
    return self->HandleControlPointWritten(error);
}
