#include "webhook_target.h"
#include "ancs_defs.h"
#include "encoding.h"
#include "time_sync.h"

#include <cJSON.h>
#include <esp_log.h>
#include <sys/time.h>

#include <cstring>

#define TAG "WebhookTarget"

const char* const kDefaultWebhookTemplate = "【{{app}}】{{title}}\n{{subtitle}}\n{{message}}";

static const struct {
    WebhookFormat format;
    const char* name;
} kFormatNames[] = {
    {WebhookFormat::kJson, "json"},
    {WebhookFormat::kCustom, "custom"},
    {WebhookFormat::kWecom, "wecom"},
    {WebhookFormat::kDingtalk, "dingtalk"},
    {WebhookFormat::kFeishu, "feishu"},
    {WebhookFormat::kBark, "bark"},
    {WebhookFormat::kNtfy, "ntfy"},
    {WebhookFormat::kServerChan, "serverchan"},
    {WebhookFormat::kTelegram, "telegram"},
};

const char* WebhookFormatName(WebhookFormat format) {
    for (auto& entry : kFormatNames) {
        if (entry.format == format) {
            return entry.name;
        }
    }
    return "json";
}

bool ParseWebhookFormat(const char* name, WebhookFormat* format) {
    for (auto& entry : kFormatNames) {
        if (name != nullptr && strcmp(entry.name, name) == 0) {
            *format = entry.format;
            return true;
        }
    }
    return false;
}

// What the secret field holds for this format
enum class SecretUse {
    kNone,
    kAuthorization,
    kSigningKey,
    kChatId,
};

static SecretUse GetSecretUse(WebhookFormat format) {
    switch (format) {
        case WebhookFormat::kJson:
        case WebhookFormat::kCustom:
        case WebhookFormat::kNtfy:
            return SecretUse::kAuthorization;
        case WebhookFormat::kDingtalk:
        case WebhookFormat::kFeishu:
            return SecretUse::kSigningKey;
        case WebhookFormat::kTelegram:
            return SecretUse::kChatId;
        default:
            return SecretUse::kNone;
    }
}

// The admin page shows the same names
static const char* CategoryLabel(uint8_t category_id) {
    static const char* const kLabels[] = {"其他", "来电", "未接来电", "语音留言", "社交", "日程",
                                          "邮件", "新闻", "健康", "财务", "位置", "娱乐"};
    return category_id < sizeof(kLabels) / sizeof(kLabels[0]) ? kLabels[category_id] : "其他";
}

static std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static bool GetPlaceholder(const std::string& key, const AncsNotification& n, std::string* value) {
    if (key == "app") {
        *value = !n.app_name.empty() ? n.app_name : n.app_id;
    } else if (key == "app_id") {
        *value = n.app_id;
    } else if (key == "title") {
        *value = n.title;
    } else if (key == "subtitle") {
        *value = n.subtitle;
    } else if (key == "message") {
        *value = n.message;
    } else if (key == "category") {
        *value = CategoryLabel(n.category_id);
    } else if (key == "device") {
        *value = !n.peer_name.empty() ? n.peer_name : n.peer_address;
    } else if (key == "battery") {
        *value = n.peer_battery >= 0 ? std::to_string(n.peer_battery) + "%" : "";
    } else if (key == "date") {
        *value = n.date;
        if (value->size() > 10 && (*value)[10] == 'T') {
            (*value)[10] = ' ';
        }
    } else if (key == "event") {
        *value = AncsEventName(n.event_id);
    } else if (key == "uid") {
        *value = std::to_string(n.uid);
    } else if (key == "positive_action") {
        *value = n.positive_action;
    } else if (key == "negative_action") {
        *value = n.negative_action;
    } else {
        return false;
    }
    return true;
}

// One line (or the whole template with json_escape). *had_placeholder /
// *all_empty tell whether the line only had empty values.
static std::string RenderPart(const std::string& tmpl, const AncsNotification& n, bool json_escape,
                              bool* had_placeholder, bool* all_empty) {
    std::string out;
    size_t pos = 0;
    *had_placeholder = false;
    *all_empty = true;
    while (pos < tmpl.size()) {
        size_t open = tmpl.find("{{", pos);
        size_t close = open == std::string::npos ? std::string::npos : tmpl.find("}}", open + 2);
        if (close == std::string::npos) {
            out.append(tmpl, pos, std::string::npos);
            break;
        }
        out.append(tmpl, pos, open - pos);
        std::string key = tmpl.substr(open + 2, close - open - 2);
        while (!key.empty() && key.front() == ' ') key.erase(0, 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        std::string value;
        if (GetPlaceholder(key, n, &value)) {
            *had_placeholder = true;
            if (value.find_first_not_of(" \t\r\n") != std::string::npos) {
                *all_empty = false;
            }
            out += json_escape ? JsonEscape(value) : value;
        } else {
            out.append(tmpl, open, close + 2 - open);  // Not ours, keep as is
        }
        pos = close + 2;
    }
    return out;
}

std::string RenderTemplate(const std::string& tmpl, const AncsNotification& notification, bool json_escape) {
    bool had_placeholder, all_empty;
    if (json_escape) {
        return RenderPart(tmpl, notification, true, &had_placeholder, &all_empty);
    }
    std::string out;
    size_t pos = 0;
    while (pos <= tmpl.size()) {
        size_t end = tmpl.find('\n', pos);
        if (end == std::string::npos) {
            end = tmpl.size();
        }
        std::string line = RenderPart(tmpl.substr(pos, end - pos), notification, false, &had_placeholder, &all_empty);
        if (!(had_placeholder && all_empty)) {
            if (!out.empty()) {
                out += '\n';
            }
            out += line;
        }
        pos = end + 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

AncsNotification MakeSampleNotification() {
    AncsNotification notification;
    notification.has_attributes = true;
    notification.category_id = kAncsCategoryOther;
    notification.app_id = "com.example.ancs-esp32";
    notification.app_name = "ANCS-ESP32";
    notification.title = "Webhook 测试";
    notification.message = "这是一条来自 ANCS-ESP32 管理后台的测试通知";
    notification.message_size = notification.message.size();
    notification.peer_name = "iPhone";
    notification.peer_battery = 80;
    time_t now = time(nullptr);
    if (TimeSync::IsClockSet()) {
        struct tm tm;
        localtime_r(&now, &tm);
        char date[24];
        strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
        notification.date = date;
    }
    return notification;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

cJSON* WebhookTarget::ToCJson(bool include_secret) const {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "name", name.c_str());
    cJSON_AddBoolToObject(json, "enabled", enabled);
    cJSON_AddStringToObject(json, "format", WebhookFormatName(format));
    cJSON_AddStringToObject(json, "url", url.c_str());
    cJSON_AddStringToObject(json, "template", message_template.c_str());
    if (include_secret) {
        cJSON_AddStringToObject(json, "secret", secret.c_str());
    } else {
        cJSON_AddBoolToObject(json, "has_secret", !secret.empty());
    }
    return json;
}

std::string WebhookTarget::Update(cJSON* json) {
    cJSON* item = cJSON_GetObjectItem(json, "name");
    if (item != nullptr) {
        if (!cJSON_IsString(item) || strlen(item->valuestring) > kMaxNameLen) {
            return "名称最长 " + std::to_string(kMaxNameLen) + " 字节";
        }
        name = item->valuestring;
    }
    item = cJSON_GetObjectItem(json, "enabled");
    if (item != nullptr) {
        if (!cJSON_IsBool(item)) {
            return "enabled 格式错误";
        }
        enabled = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItem(json, "format");
    if (item != nullptr) {
        WebhookFormat new_format;
        if (!cJSON_IsString(item) || !ParseWebhookFormat(item->valuestring, &new_format)) {
            return "不支持的推送格式";
        }
        // The secret means something else in another format
        if (GetSecretUse(new_format) != GetSecretUse(format) && !cJSON_HasObjectItem(json, "secret")) {
            secret.clear();
        }
        format = new_format;
    }
    item = cJSON_GetObjectItem(json, "url");
    if (item != nullptr) {
        if (!cJSON_IsString(item)) {
            return "URL 格式错误";
        }
        url = item->valuestring;
        while (!url.empty() && url.back() == ' ') url.pop_back();
        while (!url.empty() && url.front() == ' ') url.erase(0, 1);
    }
    item = cJSON_GetObjectItem(json, "secret");
    if (item != nullptr) {
        if (!cJSON_IsString(item)) {
            return "密钥格式错误";
        }
        secret = item->valuestring;
    }
    item = cJSON_GetObjectItem(json, "template");
    if (item != nullptr) {
        if (!cJSON_IsString(item)) {
            return "模板格式错误";
        }
        message_template = item->valuestring;
    }
    return "";
}

std::string WebhookTarget::Validate() const {
    if (url.empty()) {
        return "请填写 URL";
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return "URL 必须以 http:// 或 https:// 开头";
    }
    if (url.size() > kMaxUrlLen) {
        return "URL 最长 " + std::to_string(kMaxUrlLen) + " 字节";
    }
    if (secret.size() > kMaxSecretLen) {
        return "密钥最长 " + std::to_string(kMaxSecretLen) + " 字节";
    }
    if (message_template.size() > kMaxTemplateLen) {
        return "模板最长 " + std::to_string(kMaxTemplateLen) + " 字节";
    }
    if (format == WebhookFormat::kTelegram && secret.empty()) {
        return "请填写 Telegram Chat ID";
    }
    if (format == WebhookFormat::kNtfy) {
        size_t host = url.find("://") + 3;
        size_t slash = url.rfind('/');
        if (slash == std::string::npos || slash < host || slash + 1 >= url.size()) {
            return "ntfy 的 URL 须包含主题，例如 https://ntfy.sh/mytopic";
        }
    }
    if (format == WebhookFormat::kCustom) {
        if (message_template.empty()) {
            return "自定义格式需要填写请求体模板";
        }
        std::string body = RenderTemplate(message_template, MakeSampleNotification(), true);
        cJSON* parsed = cJSON_Parse(body.c_str());
        if (parsed == nullptr) {
            return "请求体模板不是有效的 JSON（占位符要放在字符串的引号里）";
        }
        cJSON_Delete(parsed);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

static std::string PrintJson(cJSON* json) {
    char* str = cJSON_PrintUnformatted(json);
    std::string out(str ? str : "{}");
    cJSON_free(str);
    cJSON_Delete(json);
    return out;
}

static std::string ToString(const std::array<uint8_t, 32>& digest) {
    return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

// Title (first line) and body (the rest) for services that show both
static void SplitTitle(const std::string& text, std::string* title, std::string* body) {
    size_t newline = text.find('\n');
    if (newline == std::string::npos) {
        title->clear();
        *body = text;
    } else {
        *title = text.substr(0, newline);
        *body = text.substr(newline + 1);
    }
}

BuildResult BuildWebhookRequest(const WebhookTarget& target, const AncsNotification& notification, bool test,
                                WebhookRequest* request, std::string* error) {
    request->url = target.url;
    request->authorization = GetSecretUse(target.format) == SecretUse::kAuthorization ? target.secret : "";
    request->body.clear();

    const std::string& tmpl = target.message_template.empty() ? std::string(kDefaultWebhookTemplate)
                                                              : target.message_template;
    std::string text = target.format == WebhookFormat::kJson || target.format == WebhookFormat::kCustom
                           ? ""
                           : RenderTemplate(tmpl, notification, false);
    if (text.empty()) {
        text = !notification.app_name.empty() ? notification.app_name : notification.app_id;
    }

    // Signing services reject requests whose timestamp is off by more than an hour
    bool sign = GetSecretUse(target.format) == SecretUse::kSigningKey && !target.secret.empty();
    struct timeval now = {};
    if (sign) {
        if (!TimeSync::IsClockSet()) {
            *error = "设备时间未校准，无法加签";
            return BuildResult::kNeedClock;
        }
        gettimeofday(&now, nullptr);
    }

    cJSON* root = nullptr;
    switch (target.format) {
        case WebhookFormat::kJson: {
            root = notification.ToCJson();
            if (test) {
                cJSON_ReplaceItemInObject(root, "event", cJSON_CreateString("test"));
            }
            break;
        }
        case WebhookFormat::kCustom: {
            // Validated when saved, but a value could still break it (not with JsonEscape)
            request->body = RenderTemplate(target.message_template, notification, true);
            return BuildResult::kOk;
        }
        case WebhookFormat::kWecom:
        case WebhookFormat::kDingtalk: {
            root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "msgtype", "text");
            cJSON* content = cJSON_AddObjectToObject(root, "text");
            cJSON_AddStringToObject(content, "content", text.c_str());
            if (sign) {
                // https://open.dingtalk.com/document/robots/customize-robot-security-settings
                std::string timestamp = std::to_string((long long)now.tv_sec * 1000 + now.tv_usec / 1000);
                std::string signature = Base64Encode(ToString(HmacSha256(target.secret, timestamp + "\n" + target.secret)));
                request->url += request->url.find('?') == std::string::npos ? '?' : '&';
                request->url += "timestamp=" + timestamp + "&sign=" + UrlEncode(signature);
            }
            break;
        }
        case WebhookFormat::kFeishu: {
            root = cJSON_CreateObject();
            if (sign) {
                // https://open.feishu.cn/document/client-docs/bot-v3/add-custom-bot
                std::string timestamp = std::to_string((long long)now.tv_sec);
                std::string signature = Base64Encode(ToString(HmacSha256(timestamp + "\n" + target.secret, "")));
                cJSON_AddStringToObject(root, "timestamp", timestamp.c_str());
                cJSON_AddStringToObject(root, "sign", signature.c_str());
            }
            cJSON_AddStringToObject(root, "msg_type", "text");
            cJSON* content = cJSON_AddObjectToObject(root, "content");
            cJSON_AddStringToObject(content, "text", text.c_str());
            break;
        }
        case WebhookFormat::kBark: {
            // POST /:key with a JSON body
            std::string title, body;
            SplitTitle(text, &title, &body);
            if (body.empty()) {
                body.swap(title);
            }
            while (!request->url.empty() && request->url.back() == '/') {
                request->url.pop_back();
            }
            root = cJSON_CreateObject();
            if (!title.empty()) {
                cJSON_AddStringToObject(root, "title", title.c_str());
            }
            cJSON_AddStringToObject(root, "body", body.c_str());
            cJSON_AddStringToObject(root, "group", !notification.app_name.empty() ? notification.app_name.c_str()
                                                                                 : "ANCS");
            break;
        }
        case WebhookFormat::kNtfy: {
            // JSON publishing goes to the server root, with the topic in the body
            size_t slash = request->url.rfind('/');
            std::string topic = request->url.substr(slash + 1);
            request->url.resize(slash + 1);
            std::string title, body;
            SplitTitle(text, &title, &body);
            if (body.empty()) {
                body.swap(title);
            }
            root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "topic", topic.c_str());
            if (!title.empty()) {
                cJSON_AddStringToObject(root, "title", title.c_str());
            }
            cJSON_AddStringToObject(root, "message", body.c_str());
            break;
        }
        case WebhookFormat::kServerChan: {
            std::string title, body;
            SplitTitle(text, &title, &body);
            if (title.empty()) {
                title.swap(body);
            }
            root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "title", title.c_str());
            cJSON_AddStringToObject(root, "desp", body.c_str());
            break;
        }
        case WebhookFormat::kTelegram: {
            root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "chat_id", target.secret.c_str());
            cJSON_AddStringToObject(root, "text", text.c_str());
            break;
        }
    }
    request->body = PrintJson(root);
    return BuildResult::kOk;
}

// Number field, or default_value if missing
static double GetNumber(cJSON* json, const char* key, double default_value) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    return cJSON_IsNumber(item) ? item->valuedouble : default_value;
}

static std::string GetString(cJSON* json, const char* key) {
    cJSON* item = cJSON_GetObjectItem(json, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

bool CheckWebhookResponse(WebhookFormat format, const std::string& body, std::string* error) {
    if (format == WebhookFormat::kJson || format == WebhookFormat::kCustom || format == WebhookFormat::kNtfy) {
        return true;  // The HTTP status says it all
    }
    cJSON* json = cJSON_ParseWithLength(body.data(), body.size());
    if (json == nullptr) {
        return true;  // Not what we expected, but 2xx
    }
    bool ok = true;
    std::string message;
    switch (format) {
        case WebhookFormat::kWecom:
        case WebhookFormat::kDingtalk:
            ok = GetNumber(json, "errcode", 0) == 0;
            message = GetString(json, "errmsg");
            break;
        case WebhookFormat::kFeishu:
            ok = GetNumber(json, "code", GetNumber(json, "StatusCode", 0)) == 0;
            message = GetString(json, "msg");
            break;
        case WebhookFormat::kBark:
            ok = GetNumber(json, "code", 200) == 200;
            message = GetString(json, "message");
            break;
        case WebhookFormat::kServerChan:
            ok = GetNumber(json, "code", 0) == 0;
            message = GetString(json, "message");
            break;
        case WebhookFormat::kTelegram: {
            cJSON* item = cJSON_GetObjectItem(json, "ok");
            ok = !cJSON_IsBool(item) || cJSON_IsTrue(item);
            message = GetString(json, "description");
            break;
        }
        default:
            break;
    }
    if (!ok) {
        *error = message.empty() ? body.substr(0, 200) : message;
    }
    cJSON_Delete(json);
    return ok;
}
