#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>

#include <string>
#include <mutex>

/**
 * Admin web page on port 80, served while the station is connected:
 * device status, notification history, webhook / history settings,
 * firmware upload (OTA), reboot.
 *
 * If an admin password is set (NVS namespace "web", key "password", falling
 * back to CONFIG_WEB_ADMIN_PASSWORD), every request needs HTTP Basic auth
 * with user "admin".
 *
 * The provisioning hotspot runs its own server on the same
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

private:
    WebServer() = default;

    bool CheckAuth(httpd_req_t* req);
    void SetPassword(const std::string& password);
    bool HasPassword();
    bool ReceiveFirmware(httpd_req_t* req, std::string& error);

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleGetNotifications(httpd_req_t* req);
    static esp_err_t HandleClearNotifications(httpd_req_t* req);
    static esp_err_t HandleGetConfig(httpd_req_t* req);
    static esp_err_t HandleSetConfig(httpd_req_t* req);
    static esp_err_t HandleWebhookTest(httpd_req_t* req);
    static esp_err_t HandleOta(httpd_req_t* req);
    static esp_err_t HandleReboot(httpd_req_t* req);
    static esp_err_t HandleClearBonds(httpd_req_t* req);
    static esp_err_t HandleWifiConfigMode(httpd_req_t* req);

    std::mutex server_mutex_;   // Guards server_ (Start / Stop)
    httpd_handle_t server_ = nullptr;

    std::mutex mutex_;          // Guards password_ (used by handlers)
    std::string password_;
};

#endif // WEB_SERVER_H
