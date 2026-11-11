#include "webhook_client.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

#define TAG "WebhookClient"

#define WEBHOOK_EVENT_NETWORK_READY (1 << 0)

static constexpr int kMaxAttempts = 3;
static constexpr int kTaskStackSize = 6144;

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

    queue_ = xQueueCreate(CONFIG_WEBHOOK_QUEUE_SIZE, sizeof(std::string*));
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

void WebhookClient::Post(const std::string& json) {
    if (queue_ == nullptr || !IsEnabled()) {
        return;
    }
    auto* item = new std::string(json);
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, notification dropped");
        delete item;
    }
}

void WebhookClient::Run() {
    while (true) {
        std::string* item = nullptr;
        if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Hold the payload until Wi-Fi is up
        xEventGroupWaitBits(event_group_, WEBHOOK_EVENT_NETWORK_READY, pdFALSE, pdTRUE, portMAX_DELAY);

        for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
            if (Send(*item)) {
                break;
            }
            if (attempt == kMaxAttempts) {
                ESP_LOGE(TAG, "Giving up after %d attempts", kMaxAttempts);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            }
        }
        delete item;
    }
}

bool WebhookClient::Send(const std::string& body) {
    std::string url;
    std::string auth_token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        auth_token = auth_token_;
    }
    if (url.empty()) {
        return true;  // Disabled meanwhile, nothing to do
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = CONFIG_WEBHOOK_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "ANCS-ESP32/" PROJECT_VERSION);
    if (!auth_token.empty()) {
        esp_http_client_set_header(client, "Authorization", auth_token.c_str());
    }
    esp_http_client_set_post_field(client, body.data(), body.size());

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST returned HTTP %d", status);
        // Client errors will not succeed on retry
        return status >= 400 && status < 500;
    }
    ESP_LOGI(TAG, "POST ok, HTTP %d", status);
    return true;
}
