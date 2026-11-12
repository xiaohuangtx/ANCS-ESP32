#include "webhook_client.h"
#include "notification.h"
#include "settings.h"

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

void WebhookClient::SetResultCallback(std::function<void(uint32_t id, UploadState state)> callback) {
    result_callback_ = std::move(callback);
}

void WebhookClient::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Settings settings("webhook");
        url_ = settings.GetString("url", CONFIG_WEBHOOK_URL);
        auth_token_ = settings.GetString("auth", CONFIG_WEBHOOK_AUTH_TOKEN);
    }

    if (url_.empty()) {
        ESP_LOGI(TAG, "Webhook disabled (no URL configured)");
    } else {
        ESP_LOGI(TAG, "Webhook URL: %s", url_.c_str());
    }

    queue_ = xQueueCreate(CONFIG_WEBHOOK_QUEUE_SIZE, sizeof(Request*));
    event_group_ = xEventGroupCreate();
    xTaskCreate([](void* arg) {
        static_cast<WebhookClient*>(arg)->Run();
        vTaskDelete(NULL);
    }, "webhook", kTaskStackSize, this, 3, nullptr);
}

bool WebhookClient::IsEnabled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !url_.empty();
}

void WebhookClient::SetUrl(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
    Settings settings("webhook", true);
    settings.SetString("url", url);
}

void WebhookClient::SetAuthToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auth_token_ = token;
    Settings settings("webhook", true);
    settings.SetString("auth", token);
}

std::string WebhookClient::GetUrl() {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

std::string WebhookClient::GetAuthToken() {
    std::lock_guard<std::mutex> lock(mutex_);
    return auth_token_;
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

void WebhookClient::Post(uint32_t id, const std::string& json) {
    UploadState result = UploadState::kSkipped;
    if (queue_ != nullptr && IsEnabled()) {
        auto* item = new Request{id, json};
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

int WebhookClient::SendTest() {
    AncsNotification notification;
    notification.has_attributes = true;
    notification.app_id = "com.example.ancs-esp32";
    notification.app_name = "ANCS-ESP32";
    notification.title = "Webhook test";
    notification.message = "This is a test notification from the ANCS-ESP32 admin page";
    notification.message_size = notification.message.size();

    // Same shape as a real notification, but marked so receivers can tell
    cJSON* root = notification.ToCJson();
    cJSON_ReplaceItemInObject(root, "event", cJSON_CreateString("test"));
    char* str = cJSON_PrintUnformatted(root);
    std::string body(str ? str : "{}");
    cJSON_free(str);
    cJSON_Delete(root);

    std::string url;
    std::string auth_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        auth_token = auth_token_;
    }
    if (url.empty()) {
        return -1;
    }
    // A connection of its own: this runs in the HTTP server task, and the
    // webhook task's connection may be busy or still point to an old URL
    esp_http_client_handle_t client = CreateClient(url);
    if (client == nullptr) {
        return -1;
    }
    int status = -1;
    esp_err_t err = Perform(client, body, auth_token, &status);
    GetResult(err, status);
    esp_http_client_cleanup(client);
    return err == ESP_OK ? status : -1;
}

void WebhookClient::Run() {
    while (true) {
        Request* item = nullptr;
        TickType_t wait = client_ != nullptr ? pdMS_TO_TICKS(kIdleCloseMs) : portMAX_DELAY;
        if (xQueueReceive(queue_, &item, wait) != pdTRUE) {
            CloseConnection();  // Idle
            continue;
        }

        int attempt = 1;
        UploadState result;
        while (true) {
            // Hold the payload until Wi-Fi is up, also between retries
            if (!(xEventGroupGetBits(event_group_) & WEBHOOK_EVENT_NETWORK_READY)) {
                CloseConnection();  // The socket did not survive
                xEventGroupWaitBits(event_group_, WEBHOOK_EVENT_NETWORK_READY, pdFALSE, pdTRUE, portMAX_DELAY);
            }
            result = Send(item->body);
            if (result != UploadState::kPending) {
                break;
            }
            if (!(xEventGroupGetBits(event_group_) & WEBHOOK_EVENT_NETWORK_READY)) {
                ESP_LOGW(TAG, "Wi-Fi lost, sending again once it is back");
                continue;  // Not the server's fault, does not count as an attempt
            }
            if (attempt == kMaxAttempts) {
                ESP_LOGE(TAG, "Giving up after %d attempts", kMaxAttempts);
                result = UploadState::kFailed;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            attempt++;
        }
        if (result_callback_) {
            result_callback_(item->id, result);
        }
        delete item;
    }
}

UploadState WebhookClient::Send(const std::string& body) {
    std::string url;
    std::string auth_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        auth_token = auth_token_;
    }
    if (url.empty()) {
        CloseConnection();
        return UploadState::kSkipped;  // Disabled meanwhile
    }
    if (client_ != nullptr && url != client_url_) {
        CloseConnection();
    }

    // The server may have closed a kept-alive connection in the meantime
    // (many do after 5-75 s idle), which only shows when it is used
    bool reused = client_ != nullptr;
    if (client_ == nullptr) {
        client_ = CreateClient(url);
        if (client_ == nullptr) {
            return UploadState::kPending;
        }
        client_url_ = url;
    }
    int status = 0;
    esp_err_t err = Perform(client_, body, auth_token, &status);
    if (err != ESP_OK && reused) {
        ESP_LOGI(TAG, "Kept-alive connection failed (%s), reconnecting", esp_err_to_name(err));
        err = Perform(client_, body, auth_token, &status);
    }
    return GetResult(err, status);
}

void WebhookClient::CloseConnection() {
    if (client_ != nullptr) {
        esp_http_client_cleanup(client_);
        client_ = nullptr;
        client_url_.clear();
    }
}

esp_http_client_handle_t WebhookClient::CreateClient(const std::string& url) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = CONFIG_WEBHOOK_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return nullptr;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ANCS-ESP32/" PROJECT_VERSION);
    return client;
}

esp_err_t WebhookClient::Perform(esp_http_client_handle_t client, const std::string& body,
                                 const std::string& auth_token, int* status_code) {
    if (auth_token.empty()) {
        esp_http_client_delete_header(client, "Authorization");
    } else {
        esp_http_client_set_header(client, "Authorization", auth_token.c_str());
    }
    esp_http_client_set_post_field(client, body.data(), body.size());

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

UploadState WebhookClient::GetResult(esp_err_t err, int status_code) {
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
        return UploadState::kPending;
    }
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "POST returned HTTP %d", status_code);
        // Client errors will not succeed on retry
        return status_code >= 400 && status_code < 500 ? UploadState::kFailed : UploadState::kPending;
    }
    ESP_LOGI(TAG, "POST ok, HTTP %d", status_code);
    return UploadState::kDelivered;
}
