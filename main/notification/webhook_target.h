#ifndef WEBHOOK_TARGET_H
#define WEBHOOK_TARGET_H

#include <cstdint>
#include <string>

#include "notification.h"

struct cJSON;

/**
 * Where and how a notification is pushed. Besides the raw JSON of the
 * notification, the request can be shaped for common push services, so they
 * work without a server in between.
 *
 * The meaning of `secret` depends on the format (it is never sent back to the
 * admin page):
 *   json / custom / ntfy:  value of the Authorization header (optional)
 *   dingtalk / feishu:     signing secret (optional, "加签")
 *   telegram:              chat_id (required)
 *   others:                unused, the key is part of the URL
 *
 * `message_template` shapes the text for the chat / push formats, with
 * placeholders such as {{title}} (see RenderTemplate()); the first line is the
 * title where the service has one (Bark, ntfy, ServerChan). For the custom
 * format it is the JSON body itself.
 */
enum class WebhookFormat : uint8_t {
    kJson,        // The notification as JSON (default, the original format)
    kCustom,      // JSON body from message_template
    kWecom,       // 企业微信群机器人
    kDingtalk,    // 钉钉群机器人
    kFeishu,      // 飞书 / Lark 群机器人
    kBark,
    kNtfy,
    kServerChan,  // Server酱
    kTelegram,
};

const char* WebhookFormatName(WebhookFormat format);
bool ParseWebhookFormat(const char* name, WebhookFormat* format);

struct WebhookTarget {
    static constexpr size_t kMaxNameLen = 32;
    static constexpr size_t kMaxUrlLen = 512;
    static constexpr size_t kMaxSecretLen = 256;
    static constexpr size_t kMaxTemplateLen = 1024;

    std::string name;
    bool enabled = true;
    WebhookFormat format = WebhookFormat::kJson;
    std::string url;
    std::string secret;
    std::string message_template;  // Empty: kDefaultTemplate

    bool IsActive() const { return enabled && !url.empty(); }

    // With "secret" only when include_secret, otherwise "has_secret"
    cJSON* ToCJson(bool include_secret) const;
    // Reads the fields present in json over the current values. "" on success,
    // otherwise an error message for the admin page.
    std::string Update(cJSON* json);
    // "" if the target can be used
    std::string Validate() const;
};

// Used when message_template is empty
extern const char* const kDefaultWebhookTemplate;

// Replaces {{app}}, {{app_id}}, {{title}}, {{subtitle}}, {{message}},
// {{category}}, {{device}}, {{date}}, {{event}}, {{uid}},
// {{positive_action}}, {{negative_action}}. With json_escape the values are
// escaped for use inside a JSON string. Otherwise lines whose placeholders all
// came out empty are dropped (e.g. a missing subtitle).
std::string RenderTemplate(const std::string& tmpl, const AncsNotification& notification, bool json_escape);

struct WebhookRequest {
    std::string url;
    std::string authorization;  // Empty: no header
    std::string body;
};

enum class BuildResult {
    kOk,
    kNeedClock,  // Signing needs the current time, which is not set yet
    kError,
};

// test: sent from the admin page, the event is reported as "test"
BuildResult BuildWebhookRequest(const WebhookTarget& target, const AncsNotification& notification, bool test,
                                WebhookRequest* request, std::string* error);

// Some services answer HTTP 200 with an error code in the body. Returns false
// with *error set if the 2xx response body reports a failure.
bool CheckWebhookResponse(WebhookFormat format, const std::string& body, std::string* error);

// A sample notification for tests and template validation
AncsNotification MakeSampleNotification();

#endif // WEBHOOK_TARGET_H
