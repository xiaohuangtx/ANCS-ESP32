#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <cstdint>
#include <string>

struct cJSON;

/**
 * An iOS notification received through ANCS ("added" / "modified" events;
 * "removed" events are consumed internally and never emitted).
 */
struct AncsNotification {
    uint8_t event_id = 0;        // AncsEventId
    uint8_t event_flags = 0;     // AncsEventFlag bit mask
    uint8_t category_id = 0;     // AncsCategoryId
    uint8_t category_count = 0;
    uint32_t uid = 0;

    // Source device (one ESP32 can serve several iPhones / iPads)
    uint16_t conn_handle = 0;
    std::string peer_name;       // GAP device name, e.g. "John's iPhone"
    std::string peer_address;    // Identity address, stable across reconnects

    bool has_attributes = false;
    std::string app_id;
    std::string app_name;
    std::string title;
    std::string subtitle;
    std::string message;
    int message_size = 0;
    std::string date;            // ISO 8601 local time, e.g. "2026-11-11T09:30:00"
    std::string positive_action;
    std::string negative_action;

    // iOS cuts the message at CONFIG_ANCS_MAX_MESSAGE_LEN bytes. The spec does
    // not say whether message_size counts bytes or characters, so also treat a
    // message that (nearly) fills the requested length as truncated.
    bool IsMessageTruncated() const;

    // Print a human readable block to the console
    void Print() const;
    // Serialize to the JSON body used by the webhook
    std::string ToJson() const;
    // Same fields as ToJson(), the caller owns the returned object
    cJSON* ToCJson() const;
};

#endif // NOTIFICATION_H
