#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "dns_server.h"

#include <esp_http_server.h>

#include <cstdint>

/**
 * Captive portal of the provisioning hotspot: the Wi-Fi setup page on port
 * 80, plus a DNS server that resolves every name to the hotspot. Any unknown
 * URL (e.g. the connectivity checks of iOS / Android / Windows) is redirected
 * to the page, so phones pop it up on their own after joining the hotspot.
 */
class ConfigPortal {
public:
    static constexpr const char* kAddress = "192.168.4.1";

    ConfigPortal() = default;
    ~ConfigPortal();
    ConfigPortal(const ConfigPortal&) = delete;
    ConfigPortal& operator=(const ConfigPortal&) = delete;

    // ip: address of the hotspot, network byte order
    bool Start(uint32_t ip);
    // Must not be called from a request handler
    void Stop();

private:
    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleStatus(httpd_req_t* req);
    static esp_err_t HandleScan(httpd_req_t* req);
    static esp_err_t HandleConnect(httpd_req_t* req);
    static esp_err_t HandleRemove(httpd_req_t* req);
    static esp_err_t HandleExit(httpd_req_t* req);
    static esp_err_t HandleNotFound(httpd_req_t* req, httpd_err_code_t error);

    httpd_handle_t server_ = nullptr;
    DnsServer dns_server_;
};

#endif // CONFIG_PORTAL_H
