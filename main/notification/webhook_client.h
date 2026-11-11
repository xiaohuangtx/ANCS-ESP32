#ifndef WEBHOOK_CLIENT_H
#define WEBHOOK_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#include <string>
#include <mutex>

/**
 * POSTs notification JSON to an HTTP(S) endpoint.
 *
 * The URL and Authorization header are read from NVS (namespace "webhook",
 * keys "url" / "auth"), falling back to CONFIG_WEBHOOK_URL / CONFIG_WEBHOOK_AUTH_TOKEN.
 * An empty URL disables the webhook. Requests are sent from a dedicated task,
 * queued while Wi-Fi is down, and retried a few times on failure.
 */
class WebhookClient {
public:
    static WebhookClient& GetInstance() {
        static WebhookClient instance;
        return instance;
    }
    WebhookClient(const WebhookClient&) = delete;
    WebhookClient& operator=(const WebhookClient&) = delete;

    void Start();
    bool IsEnabled();

    // Thread-safe, persisted to NVS. An empty URL disables the webhook.
    void SetUrl(const std::string& url);
    void SetAuthToken(const std::string& token);

    // Called on Wi-Fi connect / disconnect
    void SetNetworkReady(bool ready);

    // Thread-safe, non-blocking. Drops the payload if the queue is full.
    void Post(const std::string& json);

private:
    WebhookClient() = default;

    void Run();
    bool Send(const std::string& body);

    std::mutex mutex_;
    std::string url_;
    std::string auth_token_;
    QueueHandle_t queue_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
};

#endif // WEBHOOK_CLIENT_H
