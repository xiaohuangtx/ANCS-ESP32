#include "notification_filter.h"
#include "webhook_client.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

#define TAG "NotificationFilter"

// Longest NVS string value, including the terminator
static constexpr size_t kMaxStoredSize = 4000;
static constexpr int kCategoryCount = 12;

static const char* const kActionNames[] = {"forward", "record", "drop"};

const char* FilterActionName(NotificationFilter::Action action) {
    return kActionNames[static_cast<int>(action)];
}

static bool ParseAction(cJSON* item, NotificationFilter::Action* action) {
    if (!cJSON_IsString(item)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(item->valuestring, kActionNames[i]) == 0) {
            *action = static_cast<NotificationFilter::Action>(i);
            return true;
        }
    }
    return false;
}

static std::string ToLowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }
    return s;
}

NotificationFilter::NotificationFilter() {
    Settings settings("filter");
    std::string stored = settings.GetString("rules");
    if (stored.empty()) {
        return;
    }
    cJSON* json = cJSON_Parse(stored.c_str());
    std::string error = json ? Parse(json, &default_action_, &rules_) : "not JSON";
    cJSON_Delete(json);
    if (!error.empty()) {
        ESP_LOGE(TAG, "Stored rules ignored: %s", error.c_str());
        rules_.clear();
        default_action_ = Action::kForward;
    }
    hits_.assign(rules_.size(), 0);
    ESP_LOGI(TAG, "%d rules loaded, default action %s", (int)rules_.size(), FilterActionName(default_action_));
}

bool NotificationFilter::Matches(const Rule& rule, const AncsNotification& n) {
    if (rule.categories != 0 && (n.category_id >= 32 || !(rule.categories & (1u << n.category_id)))) {
        return false;
    }
    if (!rule.apps.empty()) {
        std::string app_id = ToLowerAscii(n.app_id);
        std::string app_name = ToLowerAscii(n.app_name);
        bool found = std::any_of(rule.apps.begin(), rule.apps.end(), [&](const std::string& app) {
            std::string lower = ToLowerAscii(app);
            return lower == app_id || (!app_name.empty() && lower == app_name);
        });
        if (!found) {
            return false;
        }
    }
    if (!rule.keywords.empty()) {
        std::string text = ToLowerAscii(n.title + '\n' + n.subtitle + '\n' + n.message);
        bool found = std::any_of(rule.keywords.begin(), rule.keywords.end(), [&](const std::string& keyword) {
            return text.find(ToLowerAscii(keyword)) != std::string::npos;
        });
        if (!found) {
            return false;
        }
    }
    if (!rule.devices.empty()) {
        bool found = std::any_of(rule.devices.begin(), rule.devices.end(), [&](const std::string& device) {
            return device == n.peer_name || ToLowerAscii(device) == n.peer_address;
        });
        if (!found) {
            return false;
        }
    }
    return true;
}

NotificationFilter::Decision NotificationFilter::Evaluate(const AncsNotification& notification) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < rules_.size(); i++) {
        auto& rule = rules_[i];
        if (rule.enabled && Matches(rule, notification)) {
            hits_[i]++;
            return {rule.action, rule.targets, (int)i};
        }
    }
    default_hits_++;
    return {default_action_, 0, -1};
}

// A list of non-empty strings, kept as typed (matching ignores ASCII case where it should)
static std::string ParseStrings(cJSON* json, const char* key, const char* label, std::vector<std::string>* out) {
    cJSON* array = cJSON_GetObjectItem(json, key);
    if (array == nullptr) {
        return "";
    }
    if (!cJSON_IsArray(array)) {
        return std::string(label) + "格式错误";
    }
    if (cJSON_GetArraySize(array) > NotificationFilter::kMaxValues) {
        return std::string(label) + "最多 " + std::to_string(NotificationFilter::kMaxValues) + " 个";
    }
    cJSON* item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item)) {
            return std::string(label) + "格式错误";
        }
        std::string value = item->valuestring;
        value.erase(0, value.find_first_not_of(' '));
        value.erase(value.find_last_not_of(' ') + 1);
        if (value.empty()) {
            continue;
        }
        if (value.size() > NotificationFilter::kMaxValueLen) {
            return std::string(label) + "「" + value + "」太长";
        }
        out->push_back(value);
    }
    return "";
}

// A list of small integers as a bit mask
static std::string ParseBits(cJSON* json, const char* key, const char* label, int limit, uint32_t* out) {
    cJSON* array = cJSON_GetObjectItem(json, key);
    *out = 0;
    if (array == nullptr) {
        return "";
    }
    if (!cJSON_IsArray(array)) {
        return std::string(label) + "格式错误";
    }
    cJSON* item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint >= limit) {
            return std::string(label) + "格式错误";
        }
        *out |= 1u << item->valueint;
    }
    return "";
}

std::string NotificationFilter::Parse(cJSON* json, Action* default_action, std::vector<Rule>* rules) {
    *default_action = Action::kForward;
    cJSON* item = cJSON_GetObjectItem(json, "default_action");
    if (item != nullptr && !ParseAction(item, default_action)) {
        return "默认操作格式错误";
    }
    cJSON* array = cJSON_GetObjectItem(json, "rules");
    if (array == nullptr) {
        return "";
    }
    if (!cJSON_IsArray(array)) {
        return "规则格式错误";
    }
    if (cJSON_GetArraySize(array) > kMaxRules) {
        return "最多 " + std::to_string(kMaxRules) + " 条规则";
    }
    int index = 0;
    cJSON* rule_json;
    cJSON_ArrayForEach(rule_json, array) {
        index++;
        std::string prefix = "第 " + std::to_string(index) + " 条规则：";
        if (!cJSON_IsObject(rule_json)) {
            return prefix + "格式错误";
        }
        Rule rule;
        item = cJSON_GetObjectItem(rule_json, "name");
        if (item != nullptr) {
            if (!cJSON_IsString(item) || strlen(item->valuestring) > kMaxValueLen) {
                return prefix + "名称太长";
            }
            rule.name = item->valuestring;
        }
        item = cJSON_GetObjectItem(rule_json, "enabled");
        rule.enabled = item == nullptr || cJSON_IsTrue(item);
        std::string error = ParseStrings(rule_json, "apps", "App", &rule.apps);
        if (error.empty()) error = ParseStrings(rule_json, "keywords", "关键词", &rule.keywords);
        if (error.empty()) error = ParseStrings(rule_json, "devices", "设备", &rule.devices);
        if (error.empty()) error = ParseBits(rule_json, "categories", "分类", kCategoryCount, &rule.categories);
        if (error.empty()) error = ParseBits(rule_json, "targets", "推送目标", WebhookClient::kMaxTargets, &rule.targets);
        if (!error.empty()) {
            return prefix + error;
        }
        item = cJSON_GetObjectItem(rule_json, "action");
        if (item == nullptr || !ParseAction(item, &rule.action)) {
            return prefix + "操作格式错误";
        }
        if (rule.action != Action::kForward) {
            rule.targets = 0;
        }
        rules->push_back(std::move(rule));
    }
    return "";
}

static void AddStrings(cJSON* json, const char* key, const std::vector<std::string>& values) {
    cJSON* array = cJSON_AddArrayToObject(json, key);
    for (auto& value : values) {
        cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str()));
    }
}

static void AddBits(cJSON* json, const char* key, uint32_t bits) {
    cJSON* array = cJSON_AddArrayToObject(json, key);
    for (int i = 0; i < 32; i++) {
        if (bits & (1u << i)) {
            cJSON_AddItemToArray(array, cJSON_CreateNumber(i));
        }
    }
}

// Caller holds mutex_
cJSON* NotificationFilter::RulesToCJson() {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "default_action", FilterActionName(default_action_));
    cJSON* array = cJSON_AddArrayToObject(json, "rules");
    for (auto& rule : rules_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", rule.name.c_str());
        cJSON_AddBoolToObject(item, "enabled", rule.enabled);
        AddStrings(item, "apps", rule.apps);
        AddBits(item, "categories", rule.categories);
        AddStrings(item, "keywords", rule.keywords);
        AddStrings(item, "devices", rule.devices);
        cJSON_AddStringToObject(item, "action", FilterActionName(rule.action));
        AddBits(item, "targets", rule.targets);
        cJSON_AddItemToArray(array, item);
    }
    return json;
}

cJSON* NotificationFilter::ToCJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* json = RulesToCJson();
    cJSON* hits = cJSON_AddArrayToObject(json, "hits");
    for (auto count : hits_) {
        cJSON_AddItemToArray(hits, cJSON_CreateNumber(count));
    }
    cJSON_AddNumberToObject(json, "default_hits", default_hits_);
    cJSON_AddNumberToObject(json, "max_rules", kMaxRules);
    return json;
}

std::string NotificationFilter::Set(cJSON* json) {
    Action default_action;
    std::vector<Rule> rules;
    std::string error = Parse(json, &default_action, &rules);
    if (!error.empty()) {
        return error;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto old_default = default_action_;
    default_action_ = default_action;
    rules_.swap(rules);
    cJSON* stored_json = RulesToCJson();
    char* str = cJSON_PrintUnformatted(stored_json);
    std::string stored(str ? str : "");
    cJSON_free(str);
    cJSON_Delete(stored_json);
    if (stored.empty() || stored.size() >= kMaxStoredSize) {
        // Put the old rules back
        default_action_ = old_default;
        rules_.swap(rules);
        return "规则内容太多（保存后超过 " + std::to_string(kMaxStoredSize) + " 字节），请减少规则或关键词";
    }
    Settings settings("filter", true);
    settings.SetString("rules", stored);
    hits_.assign(rules_.size(), 0);
    default_hits_ = 0;
    ESP_LOGI(TAG, "%d rules saved, default action %s", (int)rules_.size(), FilterActionName(default_action_));
    return "";
}
