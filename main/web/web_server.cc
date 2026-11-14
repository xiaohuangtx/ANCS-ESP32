#include "web_server.h"
#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"
#include "notification_history.h"
#include "notification_store.h"
#include "notification_filter.h"
#include "time_sync.h"
#include "encoding.h"
#include "log_buffer.h"
#include "settings.h"
#include "http_utils.h"
#include "ancs_defs.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <memory>

#define TAG "WebServer"

extern const char admin_html_start[] asm("_binary_admin_html_start");
extern const char admin_html_end[] asm("_binary_admin_html_end");

static const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_EXT: return "external";
        default: return "other";
    }
}

// Leave sockets for the webhook client (CONFIG_LWIP_MAX_SOCKETS is 10). Every
// open admin page keeps one for /api/events.
static constexpr int kMaxOpenSockets = 5;
static constexpr size_t kOtaBufferSize = 4096;
// Image header + first segment header + app descriptor, enough to identify the image
static constexpr size_t kMinImageSize =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

static WebServer* Self(httpd_req_t* req) {
    return static_cast<WebServer*>(req->user_ctx);
}

// Run the callback on the esp_timer task after a delay, so the HTTP response
// reaches the browser first. The timer is created once and reused.
static void RunLater(esp_timer_handle_t& timer, esp_timer_cb_t callback, int delay_ms) {
    if (timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = callback;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "web_delayed";
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, delay_ms * 1000ULL);
}

static void ScheduleRestart() {
    static esp_timer_handle_t timer = nullptr;
    RunLater(timer, [](void*) {
        ESP_LOGI(TAG, "Restarting");
        esp_restart();
    }, 1000);
}

// A freshly updated firmware stays "pending verify" until confirmed here; if it
// crashes before getting this far, the bootloader rolls back on the next boot
static void MarkFirmwareValid() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware in %s as valid", running->label);
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void WebServer::Start() {
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_ != nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings("web");
        password_ = settings.GetString("password", CONFIG_WEB_ADMIN_PASSWORD);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Large enough for the webhook test, which runs a TLS handshake in this task
    config.stack_size = 8192;
    config.max_uri_handlers = 28;
    config.max_open_sockets = kMaxOpenSockets;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        server_ = nullptr;
        return;
    }

    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, HandleIndex, this},
        {"/api/status", HTTP_GET, HandleStatus, this},
        {"/api/logs", HTTP_GET, HandleLogs, this},
        {"/api/notifications", HTTP_GET, HandleGetNotifications, this},
        {"/api/notifications", HTTP_DELETE, HandleClearNotifications, this},
        {"/api/notifications/action", HTTP_POST, HandleNotificationAction, this},
        {.uri = "/api/events", .method = HTTP_GET, .handler = HandleEvents, .user_ctx = this,
         .is_websocket = true, .ws_pre_handshake_cb = HandleEventsHandshake},
        {"/api/config", HTTP_GET, HandleGetConfig, this},
        {"/api/config", HTTP_POST, HandleSetConfig, this},
        {"/api/webhooks", HTTP_GET, HandleGetWebhooks, this},
        {"/api/webhooks", HTTP_POST, HandleSetWebhook, this},
        {"/api/webhooks", HTTP_DELETE, HandleRemoveWebhook, this},
        {"/api/webhooks/test", HTTP_POST, HandleWebhookTest, this},
        {"/api/filters", HTTP_GET, HandleGetFilters, this},
        {"/api/filters", HTTP_POST, HandleSetFilters, this},
        {"/api/ota", HTTP_POST, HandleOta, this},
        {"/api/reboot", HTTP_POST, HandleReboot, this},
        {"/api/ble/clear-bonds", HTTP_POST, HandleClearBonds, this},
        {"/api/wifi/config-mode", HTTP_POST, HandleWifiConfigMode, this},
        {"/api/wifi", HTTP_GET, HandleGetWifi, this},
        {"/api/wifi", HTTP_POST, HandleAddWifi, this},
        {"/api/wifi", HTTP_PUT, HandleUpdateWifi, this},
        {"/api/wifi", HTTP_DELETE, HandleRemoveWifi, this},
    };
    for (auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_server_ = server_;
        // A signal still queued when the server stopped was dropped with it
        notifications_changed_queued_ = false;
    }

    ESP_LOGI(TAG, "Admin page: http://%s/%s", WifiService::GetInstance().GetIpAddress().c_str(),
             HasPassword() ? " (password protected)" : "");
    MarkFirmwareValid();
}

void WebServer::Stop() {
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_server_ = nullptr;
    }
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(TAG, "Server stopped");
}

bool WebServer::HasPassword() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !password_.empty();
}

void WebServer::SetPassword(const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    password_ = password;
    Settings settings("web", true);
    settings.SetString("password", password);
}

// "" if the header is missing or too long
static std::string GetHeader(httpd_req_t* req, const char* name) {
    size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length == 0 || length >= 256) {
        return "";
    }
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) {
        return "";
    }
    value.resize(length);
    return value;
}

bool WebServer::CheckAuth(httpd_req_t* req) {
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (password_.empty()) {
            return true;
        }
        expected = "Basic " + Base64Encode("admin:" + password_);
    }

    if (GetHeader(req, "Authorization") == expected) {
        return true;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ANCS-ESP32\", charset=\"UTF-8\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

esp_err_t WebServer::HandleIndex(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, admin_html_start, admin_html_end - admin_html_start - 1);
}

esp_err_t WebServer::HandleStatus(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    auto app_desc = esp_app_get_description();
    auto& wifi = WifiService::GetInstance();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "project", app_desc->project_name);
    cJSON_AddStringToObject(json, "version", app_desc->version);
    cJSON_AddStringToObject(json, "idf_version", app_desc->idf_ver);
    std::string build_time = std::string(app_desc->date) + " " + app_desc->time;
    cJSON_AddStringToObject(json, "build_time", build_time.c_str());
    cJSON_AddStringToObject(json, "chip", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(json, "mac", mac_str);
    cJSON_AddNumberToObject(json, "uptime", (double)(esp_timer_get_time() / 1000000));

    cJSON* wifi_json = cJSON_AddObjectToObject(json, "wifi");
    cJSON_AddStringToObject(wifi_json, "ssid", wifi.GetSsid().c_str());
    cJSON_AddStringToObject(wifi_json, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddNumberToObject(wifi_json, "rssi", wifi.GetRssi());

    cJSON* ble_json = cJSON_AddObjectToObject(json, "ble");
    cJSON_AddStringToObject(ble_json, "name", BleManager::GetInstance().GetDeviceName().c_str());
    cJSON_AddNumberToObject(ble_json, "connections", BleManager::GetInstance().GetConnectionCount());
    cJSON_AddNumberToObject(ble_json, "max_connections", BleManager::GetInstance().GetMaxConnections());
    cJSON* peers_json = cJSON_AddArrayToObject(ble_json, "peers");
    for (auto& peer : BleManager::GetInstance().GetPeers()) {
        cJSON* peer_json = cJSON_CreateObject();
        cJSON_AddStringToObject(peer_json, "name", peer.name.c_str());
        cJSON_AddStringToObject(peer_json, "address", peer.address.c_str());
        if (peer.battery_level >= 0) {
            cJSON_AddNumberToObject(peer_json, "battery", peer.battery_level);
        }
        cJSON_AddItemToArray(peers_json, peer_json);
    }

    cJSON* heap_json = cJSON_AddObjectToObject(json, "heap");
    cJSON_AddNumberToObject(heap_json, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap_json, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap_json, "internal_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap_json, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(heap_json, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

    cJSON* ota_json = cJSON_AddObjectToObject(json, "ota");
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    cJSON_AddStringToObject(ota_json, "running", running ? running->label : "");
    cJSON_AddStringToObject(ota_json, "next", next ? next->label : "");
    cJSON_AddNumberToObject(ota_json, "max_size", next ? next->size : 0);

    auto& history = NotificationHistory::GetInstance();
    cJSON_AddNumberToObject(json, "notification_count", history.GetCount());
    cJSON_AddNumberToObject(json, "history_size", history.GetCapacity());
    uint32_t targets = WebhookClient::GetInstance().ResolveTargets(0);
    cJSON_AddBoolToObject(json, "webhook_enabled", targets != 0);
    cJSON_AddNumberToObject(json, "webhook_targets", __builtin_popcount(targets));

    auto time_status = TimeSync::GetInstance().GetStatus();
    cJSON* time_json = cJSON_AddObjectToObject(json, "time");
    cJSON_AddBoolToObject(time_json, "set", TimeSync::IsClockSet());
    cJSON_AddNumberToObject(time_json, "now", (double)time(nullptr));
    cJSON_AddStringToObject(time_json, "source", TimeSourceName(time_status.source));
    cJSON_AddNumberToObject(time_json, "last_sync", (double)time_status.last_sync);
    cJSON_AddNumberToObject(time_json, "utc_offset", time_status.utc_offset);
    cJSON_AddBoolToObject(time_json, "offset_from_iphone", time_status.offset_from_iphone);

    cJSON* diag_json = cJSON_AddObjectToObject(json, "diag");
    cJSON_AddStringToObject(diag_json, "reset_reason", ResetReasonName(esp_reset_reason()));
    auto diag = Application::GetInstance().GetDiagnostics();
    cJSON_AddNumberToObject(diag_json, "notifications_received", diag.notifications_received);
    cJSON_AddNumberToObject(diag_json, "notifications_handled", diag.notifications_handled);
    cJSON_AddNumberToObject(diag_json, "notifications_skipped", diag.notifications_skipped);
    cJSON_AddNumberToObject(diag_json, "notifications_dropped", diag.notifications_dropped);
    cJSON_AddNumberToObject(diag_json, "pending_tasks", diag.pending_tasks);
    cJSON_AddNumberToObject(diag_json, "busy_seconds", diag.busy_seconds);
    auto& store = NotificationStore::GetInstance();
    cJSON_AddBoolToObject(diag_json, "store_enabled", store.IsEnabled());
    if (store.IsEnabled()) {
        auto stats = store.GetStats();
        cJSON_AddNumberToObject(diag_json, "store_sectors", stats.sectors);
        cJSON_AddNumberToObject(diag_json, "store_used_sectors", stats.used_sectors);
        cJSON_AddNumberToObject(diag_json, "store_errors", stats.errors);
        cJSON_AddStringToObject(diag_json, "store_last_error", esp_err_to_name(stats.last_error));
    }
    return SendJson(req, json);
}

esp_err_t WebServer::HandleLogs(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    // ?boot=<X-Log-Boot>&since=<X-Log-Next> from the previous response
    auto& logs = LogBuffer::GetInstance();
    uint64_t since = 0;
    char query[64];
    char value[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "boot", value, sizeof(value)) == ESP_OK &&
        strtoul(value, nullptr, 10) == logs.GetBootId() &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = strtoull(value, nullptr, 10);
    }
    auto result = logs.Read(since);
    // The header values must live until the response is sent
    std::string boot = std::to_string(logs.GetBootId());
    std::string next = std::to_string(result.next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Boot", boot.c_str());
    httpd_resp_set_hdr(req, "X-Log-Next", next.c_str());
    httpd_resp_set_hdr(req, "X-Log-Reset", since == 0 || result.reset ? "1" : "0");
    return httpd_resp_send(req, result.text.data(), result.text.size());
}

esp_err_t WebServer::HandleGetNotifications(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    auto& history = NotificationHistory::GetInstance();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "capacity", history.GetCapacity());
    cJSON_AddNumberToObject(json, "max_message_len", CONFIG_ANCS_MAX_MESSAGE_LEN);
    cJSON_AddItemToObject(json, "items", history.ToCJson());
    return SendJson(req, json);
}

esp_err_t WebServer::HandleClearNotifications(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    NotificationHistory::GetInstance().Clear();
    return SendOk(req);
}

// {"id", "action": "positive" | "negative"}. The iPhone then removes or
// updates the notification, which the page learns from /api/events.
esp_err_t WebServer::HandleNotificationAction(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    cJSON* id = json ? cJSON_GetObjectItem(json, "id") : nullptr;
    const char* action = json ? GetJsonString(json, "action") : nullptr;
    bool positive = action != nullptr && strcmp(action, "positive") == 0;
    bool negative = action != nullptr && strcmp(action, "negative") == 0;
    if (!cJSON_IsNumber(id) || (!positive && !negative)) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    uint32_t record_id = (uint32_t)id->valuedouble;
    cJSON_Delete(json);

    auto notification = NotificationHistory::GetInstance().GetActive(record_id);
    if (!notification) {
        return SendError(req, "409 Conflict", "通知已在 iPhone 上清除，或 iPhone 已断开");
    }
    if (!(notification->event_flags & (positive ? kAncsFlagPositiveAction : kAncsFlagNegativeAction))) {
        return SendError(req, "400 Bad Request", "这条通知没有该操作");
    }
    if (!BleManager::GetInstance().PerformAction(notification->conn_handle, notification->uid,
                                                 positive ? kAncsActionPositive : kAncsActionNegative)) {
        return SendError(req, "409 Conflict", "iPhone 已断开");
    }
    ESP_LOGI(TAG, "%s action requested on uid=%lu from %s", positive ? "Positive" : "Negative",
             (unsigned long)notification->uid, notification->peer_name.c_str());
    return SendOk(req);
}

// Before the WebSocket upgrade. Any site can open a WebSocket to the device
// (there is no CORS for them), so check that the admin page itself asks.
esp_err_t WebServer::HandleEventsHandshake(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_FAIL;
    }
    std::string origin = GetHeader(req, "Origin");
    size_t scheme_end = origin.find("://");
    if (scheme_end == std::string::npos || origin.substr(scheme_end + 3) != GetHeader(req, "Host")) {
        ESP_LOGW(TAG, "Rejected WebSocket from origin \"%s\"", origin.c_str());
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Frames from the page: only keepalives, which also keep this socket from
// being closed as the least recently used one
esp_err_t WebServer::HandleEvents(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;  // Handshake, on ESP-IDF versions that pass it here
    }
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0) {
        return err;
    }
    uint8_t payload[16];
    if (frame.len > sizeof(payload)) {
        return ESP_FAIL;
    }
    frame.payload = payload;
    return httpd_ws_recv_frame(req, &frame, frame.len);
}

void WebServer::NotifyNotificationsChanged() {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (events_server_ == nullptr || notifications_changed_queued_.exchange(true)) {
        return;
    }
    if (httpd_queue_work(events_server_, SendNotificationsChanged, events_server_) != ESP_OK) {
        notifications_changed_queued_ = false;
    }
}

// Runs in the server task
void WebServer::SendNotificationsChanged(void* arg) {
    auto server = static_cast<httpd_handle_t>(arg);
    // Cleared first, so a change from now on queues another signal
    GetInstance().notifications_changed_queued_ = false;

    static const char kMessage[] = "{\"type\":\"notifications\"}";
    httpd_ws_frame_t frame = {};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t*)kMessage;
    frame.len = sizeof(kMessage) - 1;

    int fds[kMaxOpenSockets];
    size_t count = kMaxOpenSockets;
    if (httpd_get_client_list(server, &count, fds) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (httpd_ws_get_fd_info(server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET &&
            httpd_ws_send_frame_async(server, fds[i], &frame) != ESP_OK) {
            // The page reconnects
            httpd_sess_trigger_close(server, fds[i]);
        }
    }
}

esp_err_t WebServer::HandleGetConfig(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "history_size", NotificationHistory::GetInstance().GetCapacity());
    cJSON_AddNumberToObject(json, "history_size_min", NotificationHistory::kMinCapacity);
    cJSON_AddNumberToObject(json, "history_size_max", NotificationHistory::kMaxCapacity);
    cJSON_AddBoolToObject(json, "history_persistent", NotificationHistory::GetInstance().IsPersistent());
    cJSON_AddNumberToObject(json, "ble_max_connections", BleManager::GetInstance().GetMaxConnections());
    cJSON_AddNumberToObject(json, "ble_max_connections_limit", BleManager::kMaxConnections);
    cJSON_AddBoolToObject(json, "has_password", self->HasPassword());
    return SendJson(req, json);
}

// Every field is optional: {"history_size", "ble_max_connections", "password"}.
// The webhook targets have their own endpoint, /api/webhooks.
esp_err_t WebServer::HandleSetConfig(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    if (json == nullptr) {
        return SendError(req, "400 Bad Request", "请求格式错误");
    }

    cJSON* history_size = cJSON_GetObjectItem(json, "history_size");
    cJSON* ble_max_connections = cJSON_GetObjectItem(json, "ble_max_connections");
    cJSON* password = cJSON_GetObjectItem(json, "password");

    // Validate everything before saving anything
    std::string error;
    if (history_size != nullptr && !cJSON_IsNumber(history_size)) {
        error = "保存条数必须是数字";
    }
    if (ble_max_connections != nullptr &&
        (!cJSON_IsNumber(ble_max_connections) || ble_max_connections->valueint < 1 ||
         ble_max_connections->valueint > BleManager::kMaxConnections)) {
        error = "蓝牙连接数必须在 1 到 " + std::to_string(BleManager::kMaxConnections) + " 之间";
    }
    if (password != nullptr && !cJSON_IsString(password)) {
        error = "密码格式错误";
    }
    if (!error.empty()) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", error);
    }

    if (history_size != nullptr) {
        NotificationHistory::GetInstance().SetCapacity(history_size->valueint);
    }
    if (ble_max_connections != nullptr) {
        BleManager::GetInstance().SetMaxConnections(ble_max_connections->valueint);
    }
    if (password != nullptr) {
        self->SetPassword(password->valuestring);
        ESP_LOGI(TAG, "Admin password %s", password->valuestring[0] ? "changed" : "removed");
    }
    cJSON_Delete(json);
    return SendOk(req);
}

// {"max", "items": [target or null per slot]}. Secrets are never sent back,
// only whether one is set.
esp_err_t WebServer::HandleGetWebhooks(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "max", WebhookClient::kMaxTargets);
    cJSON_AddStringToObject(json, "default_template", kDefaultWebhookTemplate);
    cJSON* items = cJSON_AddArrayToObject(json, "items");
    for (auto& target : WebhookClient::GetInstance().GetTargets()) {
        cJSON_AddItemToArray(items, target.url.empty() ? cJSON_CreateNull() : target.ToCJson(false));
    }
    return SendJson(req, json);
}

// "slot" from the request body, -1 if missing
static int GetSlot(cJSON* json) {
    cJSON* slot = json ? cJSON_GetObjectItem(json, "slot") : nullptr;
    if (!cJSON_IsNumber(slot) || slot->valueint < 0 || slot->valueint >= WebhookClient::kMaxTargets) {
        return -1;
    }
    return slot->valueint;
}

// {"slot", "name", "enabled", "format", "url", "secret", "template"}; missing
// fields keep their value (the secret is cleared when the format changes)
esp_err_t WebServer::HandleSetWebhook(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    int slot = GetSlot(json);
    if (slot < 0) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    std::string error = WebhookClient::GetInstance().UpdateTarget(slot, json);
    cJSON_Delete(json);
    return SendResult(req, error);
}

// {"slot"}
esp_err_t WebServer::HandleRemoveWebhook(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    int slot = GetSlot(json);
    cJSON_Delete(json);
    if (slot < 0) {
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    WebhookClient::GetInstance().RemoveTarget(slot);
    return SendOk(req);
}

// {"slot"}: push a sample notification to the saved target
esp_err_t WebServer::HandleWebhookTest(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    int slot = GetSlot(json);
    cJSON_Delete(json);
    if (slot < 0) {
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    auto result = WebhookClient::GetInstance().SendTest(slot);
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", result.success);
    cJSON_AddNumberToObject(response, "status", result.status);
    cJSON_AddStringToObject(response, "error", result.error.c_str());
    return SendJson(req, response);
}

esp_err_t WebServer::HandleGetFilters(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    return SendJson(req, NotificationFilter::GetInstance().ToCJson());
}

// {"default_action", "rules": [...]}, replaces all rules (see NotificationFilter)
esp_err_t WebServer::HandleSetFilters(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    if (req->content_len > kMaxJsonBodySize) {
        return SendError(req, "400 Bad Request", "规则内容太多，请减少规则或关键词");
    }
    cJSON* json = ReadJsonBody(req);
    if (json == nullptr) {
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    std::string error = NotificationFilter::GetInstance().Set(json);
    cJSON_Delete(json);
    return SendResult(req, error);
}

esp_err_t WebServer::HandleReboot(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    ScheduleRestart();
    return SendOk(req);
}

esp_err_t WebServer::HandleClearBonds(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    BleManager::GetInstance().ClearBonds();
    return SendOk(req);
}

esp_err_t WebServer::HandleWifiConfigMode(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    // Entering config mode stops this server, which cannot be done from one of
    // its own handlers: let the main task do it once the response is out
    static esp_timer_handle_t timer = nullptr;
    RunLater(timer, [](void*) {
        Application::GetInstance().Schedule([]() {
            WifiService::GetInstance().EnterWifiConfigMode();
        });
    }, 500);
    return SendOk(req);
}

// Passwords are never sent back, only whether one is set
esp_err_t WebServer::HandleGetWifi(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    auto& wifi = WifiService::GetInstance();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "connected", wifi.IsConnected() ? wifi.GetSsid().c_str() : "");
    cJSON_AddNumberToObject(json, "max", WifiService::kMaxSavedNetworks);
    cJSON* items = cJSON_AddArrayToObject(json, "items");
    for (auto& network : wifi.GetSavedNetworks()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
        cJSON_AddBoolToObject(item, "has_password", network.has_password);
        cJSON_AddNumberToObject(item, "channel", network.channel);
        cJSON_AddItemToArray(items, item);
    }
    return SendJson(req, json);
}

// {"ssid", "password"}; an empty or missing password means an open network
esp_err_t WebServer::HandleAddWifi(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    const char* ssid = json ? GetJsonString(json, "ssid") : nullptr;
    if (ssid == nullptr) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    const char* password = GetJsonString(json, "password");
    auto error = WifiService::GetInstance().AddNetwork(ssid, password ? password : "");
    cJSON_Delete(json);
    return SendResult(req, error);
}

// {"ssid", "new_ssid", "password"}; a missing new_ssid / password keeps the current one
esp_err_t WebServer::HandleUpdateWifi(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    const char* ssid = json ? GetJsonString(json, "ssid") : nullptr;
    if (ssid == nullptr) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    const char* new_ssid = GetJsonString(json, "new_ssid");
    const char* password = GetJsonString(json, "password");
    std::string password_str = password ? password : "";
    auto error = WifiService::GetInstance().UpdateNetwork(ssid, new_ssid ? new_ssid : ssid,
                                                          password ? &password_str : nullptr);
    cJSON_Delete(json);
    return SendResult(req, error);
}

// {"ssid"}
esp_err_t WebServer::HandleRemoveWifi(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    const char* ssid = json ? GetJsonString(json, "ssid") : nullptr;
    if (ssid == nullptr) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    auto error = WifiService::GetInstance().RemoveNetwork(ssid);
    cJSON_Delete(json);
    return SendResult(req, error);
}

// The request body is the raw application image (build/ancs-esp32.bin)
esp_err_t WebServer::HandleOta(httpd_req_t* req) {
    if (!Self(req)->CheckAuth(req)) {
        return ESP_OK;
    }
    if (!HasContentType(req, "application/octet-stream")) {
        return SendError(req, "400 Bad Request", "Content-Type 必须为 application/octet-stream");
    }
    std::string error;
    if (!Self(req)->ReceiveFirmware(req, error)) {
        ESP_LOGE(TAG, "Firmware update failed: %s", error.c_str());
        return SendError(req, "400 Bad Request", error);
    }
    ScheduleRestart();
    return SendOk(req);
}

static bool CheckImageHeader(const char* data, std::string& error) {
    auto* header = reinterpret_cast<const esp_image_header_t*>(data);
    if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
        error = "不是有效的固件文件";
        return false;
    }
    if (header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        error = "固件的芯片型号与本设备 (" CONFIG_IDF_TARGET ") 不一致";
        return false;
    }
    auto* desc = reinterpret_cast<const esp_app_desc_t*>(
        data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        // e.g. bootloader.bin, which has the same image header
        error = "不是应用程序固件，请上传 build/" + std::string(esp_app_get_description()->project_name) + ".bin";
        return false;
    }
    auto* running = esp_app_get_description();
    if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
        error = "固件项目名 \"" + std::string(desc->project_name, strnlen(desc->project_name, sizeof(desc->project_name))) +
                "\" 与本设备 \"" + running->project_name + "\" 不一致";
        return false;
    }
    ESP_LOGI(TAG, "New firmware: %s v%.32s (running v%s)", desc->project_name, desc->version, running->version);
    return true;
}

bool WebServer::ReceiveFirmware(httpd_req_t* req, std::string& error) {
    const size_t total = req->content_len;
    const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
    if (partition == nullptr) {
        error = "分区表中没有 OTA 分区";
        return false;
    }
    if (total < kMinImageSize) {
        error = "文件太小，不是有效的固件";
        return false;
    }
    if (total > partition->size) {
        error = "固件大小超过 OTA 分区 (" + std::to_string(partition->size / 1024) + " KB)";
        return false;
    }
    ESP_LOGI(TAG, "Receiving %u bytes into partition %s", (unsigned)total, partition->label);

    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kOtaBufferSize]);
    if (!buffer) {
        error = "内存不足";
        return false;
    }

    esp_ota_handle_t handle = 0;
    bool begun = false;
    size_t received = 0;
    int last_percent = 0;
    esp_err_t err = ESP_OK;
    while (received < total) {
        size_t length = std::min(kOtaBufferSize, total - received);
        if (!RecvExact(req, buffer.get(), length)) {
            error = "接收数据失败";
            break;
        }

        // The first block always holds the whole header (kOtaBufferSize > kMinImageSize)
        if (!begun) {
            if (!CheckImageHeader(buffer.get(), error)) {
                break;
            }
            // Sectors are erased as they are written instead of all up front
            err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                error = std::string("esp_ota_begin 失败: ") + esp_err_to_name(err);
                break;
            }
            begun = true;
        }

        err = esp_ota_write(handle, buffer.get(), length);
        if (err != ESP_OK) {
            error = std::string("写入 Flash 失败: ") + esp_err_to_name(err);
            break;
        }
        received += length;

        int percent = received * 100 / total;
        if (percent / 10 != last_percent / 10) {
            ESP_LOGI(TAG, "Firmware upload %d%%", percent);
        }
        last_percent = percent;
    }

    if (received < total) {
        if (begun) {
            esp_ota_abort(handle);
        }
        return false;
    }

    // Verifies the image (checksum / SHA-256)
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "固件校验失败，文件可能已损坏"
                                                   : std::string("esp_ota_end 失败: ") + esp_err_to_name(err);
        return false;
    }
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        error = std::string("设置启动分区失败: ") + esp_err_to_name(err);
        return false;
    }
    ESP_LOGI(TAG, "Firmware written to %s, restarting", partition->label);
    return true;
}
