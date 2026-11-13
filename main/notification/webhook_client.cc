#include "webhook_client.h"
#include "notification.h"
#include "settings.h"
#include "time_sync.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>

#define TAG "WebhookClient"

#define WEBHOOK_EVENT_NETWORK_READY (1 << 0)

static constexpr int kMaxAttempts = 3;
static constexpr int kTaskStackSize = 6144;
// A kept-alive connection is closed after this long without requests
static constexpr int kIdleCloseMs = 30000;
// Enough for the error message in a push service's response
static constexpr size_t kMaxResponseSize = 1024;
// How long a signed request waits for the clock after a reboot (SNTP / iPhone)
static constexpr int kClockWaitMs = 30000;

static std::string SlotKey(int slot) {
    return "t" + std::to_string(slot);
}

// "scheme://host:port" part of a URL: requests to the same one can share a connection
static std::string Origin(const std::string& url) {
    size_t start = url.find("://");
    size_t end = start == std::string::npos ? std::string::npos : url.find_first_of("/?#", start + 3);
    std::string origin = url.substr(0, end);
    for (auto& c : origin) {
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }
    return origin;
}

void WebhookClient::SetResultCallback(std::function<void(uint32_t id, UploadState state)> callback) {
    result_callback_ = std::move(callback);
}

void WebhookClient::Start() {
    LoadTargets();

    queue_ = xQueueCreate(CONFIG_WEBHOOK_QUEUE_SIZE, sizeof(Request*));
    event_group_ = xEventGroupCreate();
    xTaskCreate([](void* arg) {
        static_cast<WebhookClient*>(arg)->Run();
        vTaskDelete(NULL);
    }, "webhook", kTaskStackSize, this, 3, nullptr);
}

void WebhookClient::LoadTargets() {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings("webhook");
    bool migrated = false;
    for (int slot = 0; slot < kMaxTargets; slot++) {
        migrated |= settings.HasKey(SlotKey(slot));
        std::string stored = settings.GetString(SlotKey(slot));
        cJSON* json = stored.empty() ? nullptr : cJSON_Parse(stored.c_str());
        if (json != nullptr) {
            targets_[slot].Update(json);
            cJSON_Delete(json);
        }
    }

    if (!migrated) {
        // Settings of the single webhook from earlier versions
        WebhookTarget& target = targets_[0];
        target.url = settings.GetString("url", CONFIG_WEBHOOK_URL);
        target.secret = settings.GetString("auth", CONFIG_WEBHOOK_AUTH_TOKEN);
        if (!target.url.empty()) {
            target.name = "Webhook";
        }
    }

    int active = 0;
    for (int slot = 0; slot < kMaxTargets; slot++) {
        auto& target = targets_[slot];
        if (target.IsActive()) {
            active++;
            ESP_LOGI(TAG, "Target %d \"%s\" (%s): %s", slot + 1, target.name.c_str(),
                     WebhookFormatName(target.format), target.url.c_str());
        }
    }
    if (active == 0) {
        ESP_LOGI(TAG, "Webhook disabled (no target configured)");
    }
}

static std::string Serialize(const WebhookTarget& target) {
    if (target.url.empty()) {
        return "";
    }
    cJSON* json = target.ToCJson(true);
    char* str = cJSON_PrintUnformatted(json);
    std::string stored = str ? str : "";
    cJSON_free(str);
    cJSON_Delete(json);
    return stored;
}

// Caller holds mutex_. The other slots are written too the first time, so the
// single webhook settings of earlier versions are not picked up again.
void WebhookClient::SaveTarget(int slot) {
    Settings settings("webhook", true);
    for (int i = 0; i < kMaxTargets; i++) {
        if (i == slot || !settings.HasKey(SlotKey(i))) {
            settings.SetString(SlotKey(i), Serialize(targets_[i]));
        }
    }
}

bool WebhookClient::IsEnabled() {
    return ResolveTargets(0) != 0;
}

uint32_t WebhookClient::ResolveTargets(uint32_t wanted) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t active = 0;
    for (int slot = 0; slot < kMaxTargets; slot++) {
        if (targets_[slot].IsActive()) {
            active |= 1u << slot;
        }
    }
    return wanted == 0 ? active : active & wanted;
}

std::array<WebhookTarget, WebhookClient::kMaxTargets> WebhookClient::GetTargets() {
    std::lock_guard<std::mutex> lock(mutex_);
    return targets_;
}

std::string WebhookClient::UpdateTarget(int slot, cJSON* json) {
    if (slot < 0 || slot >= kMaxTargets) {
        return "推送目标编号错误";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    WebhookTarget target = targets_[slot];
    std::string error = target.Update(json);
    if (error.empty()) {
        error = target.Validate();
    }
    if (!error.empty()) {
        return error;
    }
    if (target.name.empty()) {
        target.name = "推送 " + std::to_string(slot + 1);
    }
    targets_[slot] = target;
    SaveTarget(slot);
    ESP_LOGI(TAG, "Target %d \"%s\" (%s) saved: %s, %s", slot + 1, target.name.c_str(), WebhookFormatName(target.format),
             target.url.c_str(), target.enabled ? "enabled" : "disabled");
    return "";
}

void WebhookClient::RemoveTarget(int slot) {
    if (slot < 0 || slot >= kMaxTargets) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    targets_[slot] = WebhookTarget();
    SaveTarget(slot);
    ESP_LOGI(TAG, "Target %d removed", slot + 1);
}

void WebhookClient::SetNetworkReady(bool ready) {
    if (event_group_ == nullptr) {
        return;
    }
    if (ready) {
        xEventGroupSetBits(event_group_, WEBHOOK_EVENT_NETWORK_READY);
    } else {
        xEventGroupClearBits(event_group_, WEBHOOK_EVENT_NETWORK_READY);
    }
}

void WebhookClient::Post(uint32_t id, const AncsNotification& notification, uint32_t targets) {
    UploadState result = UploadState::kSkipped;
    if (queue_ != nullptr && targets != 0) {
        auto* item = new Request{id, notification, targets};
        if (xQueueSend(queue_, &item, 0) == pdTRUE) {
            return;
        }
        ESP_LOGW(TAG, "Queue full, notification dropped");
        delete item;
        result = UploadState::kFailed;
    }
    if (result_callback_) {
        result_callback_(id, result);
    }
}

WebhookClient::TestResult WebhookClient::SendTest(int slot) {
    if (slot < 0 || slot >= kMaxTargets) {
        return {false, -1, "推送目标编号错误"};
    }
    WebhookTarget target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = targets_[slot];
    }
    if (target.url.empty()) {
        return {false, -1, "未配置 URL"};
    }

    WebhookRequest request;
    std::string error;
    if (BuildWebhookRequest(target, MakeSampleNotification(), true, &request, &error) != BuildResult::kOk) {
        return {false, -1, error};
    }
    // A connection of its own: this runs in the HTTP server task, and the
    // webhook task's connection may be busy
    std::string response;
    esp_http_client_handle_t client = CreateClient(request.url, &response);
    if (client == nullptr) {
        return {false, -1, "创建 HTTP 客户端失败"};
    }
    int status = -1;
    esp_err_t err = Perform(client, request, &response, &status);
    UploadState result = GetResult(target, err, status, response, &error);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return {false, -1, std::string("连接失败：") + esp_err_to_name(err)};
    }
    return {result == UploadState::kDelivered, status, error};
}

void WebhookClient::Run() {
    while (true) {
        Request* item = nullptr;
        TickType_t wait = client_ != nullptr ? pdMS_TO_TICKS(kIdleCloseMs) : portMAX_DELAY;
        if (xQueueReceive(queue_, &item, wait) != pdTRUE) {
            CloseConnection();  // Idle
            continue;
        }

        bool failed = false;
        bool delivered = false;
        for (int slot = 0; slot < kMaxTargets; slot++) {
            if (item->targets & (1u << slot)) {
                UploadState state = SendToTarget(*item, slot);
                failed |= state == UploadState::kFailed;
                delivered |= state == UploadState::kDelivered;
            }
        }
        UploadState result = failed ? UploadState::kFailed : delivered ? UploadState::kDelivered : UploadState::kSkipped;
        if (result_callback_) {
            result_callback_(item->id, result);
        }
        delete item;
    }
}

UploadState WebhookClient::SendToTarget(const Request& request, int slot) {
    int attempt = 1;
    bool waited_for_clock = false;
    while (true) {
        // Hold the notification until Wi-Fi is up, also between retries
        if (!(xEventGroupGetBits(event_group_) & WEBHOOK_EVENT_NETWORK_READY)) {
            CloseConnection();  // The socket did not survive
            xEventGroupWaitBits(event_group_, WEBHOOK_EVENT_NETWORK_READY, pdFALSE, pdTRUE, portMAX_DELAY);
        }

        // Read again on every attempt: the target may have been edited meanwhile
        WebhookTarget target;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target = targets_[slot];
        }
        if (!target.IsActive()) {
            return UploadState::kSkipped;  // Disabled or removed meanwhile
        }

        WebhookRequest http_request;
        std::string error;
        BuildResult build = BuildWebhookRequest(target, request.notification, false, &http_request, &error);
        if (build == BuildResult::kNeedClock && !waited_for_clock) {
            // Right after a reboot SNTP needs a moment
            ESP_LOGI(TAG, "Target %d: waiting for the clock to sign the request", slot + 1);
            waited_for_clock = true;
            TimeSync::GetInstance().WaitUntilSet(pdMS_TO_TICKS(kClockWaitMs));
            continue;
        }
        if (build != BuildResult::kOk) {
            ESP_LOGE(TAG, "Target %d: %s", slot + 1, error.c_str());
            return UploadState::kFailed;
        }

        UploadState result = Send(target, http_request);
        if (result != UploadState::kPending) {
            return result;
        }
        if (!(xEventGroupGetBits(event_group_) & WEBHOOK_EVENT_NETWORK_READY)) {
            ESP_LOGW(TAG, "Wi-Fi lost, sending again once it is back");
            continue;  // Not the server's fault, does not count as an attempt
        }
        if (attempt == kMaxAttempts) {
            ESP_LOGE(TAG, "Target %d: giving up after %d attempts", slot + 1, kMaxAttempts);
            return UploadState::kFailed;
        }
        vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
        attempt++;
    }
}

UploadState WebhookClient::Send(const WebhookTarget& target, const WebhookRequest& request) {
    std::string origin = Origin(request.url);
    if (client_ != nullptr && origin != client_origin_) {
        CloseConnection();
    }

    // The server may have closed a kept-alive connection in the meantime
    // (many do after 5-75 s idle), which only shows when it is used
    bool reused = client_ != nullptr;
    if (client_ == nullptr) {
        client_ = CreateClient(request.url, &response_);
        if (client_ == nullptr) {
            return UploadState::kPending;
        }
        client_origin_ = origin;
    } else if (esp_http_client_set_url(client_, request.url.c_str()) != ESP_OK) {
        CloseConnection();
        return UploadState::kPending;
    }
    int status = 0;
    esp_err_t err = Perform(client_, request, &response_, &status);
    if (err != ESP_OK && reused) {
        ESP_LOGI(TAG, "Kept-alive connection failed (%s), reconnecting", esp_err_to_name(err));
        err = Perform(client_, request, &response_, &status);
    }
    std::string error;
    return GetResult(target, err, status, response_, &error);
}

void WebhookClient::CloseConnection() {
    if (client_ != nullptr) {
        esp_http_client_cleanup(client_);
        client_ = nullptr;
        client_origin_.clear();
    }
}

esp_err_t WebhookClient::HandleHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<std::string*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && response != nullptr && event->data_len > 0) {
        size_t room = kMaxResponseSize - std::min(kMaxResponseSize, response->size());
        response->append(static_cast<const char*>(event->data), std::min<size_t>(room, event->data_len));
    }
    return ESP_OK;
}

esp_http_client_handle_t WebhookClient::CreateClient(const std::string& url, std::string* response) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = CONFIG_WEBHOOK_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HandleHttpEvent;
    config.user_data = response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return nullptr;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ANCS-ESP32/" PROJECT_VERSION);
    return client;
}

esp_err_t WebhookClient::Perform(esp_http_client_handle_t client, const WebhookRequest& request,
                                 std::string* response, int* status_code) {
    if (request.authorization.empty()) {
        esp_http_client_delete_header(client, "Authorization");
    } else {
        esp_http_client_set_header(client, "Authorization", request.authorization.c_str());
    }
    esp_http_client_set_post_field(client, request.body.data(), request.body.size());
    response->clear();

    // Reuses the open connection if there is one. After a complete response
    // the client keeps it, unless the server answered "Connection: close".
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        // The request may have stopped halfway; start the next one from scratch
        esp_http_client_close(client);
        return err;
    }
    *status_code = esp_http_client_get_status_code(client);
    return ESP_OK;
}

UploadState WebhookClient::GetResult(const WebhookTarget& target, esp_err_t err, int status_code,
                                     const std::string& response, std::string* error) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "\"%s\": POST failed: %s", target.name.c_str(), esp_err_to_name(err));
        *error = std::string("连接失败：") + esp_err_to_name(err);
        return UploadState::kPending;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "\"%s\": POST returned HTTP %d %.*s", target.name.c_str(), status_code,
                 (int)std::min<size_t>(response.size(), 200), response.c_str());
        *error = "服务器返回 HTTP " + std::to_string(status_code);
        if (!response.empty()) {
            *error += "：" + response.substr(0, 200);
        }
        // Client errors will not succeed on retry
        return status_code >= 400 && status_code < 500 ? UploadState::kFailed : UploadState::kPending;
    }
    std::string reported;
    if (!CheckWebhookResponse(target.format, response, &reported)) {
        ESP_LOGW(TAG, "\"%s\": HTTP %d, but the service reported an error: %s", target.name.c_str(), status_code,
                 reported.c_str());
        *error = "服务返回错误：" + reported;
        return UploadState::kFailed;
    }
    ESP_LOGI(TAG, "\"%s\": POST ok, HTTP %d", target.name.c_str(), status_code);
    return UploadState::kDelivered;
}
