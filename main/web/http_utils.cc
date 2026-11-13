#include "http_utils.h"

#include <cstring>
#include <strings.h>

static constexpr int kMaxRecvTimeouts = 3;

esp_err_t SendJson(httpd_req_t* req, cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, str ? str : "{}");
    cJSON_free(str);
    return err;
}

esp_err_t SendOk(httpd_req_t* req) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "success", true);
    return SendJson(req, json);
}

esp_err_t SendError(httpd_req_t* req, const char* status, const std::string& message) {
    httpd_resp_set_status(req, status);
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message.c_str());
    return SendJson(req, json);
}

esp_err_t SendResult(httpd_req_t* req, const std::string& error) {
    return error.empty() ? SendOk(req) : SendError(req, "400 Bad Request", error);
}

const char* GetJsonString(cJSON* json, const char* key) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

bool HasContentType(httpd_req_t* req, const char* type) {
    char value[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    size_t length = strlen(type);
    return strncasecmp(value, type, length) == 0 && (value[length] == '\0' || value[length] == ';');
}

bool RecvExact(httpd_req_t* req, char* buffer, size_t length) {
    size_t offset = 0;
    int timeouts = 0;
    while (offset < length) {
        int ret = httpd_req_recv(req, buffer + offset, length - offset);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxRecvTimeouts) {
            continue;
        }
        if (ret <= 0) {
            return false;
        }
        offset += ret;
    }
    return true;
}

cJSON* ReadJsonBody(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > kMaxJsonBodySize || !HasContentType(req, "application/json")) {
        return nullptr;
    }
    std::string body(req->content_len, '\0');
    if (!RecvExact(req, body.data(), body.size())) {
        return nullptr;
    }
    return cJSON_ParseWithLength(body.data(), body.size());
}
