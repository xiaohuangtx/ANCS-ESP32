#include "notification_history.h"
#include "notification_store.h"
#include "ancs_defs.h"
#include "settings.h"

#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "NotificationHistory"

// Ids handed out when a notification is not stored; never clash with store ids
static constexpr uint32_t kFirstUnstoredId = 0x80000000;
// Earlier than this, the clock has not been set
static constexpr time_t kMinValidTime = 1704067200;  // 2024-01-01

static time_t WallTime() {
    time_t now = time(nullptr);
    return now >= kMinValidTime ? now : 0;
}

static bool IsSameNotification(const AncsNotification& a, const AncsNotification& b) {
    return a.uid == b.uid && a.peer_address == b.peer_address;
}

NotificationHistory::NotificationHistory() : next_id_(kFirstUnstoredId) {
    Settings settings("history");
    capacity_ = std::clamp((int)settings.GetInt("size", CONFIG_NOTIFICATION_HISTORY_SIZE), kMinCapacity, kMaxCapacity);
    cleared_id_ = (uint32_t)settings.GetInt("cleared", 0);
    Load();
}

void NotificationHistory::Load() {
    NotificationStore::GetInstance().ForEach([this](const StoredNotification& stored) {
        if (stored.id <= cleared_id_ || (int)entries_.size() >= capacity_) {
            return false;
        }
        // Newest first: a later "modified" event already took its place
        bool replaced = std::any_of(entries_.begin(), entries_.end(), [&stored](const Entry& entry) {
            return entry.notification.event_id == kAncsEventModified &&
                   IsSameNotification(entry.notification, stored.notification);
        });
        if (!replaced) {
            entries_.push_back({stored.id, stored.notification, stored.upload_state, -1, stored.received_time});
        }
        return true;
    });
    if (!entries_.empty()) {
        ESP_LOGI(TAG, "Loaded %d notifications from flash", (int)entries_.size());
    }
}

bool NotificationHistory::IsPersistent() {
    return NotificationStore::GetInstance().IsEnabled();
}

uint32_t NotificationHistory::Add(const AncsNotification& notification, UploadState upload_state) {
    time_t received_time = WallTime();
    // Outside the lock: making room may erase a sector, which takes a while
    uint32_t id = NotificationStore::GetInstance().Append(notification, upload_state, received_time);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == 0) {
            id = next_id_++;
        }
        if (notification.event_id == kAncsEventModified) {
            auto it = std::find_if(entries_.begin(), entries_.end(), [&notification](const Entry& entry) {
                return IsSameNotification(entry.notification, notification);
            });
            if (it != entries_.end()) {
                entries_.erase(it);
            }
        }
        entries_.push_front({id, notification, upload_state, esp_timer_get_time(), received_time, true});
        while ((int)entries_.size() > capacity_) {
            entries_.pop_back();
        }
    }
    NotifyChanged();
    return id;
}

void NotificationHistory::SetUploadState(uint32_t id, UploadState state) {
    NotificationStore::GetInstance().SetUploadState(id, state);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [id](const Entry& entry) { return entry.id == id; });
        if (it == entries_.end()) {
            return;
        }
        it->upload_state = state;
    }
    NotifyChanged();
}

void NotificationHistory::SetRemoved(const AncsNotification& notification) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&notification](const Entry& entry) {
            return entry.active && IsSameNotification(entry.notification, notification);
        });
        if (it == entries_.end()) {
            return;
        }
        it->active = false;
    }
    NotifyChanged();
}

void NotificationHistory::Reactivate(const AncsNotification& notification) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&notification](const Entry& entry) {
            return IsSameNotification(entry.notification, notification);
        });
        if (it == entries_.end()) {
            return;
        }
        // The uid stays the same, but the connection is a new one. Only the
        // actions are taken over: the other flags (PreExisting) describe the replay.
        auto& listed = it->notification;
        constexpr uint8_t kActionFlags = kAncsFlagPositiveAction | kAncsFlagNegativeAction;
        listed.conn_handle = notification.conn_handle;
        listed.event_flags = (listed.event_flags & ~kActionFlags) | (notification.event_flags & kActionFlags);
        listed.positive_action = notification.positive_action;
        listed.negative_action = notification.negative_action;
        it->active = true;
    }
    NotifyChanged();
}

void NotificationHistory::OnDisconnected(uint16_t conn_handle) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : entries_) {
            if (entry.active && entry.notification.conn_handle == conn_handle) {
                entry.active = false;
                changed = true;
            }
        }
    }
    if (changed) {
        NotifyChanged();
    }
}

std::optional<AncsNotification> NotificationHistory::GetActive(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id == id) {
            return entry.active ? std::optional(entry.notification) : std::nullopt;
        }
    }
    return std::nullopt;
}

void NotificationHistory::Clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Hide the stored records rather than erase them: wiping the partition
        // would take long and wear the flash
        uint32_t last_stored_id = cleared_id_;
        for (auto& entry : entries_) {
            if (entry.id < kFirstUnstoredId) {
                last_stored_id = std::max(last_stored_id, entry.id);
            }
        }
        entries_.clear();
        if (last_stored_id != cleared_id_) {
            cleared_id_ = last_stored_id;
            Settings settings("history", true);
            settings.SetInt("cleared", (int32_t)cleared_id_);
        }
    }
    NotifyChanged();
}

int NotificationHistory::GetCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

int NotificationHistory::GetCapacity() {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

void NotificationHistory::SetCapacity(int capacity) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = std::clamp(capacity, kMinCapacity, kMaxCapacity);
        while ((int)entries_.size() > capacity_) {
            entries_.pop_back();
        }
        Settings settings("history", true);
        settings.SetInt("size", capacity_);
        ESP_LOGI(TAG, "Capacity set to %d", capacity_);
    }
    NotifyChanged();
}

void NotificationHistory::NotifyChanged() {
    if (change_callback_) {
        change_callback_();
    }
}

cJSON* NotificationHistory::ToCJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_us = esp_timer_get_time();
    time_t now = WallTime();
    cJSON* array = cJSON_CreateArray();
    for (auto& entry : entries_) {
        cJSON* item = entry.notification.ToCJson();
        cJSON_AddNumberToObject(item, "id", entry.id);
        cJSON_AddStringToObject(item, "upload", UploadStateName(entry.upload_state));
        cJSON_AddBoolToObject(item, "active", entry.active);
        if (entry.received_us >= 0) {
            cJSON_AddNumberToObject(item, "received_ago", (double)((now_us - entry.received_us) / 1000000));
        } else if (entry.received_time != 0 && now != 0) {
            cJSON_AddNumberToObject(item, "received_ago", (double)std::max<time_t>(0, now - entry.received_time));
        }
        cJSON_AddItemToArray(array, item);
    }
    return array;
}
