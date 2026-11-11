#include "notification_history.h"
#include "ancs_defs.h"
#include "settings.h"

#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "NotificationHistory"

NotificationHistory::NotificationHistory() {
    Settings settings("history");
    capacity_ = std::clamp((int)settings.GetInt("size", CONFIG_NOTIFICATION_HISTORY_SIZE), kMinCapacity, kMaxCapacity);
}

void NotificationHistory::Add(const AncsNotification& notification) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (notification.event_id == kAncsEventModified) {
        auto it = std::find_if(entries_.begin(), entries_.end(), [&notification](const Entry& entry) {
            return entry.notification.uid == notification.uid &&
                   entry.notification.peer_address == notification.peer_address;
        });
        if (it != entries_.end()) {
            entries_.erase(it);
        }
    }
    entries_.push_front({notification, esp_timer_get_time()});
    while ((int)entries_.size() > capacity_) {
        entries_.pop_back();
    }
}

void NotificationHistory::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
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
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = std::clamp(capacity, kMinCapacity, kMaxCapacity);
    while ((int)entries_.size() > capacity_) {
        entries_.pop_back();
    }
    Settings settings("history", true);
    settings.SetInt("size", capacity_);
    ESP_LOGI(TAG, "Capacity set to %d", capacity_);
}

cJSON* NotificationHistory::ToCJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = esp_timer_get_time();
    cJSON* array = cJSON_CreateArray();
    for (auto& entry : entries_) {
        cJSON* item = entry.notification.ToCJson();
        cJSON_AddNumberToObject(item, "received_ago", (double)((now - entry.received_us) / 1000000));
        cJSON_AddItemToArray(array, item);
    }
    return array;
}
