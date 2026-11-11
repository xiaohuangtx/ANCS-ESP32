#ifndef NOTIFICATION_HISTORY_H
#define NOTIFICATION_HISTORY_H

#include <deque>
#include <mutex>
#include <cstdint>

#include "notification.h"

/**
 * The most recent notifications, kept in RAM for the admin web page.
 *
 * The capacity is read from NVS (namespace "history", key "size"), falling
 * back to CONFIG_NOTIFICATION_HISTORY_SIZE. The list is lost on reboot.
 * All methods are thread-safe.
 */
class NotificationHistory {
public:
    static constexpr int kMinCapacity = 1;
    static constexpr int kMaxCapacity = 100;

    static NotificationHistory& GetInstance() {
        static NotificationHistory instance;
        return instance;
    }
    NotificationHistory(const NotificationHistory&) = delete;
    NotificationHistory& operator=(const NotificationHistory&) = delete;

    // A "modified" event replaces the earlier entry of the same notification
    void Add(const AncsNotification& notification);
    void Clear();
    int GetCount();

    int GetCapacity();
    // Clamped to [kMinCapacity, kMaxCapacity], persisted to NVS
    void SetCapacity(int capacity);

    // JSON array, newest first. Each item is AncsNotification::ToCJson() plus
    // "received_ago" (seconds since it was received). The caller owns it.
    cJSON* ToCJson();

private:
    NotificationHistory();

    struct Entry {
        AncsNotification notification;
        int64_t received_us = 0;
    };

    std::mutex mutex_;
    std::deque<Entry> entries_;  // Newest first
    int capacity_ = CONFIG_NOTIFICATION_HISTORY_SIZE;
};

#endif // NOTIFICATION_HISTORY_H
