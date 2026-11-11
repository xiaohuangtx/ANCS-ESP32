#include "config_portal.h"
#include "wifi_service.h"
#include "http_utils.h"

#include <esp_log.h>
#include <cJSON.h>

#include <string>

#define TAG "ConfigPortal"

extern const char config_portal_html_start[] asm("_binary_config_portal_html_start");
extern const char config_portal_html_end[] asm("_binary_config_portal_html_end");

// Time for the browser to show the result before the hotspot goes away
static constexpr int kExitDelayMs = 3000;

ConfigPortal::~ConfigPortal() {
    Stop();
}

bool ConfigPortal::Start(uint32_t ip) {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    // Phones open several connections for their connectivity checks
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        server_ = nullptr;
        return false;
    }

    const httpd_uri_t uris[] = {
        {"/", HTTP_GET, HandleIndex, this},
        {"/api/status", HTTP_GET, HandleStatus, this},
        {"/api/scan", HTTP_GET, HandleScan, this},
        {"/api/connect", HTTP_POST, HandleConnect, this},
        {"/api/saved", HTTP_DELETE, HandleRemove, this},
        {"/api/exit", HTTP_POST, HandleExit, this},
    };
    for (auto& uri : uris) {
        httpd_register_uri_handler(server_, &uri);
    }
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, HandleNotFound);

    dns_server_.Start(ip);
    ESP_LOGI(TAG, "Setup page: http://%s/", kAddress);
    return true;
}

void ConfigPortal::Stop() {
    if (server_ == nullptr) {
        return;
    }
    dns_server_.Stop();
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(TAG, "Stopped");
}

esp_err_t ConfigPortal::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, config_portal_html_start, config_portal_html_end - config_portal_html_start - 1);
}

esp_err_t ConfigPortal::HandleStatus(httpd_req_t* req) {
    auto& wifi = WifiService::GetInstance();
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "device", wifi.GetDeviceName().c_str());
    cJSON_AddBoolToObject(json, "support_5g", WifiService::Supports5G());
    cJSON_AddNumberToObject(json, "max", WifiService::kMaxSavedNetworks);
    cJSON* saved = cJSON_AddArrayToObject(json, "saved");
    for (auto& network : wifi.GetSavedNetworks()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
        cJSON_AddBoolToObject(item, "has_password", network.has_password);
        cJSON_AddItemToArray(saved, item);
    }
    return SendJson(req, json);
}

esp_err_t ConfigPortal::HandleScan(httpd_req_t* req) {
    cJSON* json = cJSON_CreateObject();
    cJSON* aps = cJSON_AddArrayToObject(json, "aps");
    for (auto& network : WifiService::GetInstance().GetScanResults()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", network.rssi);
        cJSON_AddBoolToObject(item, "secure", network.secure);
        cJSON_AddItemToArray(aps, item);
    }
    return SendJson(req, json);
}

// {"ssid", "password"}; an empty or missing password means an open network.
// On success the device leaves config mode shortly after answering.
esp_err_t ConfigPortal::HandleConnect(httpd_req_t* req) {
    cJSON* json = ReadJsonBody(req);
    const char* ssid = json ? GetJsonString(json, "ssid") : nullptr;
    if (ssid == nullptr) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    const char* password = GetJsonString(json, "password");
    std::string ssid_str = ssid;
    std::string password_str = password ? password : "";
    cJSON_Delete(json);

    auto& wifi = WifiService::GetInstance();
    std::string ip;
    auto error = wifi.ProvisionNetwork(ssid_str, password_str, ip);
    if (!error.empty()) {
        return SendError(req, "400 Bad Request", error);
    }
    wifi.ExitWifiConfigMode(kExitDelayMs);

    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", true);
    cJSON_AddStringToObject(result, "ssid", ssid_str.c_str());
    cJSON_AddStringToObject(result, "ip", ip.c_str());
    return SendJson(req, result);
}

// {"ssid"}
esp_err_t ConfigPortal::HandleRemove(httpd_req_t* req) {
    cJSON* json = ReadJsonBody(req);
    const char* ssid = json ? GetJsonString(json, "ssid") : nullptr;
    if (ssid == nullptr) {
        cJSON_Delete(json);
        return SendError(req, "400 Bad Request", "请求格式错误");
    }
    auto error = WifiService::GetInstance().RemoveNetwork(ssid);
    cJSON_Delete(json);
    return SendResult(req, error);
}

// Leave config mode and connect to the saved networks
esp_err_t ConfigPortal::HandleExit(httpd_req_t* req) {
    auto& wifi = WifiService::GetInstance();
    if (wifi.GetSavedNetworks().empty()) {
        return SendError(req, "400 Bad Request", "还没有保存任何 Wi-Fi");
    }
    wifi.ExitWifiConfigMode(kExitDelayMs);
    return SendOk(req);
}

esp_err_t ConfigPortal::HandleNotFound(httpd_req_t* req, httpd_err_code_t error) {
    static const std::string location = std::string("http://") + kAddress + "/";
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}
