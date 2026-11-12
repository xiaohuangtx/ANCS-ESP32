#include "web_server.h"
#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"
#include "notification_history.h"
#include "settings.h"
#include "http_utils.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <memory>

#define TAG "WebServer"

extern const char admin_html_start[] asm("_binary_admin_html_start");
extern const char admin_html_end[] asm("_binary_admin_html_end");

static constexpr size_t kOtaBufferSize = 4096;
// Image header + first segment header + app descriptor, enough to identify the image
static constexpr size_t kMinImageSize =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

static WebServer* Self(httpd_req_t* req) {
    return static_cast<WebServer*>(req->user_ctx);
}

static std::string Base64Encode(const std::string& in) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) n |= (uint8_t)in[i + 1] << 8;
        if (i + 2 < in.size()) n |= (uint8_t)in[i + 2];
        out += kTable[(n >> 18) & 63];
        out += kTable[(n >> 12) & 63];
        out += i + 1 < in.size() ? kTable[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kTable[n & 63] : '=';
    }
    return out;
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
    config.max_uri_handlers = 20;
    // Leave sockets for the webhook client (CONFIG_LWIP_MAX_SOCKETS is 10)
    config.max_open_sockets = 4;
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
        {"/api/notifications", HTTP_GET, HandleGetNotifications, this},
        {"/api/notifications", HTTP_DELETE, HandleClearNotifications, this},
        {"/api/config", HTTP_GET, HandleGetConfig, this},
        {"/api/config", HTTP_POST, HandleSetConfig, this},
        {"/api/webhook/test", HTTP_POST, HandleWebhookTest, this},
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

    ESP_LOGI(TAG, "Admin page: http://%s/%s", WifiService::GetInstance().GetIpAddress().c_str(),
             HasPassword() ? " (password protected)" : "");
    MarkFirmwareValid();
}

void WebServer::Stop() {
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_ == nullptr) {
        return;
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

// With DNS rebinding a hostile domain resolves to the device, so the browser
// treats the device as part of that site. Such requests carry the hostile
// domain in Host: only accept an IP address or the device's own hostname
// (e.g. "ancs-a1b2c3.lan" from the router's DNS).
static bool IsAllowedHost(const std::string& host) {
    if (host.empty() || host[0] == '[') {
        return true;  // HTTP/1.0 without Host, or an IPv6 literal
    }
    std::string name = host.substr(0, host.find(':'));
    esp_ip4_addr_t ip;
    if (esp_netif_str_to_ip4(name.c_str(), &ip) == ESP_OK) {
        return true;
    }
    std::string label = name.substr(0, name.find('.'));
    return strcasecmp(label.c_str(), WifiService::GetInstance().GetDeviceName().c_str()) == 0;
}

bool WebServer::CheckRequest(httpd_req_t* req) {
    std::string host = GetHeader(req, "Host");
    if (!IsAllowedHost(host)) {
        ESP_LOGW(TAG, "Rejected request for host \"%s\"", host.c_str());
        SendError(req, "403 Forbidden", "请使用设备的 IP 地址访问");
        return false;
    }
    // Browsers send Origin with every POST / PUT / DELETE; requests from other
    // tools usually have none. Content-Type alone would not cover the requests
    // without a body (reboot, clear bonds, ...).
    if (req->method != HTTP_GET) {
        std::string origin = GetHeader(req, "Origin");
        if (!origin.empty() && origin != "http://" + host) {
            ESP_LOGW(TAG, "Rejected cross-site request from \"%s\"", origin.c_str());
            SendError(req, "403 Forbidden", "不允许跨站请求");
            return false;
        }
    }

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
    if (!Self(req)->CheckRequest(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, admin_html_start, admin_html_end - admin_html_start - 1);
}

esp_err_t WebServer::HandleStatus(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
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
    cJSON_AddBoolToObject(json, "webhook_enabled", WebhookClient::GetInstance().IsEnabled());
    return SendJson(req, json);
}

esp_err_t WebServer::HandleGetNotifications(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
        return ESP_OK;
    }
    NotificationHistory::GetInstance().Clear();
    return SendOk(req);
}

esp_err_t WebServer::HandleGetConfig(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->CheckRequest(req)) {
        return ESP_OK;
    }
    auto& webhook = WebhookClient::GetInstance();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "webhook_url", webhook.GetUrl().c_str());
    cJSON_AddStringToObject(json, "webhook_auth", webhook.GetAuthToken().c_str());
    cJSON_AddNumberToObject(json, "history_size", NotificationHistory::GetInstance().GetCapacity());
    cJSON_AddNumberToObject(json, "history_size_min", NotificationHistory::kMinCapacity);
    cJSON_AddNumberToObject(json, "history_size_max", NotificationHistory::kMaxCapacity);
    cJSON_AddNumberToObject(json, "ble_max_connections", BleManager::GetInstance().GetMaxConnections());
    cJSON_AddNumberToObject(json, "ble_max_connections_limit", BleManager::kMaxConnections);
    cJSON_AddBoolToObject(json, "has_password", self->HasPassword());
    return SendJson(req, json);
}

// Every field is optional:
// {"webhook_url", "webhook_auth", "history_size", "ble_max_connections", "password"}
esp_err_t WebServer::HandleSetConfig(httpd_req_t* req) {
    auto* self = Self(req);
    if (!self->CheckRequest(req)) {
        return ESP_OK;
    }
    cJSON* json = ReadJsonBody(req);
    if (json == nullptr) {
        return SendError(req, "400 Bad Request", "请求格式错误");
    }

    cJSON* url = cJSON_GetObjectItem(json, "webhook_url");
    cJSON* auth = cJSON_GetObjectItem(json, "webhook_auth");
    cJSON* history_size = cJSON_GetObjectItem(json, "history_size");
    cJSON* ble_max_connections = cJSON_GetObjectItem(json, "ble_max_connections");
    cJSON* password = cJSON_GetObjectItem(json, "password");

    // Validate everything before saving anything
    std::string error;
    if (url != nullptr) {
        std::string value = cJSON_IsString(url) ? url->valuestring : "";
        if (!cJSON_IsString(url) ||
            (!value.empty() && value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0)) {
            error = "Webhook URL 必须以 http:// 或 https:// 开头";
        }
    }
    if (auth != nullptr && !cJSON_IsString(auth)) {
        error = "Authorization 格式错误";
    }
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

    auto& webhook = WebhookClient::GetInstance();
    if (url != nullptr) {
        webhook.SetUrl(url->valuestring);
        ESP_LOGI(TAG, "Webhook URL set to \"%s\"", url->valuestring);
    }
    if (auth != nullptr) {
        webhook.SetAuthToken(auth->valuestring);
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

esp_err_t WebServer::HandleWebhookTest(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
        return ESP_OK;
    }
    auto& webhook = WebhookClient::GetInstance();
    if (!webhook.IsEnabled()) {
        return SendError(req, "400 Bad Request", "未配置 Webhook URL");
    }
    int status = webhook.SendTest();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", status >= 200 && status < 300);
    cJSON_AddNumberToObject(json, "status", status);
    return SendJson(req, json);
}

esp_err_t WebServer::HandleReboot(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
        return ESP_OK;
    }
    ScheduleRestart();
    return SendOk(req);
}

esp_err_t WebServer::HandleClearBonds(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
        return ESP_OK;
    }
    BleManager::GetInstance().ClearBonds();
    return SendOk(req);
}

esp_err_t WebServer::HandleWifiConfigMode(httpd_req_t* req) {
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
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
    if (!Self(req)->CheckRequest(req)) {
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
