#ifndef NOTIFICATION_FILTER_H
#define NOTIFICATION_FILTER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "notification.h"

struct cJSON;

/**
 * Rules deciding what happens to each notification: pushed (to all webhook
 * targets or some of them), only recorded on the admin page, or dropped.
 *
 * Rules are checked in order and the first enabled rule that matches decides;
 * without a match the default action applies (push to all targets). A rule
 * matches when every condition it has matches; within one condition any of
 * the values may match. An empty condition matches everything:
 *   apps:       app id or app display name, exact, ignoring ASCII case
 *   categories: ANCS category ids
 *   keywords:   contained in the title, subtitle or message, ignoring ASCII case
 *   devices:    device name or identity address, exact
 *
 * Saved as JSON in NVS (namespace "filter", key "rules"). Thread-safe.
 */
class NotificationFilter {
public:
    static constexpr int kMaxRules = 20;
    static constexpr int kMaxValues = 20;      // Per condition
    static constexpr size_t kMaxValueLen = 64;

    enum class Action : uint8_t {
        kForward,  // Record, and push to the webhook targets
        kRecord,   // Record on the admin page only
        kDrop,     // Not recorded, not pushed
    };

    struct Decision {
        Action action;
        uint32_t targets;  // Webhook target slots for kForward, 0: all
        int rule;          // Index of the matching rule, -1: default action
    };

    static NotificationFilter& GetInstance() {
        static NotificationFilter instance;
        return instance;
    }
    NotificationFilter(const NotificationFilter&) = delete;
    NotificationFilter& operator=(const NotificationFilter&) = delete;

    Decision Evaluate(const AncsNotification& notification);

    // {"default_action", "rules": [{"name", "enabled", "apps", "categories",
    // "keywords", "devices", "action", "targets"}], "hits": [...]}. "hits"
    // counts the matches of each rule since boot (or since the rules were saved).
    cJSON* ToCJson();
    // Replaces all rules; "" on success, otherwise an error message
    std::string Set(cJSON* json);

private:
    struct Rule {
        std::string name;
        bool enabled = true;
        std::vector<std::string> apps;
        uint32_t categories = 0;  // Bit per category id, 0: any
        std::vector<std::string> keywords;
        std::vector<std::string> devices;
        Action action = Action::kForward;
        uint32_t targets = 0;     // Bit per webhook slot, 0: all
    };

    NotificationFilter();
    static bool Matches(const Rule& rule, const AncsNotification& notification);
    static std::string Parse(cJSON* json, Action* default_action, std::vector<Rule>* rules);
    cJSON* RulesToCJson();

    std::mutex mutex_;
    Action default_action_ = Action::kForward;
    std::vector<Rule> rules_;
    std::vector<uint32_t> hits_;
    uint32_t default_hits_ = 0;
};

const char* FilterActionName(NotificationFilter::Action action);

#endif // NOTIFICATION_FILTER_H
