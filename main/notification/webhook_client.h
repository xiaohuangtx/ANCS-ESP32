#ifndef WEBHOOK_CLIENT_H
#define WEBHOOK_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_http_client.h>

#include <array>
#include <string>
#include <mutex>
#include <functional>

#include "notification.h"
#include "webhook_target.h"

/**
 * Pushes notifications to up to kMaxTargets webhook targets (see
 * WebhookTarget for the formats: plain JSON, WeCom, DingTalk, Feishu, Bark...).
 *
 * The targets are stored in NVS (namespace "webhook", keys "t0".."t2", JSON).
 * Settings from before multiple targets (keys "url" / "auth", falling back to
 * CONFIG_WEBHOOK_URL / CONFIG_WEBHOOK_AUTH_TOKEN) become the first target.
 *
 * Requests are sent from a dedicated task, queued while Wi-Fi is down, and
 * retried a few times on failure, separately for each target. The combined
 * UploadState of each notification is reported through the result callback:
 * delivered once every target accepted it, failed if any target gave up.
 *
 * The task keeps its HTTP(S) connection open between requests to the same
 * server (keep-alive), so a burst of notifications costs one TLS handshake
 * instead of one each. The connection is closed after kIdleCloseMs without
 * requests to free the TLS memory, and when Wi-Fi goes down.
 */
class WebhookClient {
public:
    static constexpr int kMaxTargets = 3;

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
    // Whether any target is enabled and has a URL
    bool IsEnabled();

    // Thread-safe. Slots without a target have an empty URL.
    std::array<WebhookTarget, kMaxTargets> GetTargets();
    // Updates the slot with the fields present in json (see WebhookTarget::Update)
    // and persists it. Returns "" on success, otherwise an error message.
    std::string UpdateTarget(int slot, cJSON* json);
    void RemoveTarget(int slot);

    // The active targets among the wanted slots (bit mask, 0: all)
    uint32_t ResolveTargets(uint32_t wanted);

    // Called on Wi-Fi connect / disconnect
    void SetNetworkReady(bool ready);

    // Thread-safe, non-blocking. Drops the notification if the queue is full.
    // The id is passed back to the result callback. targets: bit mask from
    // ResolveTargets(); slots that are no longer active when the request is
    // sent are skipped.
    void Post(uint32_t id, const AncsNotification& notification, uint32_t targets);

    // Blocking: push a sample notification to one target right away
    // (bypassing the queue). status is the HTTP status code, -1 on a network
    // error; error explains a failure.
    struct TestResult {
        bool success;
        int status;
        std::string error;
    };
    TestResult SendTest(int slot);

private:
    WebhookClient() = default;

    struct Request {
        uint32_t id;
        AncsNotification notification;
        uint32_t targets;
    };

    void LoadTargets();
    void SaveTarget(int slot);
    void Run();
    UploadState SendToTarget(const Request& request, int slot);
    // Sends over the task's kept-alive connection. kPending: worth retrying.
    UploadState Send(const WebhookTarget& target, const WebhookRequest& request);
    void CloseConnection();

    static esp_http_client_handle_t CreateClient(const std::string& url, std::string* response);
    // ESP_OK once the server answered, *status_code is set then. On an error
    // the connection is closed, so the next request starts over cleanly.
    static esp_err_t Perform(esp_http_client_handle_t client, const WebhookRequest& request,
                             std::string* response, int* status_code);
    // kDelivered (2xx and the body does not report an error), kFailed (4xx or
    // an error in the body, will not succeed on retry) or kPending
    static UploadState GetResult(const WebhookTarget& target, esp_err_t err, int status_code,
                                 const std::string& response, std::string* error);
    static esp_err_t HandleHttpEvent(esp_http_client_event_t* event);

    std::mutex mutex_;
    std::array<WebhookTarget, kMaxTargets> targets_;
    QueueHandle_t queue_ = nullptr;
    std::function<void(uint32_t id, UploadState state)> result_callback_;
    EventGroupHandle_t event_group_ = nullptr;

    // Only used by the webhook task
    esp_http_client_handle_t client_ = nullptr;
    std::string client_origin_;  // scheme://host:port of the open connection
    std::string response_;
};

#endif // WEBHOOK_CLIENT_H
