#ifndef WEBHOOK_CLIENT_H
#define WEBHOOK_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_http_client.h>

#include <string>
#include <mutex>

/**
 * POSTs notification JSON to an HTTP(S) endpoint.
 *
 * The URL and Authorization header are read from NVS (namespace "webhook",
 * keys "url" / "auth"), falling back to CONFIG_WEBHOOK_URL / CONFIG_WEBHOOK_AUTH_TOKEN.
 * An empty URL disables the webhook. Requests are sent from a dedicated task,
 * queued while Wi-Fi is down, and retried a few times on failure.
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
    void Post(const std::string& json);

    // Blocking: POST a sample notification right away (bypassing the queue).
    // Returns the HTTP status code, or -1 on a network error / no URL.
    int SendTest();

private:
    WebhookClient() = default;

    void Run();
    // Sends over the task's kept-alive connection. Returns true when done
    // (2xx, or a 4xx that will not succeed on retry).
    bool Send(const std::string& body);
    void CloseConnection();

    static esp_http_client_handle_t CreateClient(const std::string& url);
    // ESP_OK once the server answered, *status_code is set then. On an error
    // the connection is closed, so the next request starts over cleanly.
    static esp_err_t Perform(esp_http_client_handle_t client, const std::string& body,
                             const std::string& auth_token, int* status_code);
    static bool IsDone(esp_err_t err, int status_code);

    std::mutex mutex_;
    std::string url_;
    std::string auth_token_;
    QueueHandle_t queue_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;

    // Only used by the webhook task
    esp_http_client_handle_t client_ = nullptr;
    std::string client_url_;
};

#endif // WEBHOOK_CLIENT_H
