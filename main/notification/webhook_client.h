#ifndef WEBHOOK_CLIENT_H
#define WEBHOOK_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_http_client.h>

#include <string>
#include <mutex>
#include <functional>

#include "notification.h"

/**
 * POSTs notification JSON to an HTTP(S) endpoint.
 *
 * The URL and Authorization header are read from NVS (namespace "webhook",
 * keys "url" / "auth"), falling back to CONFIG_WEBHOOK_URL / CONFIG_WEBHOOK_AUTH_TOKEN.
 * An empty URL disables the webhook. Requests are sent from a dedicated task,
 * queued while Wi-Fi is down, and retried a few times on failure. The final
 * UploadState of each request is reported through the result callback.
 *
 * The task keeps its HTTP(S) connection open between requests (keep-alive),
 * so a burst of notifications costs one TLS handshake instead of one each. The
 * connection is closed after kIdleCloseMs without requests to free the TLS
 * memory, and when the URL changes or Wi-Fi goes down.
 */
class WebhookClient {
public:
    static WebhookClient& GetInstance() {
        static WebhookClient instance;
        return instance;
    }
    WebhookClient(const WebhookClient&) = delete;
    WebhookClient& operator=(const WebhookClient&) = delete;

    // Called from the webhook task, or from Post() when a request is dropped.
    // Set before Start().
    void SetResultCallback(std::function<void(uint32_t id, UploadState state)> callback);

    void Start();
    bool IsEnabled();

    // Thread-safe, persisted to NVS. An empty URL disables the webhook.
    void SetUrl(const std::string& url);
    void SetAuthToken(const std::string& token);
    std::string GetUrl();
    std::string GetAuthToken();

    // Called on Wi-Fi connect / disconnect
    void SetNetworkReady(bool ready);

    // Thread-safe, non-blocking. Drops the payload if the queue is full.
    // The id is passed back to the result callback.
    void Post(uint32_t id, const std::string& json);

    // Blocking: POST a sample notification right away (bypassing the queue).
    // Returns the HTTP status code, or -1 on a network error / no URL.
    int SendTest();

private:
    WebhookClient() = default;

    struct Request {
        uint32_t id;
        std::string body;
    };

    void Run();
    // Sends over the task's kept-alive connection. kPending: worth retrying.
    UploadState Send(const std::string& body);
    void CloseConnection();

    static esp_http_client_handle_t CreateClient(const std::string& url);
    // ESP_OK once the server answered, *status_code is set then. On an error
    // the connection is closed, so the next request starts over cleanly.
    static esp_err_t Perform(esp_http_client_handle_t client, const std::string& body,
                             const std::string& auth_token, int* status_code);
    // kDelivered (2xx), kFailed (4xx, will not succeed on retry) or kPending
    static UploadState GetResult(esp_err_t err, int status_code);

    std::mutex mutex_;
    std::string url_;
    std::string auth_token_;
    QueueHandle_t queue_ = nullptr;
    std::function<void(uint32_t id, UploadState state)> result_callback_;
    EventGroupHandle_t event_group_ = nullptr;

    // Only used by the webhook task
    esp_http_client_handle_t client_ = nullptr;
    std::string client_url_;
};

#endif // WEBHOOK_CLIENT_H
