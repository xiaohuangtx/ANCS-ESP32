#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>

#include <string>
#include <mutex>

/**
 * Admin web page on port 80, served while the station is connected:
 * device status, notification history, webhook / history settings,
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

private:
    WebServer() = default;

    // Host / Origin checks, then the password. Sends the error response and
    // returns false if the request must not be handled.
    bool CheckRequest(httpd_req_t* req);
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
    static esp_err_t HandleGetWifi(httpd_req_t* req);
    static esp_err_t HandleAddWifi(httpd_req_t* req);
    static esp_err_t HandleUpdateWifi(httpd_req_t* req);
    static esp_err_t HandleRemoveWifi(httpd_req_t* req);

    std::mutex server_mutex_;   // Guards server_ (Start / Stop)
    httpd_handle_t server_ = nullptr;

    std::mutex mutex_;          // Guards password_ (used by handlers)
    std::string password_;
};

#endif // WEB_SERVER_H
