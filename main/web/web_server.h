#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>

#include <string>
#include <mutex>
#include <atomic>

/**
 * Admin web page on port 80, served while the station is connected:
 * device status, notification history, webhook targets, filter rules,
 * history settings,
 * saved Wi-Fi networks, firmware upload (OTA), reboot.
 *
 * If an admin password is set (NVS namespace "web", key "password", falling
 * back to CONFIG_WEB_ADMIN_PASSWORD), every request needs HTTP Basic auth
 * with user "admin".
 *
 * Requests must address the device by IP or hostname (against DNS rebinding),
 * and requests that change something must not come from another site (CSRF):
 * a page on the internet could otherwise redirect the webhook or flash firmware
 * through the browser of anyone on the LAN.
 *
 * The page keeps a WebSocket open on /api/events. It only carries change
 * signals ({"type": "notifications"}), the page then fetches the data itself.
 * The handshake is authorized by a token from /api/events/token instead of
 * Basic auth, which browsers do not reliably send on WebSockets.
 *
 * The provisioning hotspot runs its own server (ConfigPortal) on the same
 * port, so this one must be stopped before entering Wi-Fi config mode.
 */
class WebServer {
public:
    static WebServer& GetInstance() {
        static WebServer instance;
        return instance;
    }
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Thread-safe, but must not be called from a request handler
    void Start();
    void Stop();

    // Thread-safe: tell the open pages that the notification list changed.
    // Signals sent close together are merged into one.
    void NotifyNotificationsChanged();

    // In bytes (UTF-8). Longer passwords would not fit in the Authorization
    // header CheckAuth accepts, which would lock the admin out.
    static constexpr size_t kMaxPasswordLength = 64;

private:
    WebServer() = default;

    // Whether the request has the right Basic credentials (or no password is set)
    bool IsAuthorized(httpd_req_t* req);
    // Sends the 401 response and returns false if the password is wrong
    bool CheckAuth(httpd_req_t* req);
    // Whether the query has ?token= matching events_token_
    bool HasEventsToken(httpd_req_t* req);
    // Caller holds mutex_
    void RenewEventsToken();
    void SetPassword(const std::string& password);
    bool HasPassword();
    bool ReceiveFirmware(httpd_req_t* req, std::string& error);

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleLogs(httpd_req_t* req);
    static esp_err_t HandleGetNotifications(httpd_req_t* req);
    static esp_err_t HandleClearNotifications(httpd_req_t* req);
    static esp_err_t HandleNotificationAction(httpd_req_t* req);
    static esp_err_t HandleGetEventsToken(httpd_req_t* req);
    static esp_err_t HandleEventsHandshake(httpd_req_t* req);
    static esp_err_t HandleEvents(httpd_req_t* req);
    static void SendNotificationsChanged(void* arg);
    static esp_err_t HandleGetConfig(httpd_req_t* req);
    static esp_err_t HandleSetConfig(httpd_req_t* req);
    static esp_err_t HandleGetWebhooks(httpd_req_t* req);
    static esp_err_t HandleSetWebhook(httpd_req_t* req);
    static esp_err_t HandleRemoveWebhook(httpd_req_t* req);
    static esp_err_t HandleWebhookTest(httpd_req_t* req);
    static esp_err_t HandleGetFilters(httpd_req_t* req);
    static esp_err_t HandleSetFilters(httpd_req_t* req);
    static esp_err_t HandleOta(httpd_req_t* req);
    static esp_err_t HandleReboot(httpd_req_t* req);
    static esp_err_t HandleClearBonds(httpd_req_t* req);
    static esp_err_t HandleWifiConfigMode(httpd_req_t* req);
    static esp_err_t HandleGetWifi(httpd_req_t* req);
    static esp_err_t HandleAddWifi(httpd_req_t* req);
    static esp_err_t HandleUpdateWifi(httpd_req_t* req);
    static esp_err_t HandleRemoveWifi(httpd_req_t* req);

    std::mutex server_mutex_;   // Guards server_ (Start / Stop)
    httpd_handle_t server_ = nullptr;

    std::mutex mutex_;          // Guards password_ and events_token_ (used by handlers)
    std::string password_;
    // Lets the page open /api/events without Basic auth, see HandleGetEventsToken.
    // Random, renewed on every start and password change.
    std::string events_token_;

    // Separate from server_mutex_: Stop() holds that one while waiting for the
    // server task, which may be in a handler that ends up notifying
    std::mutex events_mutex_;   // Guards events_server_
    httpd_handle_t events_server_ = nullptr;
    std::atomic<bool> notifications_changed_queued_ = false;
};

#endif // WEB_SERVER_H
