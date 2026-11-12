#ifndef HTTP_UTILS_H
#define HTTP_UTILS_H

#include <esp_http_server.h>
#include <cJSON.h>

#include <string>

// JSON helpers shared by the admin page and the provisioning page

// Takes ownership of json
esp_err_t SendJson(httpd_req_t* req, cJSON* json);
// {"success": true}
esp_err_t SendOk(httpd_req_t* req);
// {"error": message}
esp_err_t SendError(httpd_req_t* req, const char* status, const std::string& message);
// "" means success, anything else is sent as a 400 error
esp_err_t SendResult(httpd_req_t* req, const std::string& error);

// nullptr if missing or not a string
const char* GetJsonString(cJSON* json, const char* key);

// Whether the Content-Type header is type (parameters such as "; charset=utf-8"
// are ignored). Browsers cannot send a cross-site request with application/json
// or application/octet-stream without a CORS preflight, which is never answered.
bool HasContentType(httpd_req_t* req, const char* type);

// httpd_req_recv() may return less than asked for
bool RecvExact(httpd_req_t* req, char* buffer, size_t length);
// The caller deletes the result; nullptr if the body is missing, too large, not
// JSON or not sent as application/json
cJSON* ReadJsonBody(httpd_req_t* req);

#endif // HTTP_UTILS_H
