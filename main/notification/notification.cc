#include "notification.h"
#include "ancs_defs.h"

#include <cstdio>
#include <cJSON.h>
#include <esp_mac.h>

static std::string FlagsToString(uint8_t flags) {
    std::string s;
    auto append = [&s](const char* name) {
        if (!s.empty()) {
            s += "|";
        }
        s += name;
    };
    if (flags & kAncsFlagSilent) append("Silent");
    if (flags & kAncsFlagImportant) append("Important");
    if (flags & kAncsFlagPreExisting) append("PreExisting");
    if (flags & kAncsFlagPositiveAction) append("PositiveAction");
    if (flags & kAncsFlagNegativeAction) append("NegativeAction");
    return s.empty() ? "-" : s;
}

static std::string GetDeviceId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

void AncsNotification::Print() const {
    printf("\n==================== ANCS Notification ====================\n");
    printf("Device   : %s (%s)\n", peer_name.c_str(), peer_address.c_str());
    printf("Event    : %s (uid=%lu)\n", AncsEventName(event_id), (unsigned long)uid);
    printf("Category : %s (count=%u)\n", AncsCategoryName(category_id), category_count);
    printf("Flags    : %s\n", FlagsToString(event_flags).c_str());
    if (has_attributes) {
        if (app_name.empty()) {
            printf("App      : %s\n", app_id.c_str());
        } else {
            printf("App      : %s (%s)\n", app_name.c_str(), app_id.c_str());
        }
        printf("Date     : %s\n", date.c_str());
        printf("Title    : %s\n", title.c_str());
        if (!subtitle.empty()) {
            printf("Subtitle : %s\n", subtitle.c_str());
        }
        printf("Message  : %s\n", message.c_str());
        if ((size_t)message_size > message.size()) {
            printf("           (truncated, full size %d bytes)\n", message_size);
        }
        if (!positive_action.empty() || !negative_action.empty()) {
            printf("Actions  : [%s] / [%s]\n", positive_action.c_str(), negative_action.c_str());
        }
    }
    printf("===========================================================\n\n");
}

std::string AncsNotification::ToJson() const {
    static const std::string device_id = GetDeviceId();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", device_id.c_str());
    cJSON_AddStringToObject(root, "peer_name", peer_name.c_str());
    cJSON_AddStringToObject(root, "peer_address", peer_address.c_str());
    cJSON_AddStringToObject(root, "event", AncsEventName(event_id));
    cJSON_AddNumberToObject(root, "uid", uid);
    cJSON_AddStringToObject(root, "category", AncsCategoryName(category_id));
    cJSON_AddNumberToObject(root, "category_id", category_id);
    cJSON_AddNumberToObject(root, "category_count", category_count);

    cJSON* flags = cJSON_AddObjectToObject(root, "flags");
    cJSON_AddBoolToObject(flags, "silent", event_flags & kAncsFlagSilent);
    cJSON_AddBoolToObject(flags, "important", event_flags & kAncsFlagImportant);
    cJSON_AddBoolToObject(flags, "pre_existing", event_flags & kAncsFlagPreExisting);
    cJSON_AddBoolToObject(flags, "positive_action", event_flags & kAncsFlagPositiveAction);
    cJSON_AddBoolToObject(flags, "negative_action", event_flags & kAncsFlagNegativeAction);

    if (has_attributes) {
        cJSON_AddStringToObject(root, "app_id", app_id.c_str());
        cJSON_AddStringToObject(root, "app_name", app_name.c_str());
        cJSON_AddStringToObject(root, "title", title.c_str());
        cJSON_AddStringToObject(root, "subtitle", subtitle.c_str());
        cJSON_AddStringToObject(root, "message", message.c_str());
        cJSON_AddNumberToObject(root, "message_size", message_size);
        cJSON_AddStringToObject(root, "date", date.c_str());
        cJSON_AddStringToObject(root, "positive_action", positive_action.c_str());
        cJSON_AddStringToObject(root, "negative_action", negative_action.c_str());
    }

    char* str = cJSON_PrintUnformatted(root);
    std::string json(str ? str : "{}");
    cJSON_free(str);
    cJSON_Delete(root);
    return json;
}
