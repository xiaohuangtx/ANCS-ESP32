#ifndef NOTIFICATION_HISTORY_H
#define NOTIFICATION_HISTORY_H

#include <deque>
#include <mutex>
#include <cstdint>
#include <ctime>

#include "notification.h"

/**
 * The most recent notifications for the admin web page, with their webhook
 * delivery state.
 *
 * The capacity is read from NVS (namespace "history", key "size"), falling
 * back to CONFIG_NOTIFICATION_HISTORY_SIZE. Every notification also goes to
 * NotificationStore when it is enabled, and the list is reloaded from there on
 * boot; otherwise it is lost on reboot. All methods are thread-safe.
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

    bool IsPersistent();

    // A "modified" event replaces the earlier entry of the same notification.
    // Returns the id to report the upload state with.
    uint32_t Add(const AncsNotification& notification, UploadState upload_state);
    void SetUploadState(uint32_t id, UploadState state);
    // Stored records stay in flash until overwritten, but are not shown again
    void Clear();
    int GetCount();

    int GetCapacity();
    // Clamped to [kMinCapacity, kMaxCapacity], persisted to NVS
    void SetCapacity(int capacity);

    // JSON array, newest first. Each item is AncsNotification::ToCJson() plus
    // "id", "upload" (UploadStateName) and "received_ago" (seconds since it was
    // received, missing when received before a reboot without a set clock).
    // The caller owns it.
    cJSON* ToCJson();

private:
    NotificationHistory();
    void Load();

    struct Entry {
        uint32_t id = 0;
        AncsNotification notification;
        UploadState upload_state = UploadState::kPending;
        int64_t received_us = -1;  // esp_timer time, -1: before this boot
        time_t received_time = 0;  // Wall clock, 0: not set
    };

    std::mutex mutex_;
    std::deque<Entry> entries_;  // Newest first
    int capacity_ = CONFIG_NOTIFICATION_HISTORY_SIZE;
    uint32_t next_id_ = 1;       // Without NotificationStore
    uint32_t cleared_id_ = 0;    // Stored records up to this id were cleared
};

#endif // NOTIFICATION_HISTORY_H
