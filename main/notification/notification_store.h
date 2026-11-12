#ifndef NOTIFICATION_STORE_H
#define NOTIFICATION_STORE_H

#include <esp_partition.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "notification.h"

struct StoredNotification {
    uint32_t id = 0;
    UploadState upload_state = UploadState::kPending;
    time_t received_time = 0;  // 0 when the clock was not set
    AncsNotification notification;
};

/**
 * Notifications and their webhook delivery state, kept in flash across reboots.
 *
 * A ring log on the first kMaxSize bytes of the "storage" partition (the rest
 * is left for other data): each 4 KB sector starts with a header carrying an
 * increasing sequence number, records are appended behind it and never span
 * sectors. When the ring is
 * full the oldest sector is erased, so every sector wears evenly. The delivery
 * state is one byte outside the record CRC that only ever clears bits, so it
 * is written in place without an erase.
 *
 * Disabled when there is no "storage" partition (4 MB / 8 MB tables).
 * All methods are thread-safe.
 */
class NotificationStore {
public:
    static constexpr size_t kMaxSize = 2 * 1024 * 1024;

    static NotificationStore& GetInstance() {
        static NotificationStore instance;
        return instance;
    }
    NotificationStore(const NotificationStore&) = delete;
    NotificationStore& operator=(const NotificationStore&) = delete;

    bool IsEnabled() const { return partition_ != nullptr; }

    // Returns the new record id (increasing across reboots), 0 on failure
    uint32_t Append(const AncsNotification& notification, UploadState state, time_t received_time);
    // Only a pending record can change state
    void SetUploadState(uint32_t id, UploadState state);

    // Newest first, until the callback returns false. The callback must not
    // call back into the store.
    void ForEach(const std::function<bool(const StoredNotification&)>& callback);

    // Records still kPending, oldest first
    std::vector<uint32_t> GetPendingIds();
    bool ReadPending(uint32_t id, StoredNotification* record);

private:
    NotificationStore();

    struct Record {
        uint32_t offset;  // Within the partition
        StoredNotification stored;
    };

    void Scan();
    // Valid records of a sector in write order. *end is where the next record
    // goes, or 0 if the sector cannot take more (torn write).
    bool ReadSector(int sector, std::vector<Record>* records, uint32_t* end, bool parse);
    bool OpenNextSector();

    std::mutex mutex_;
    const esp_partition_t* partition_ = nullptr;
    int sector_count_ = 0;
    std::vector<uint32_t> sector_sequence_;  // 0: erased or invalid
    std::vector<uint8_t> buffer_;            // One sector
    int head_ = -1;                          // Sector written last
    uint32_t sequence_ = 0;                  // Of the head sector
    uint32_t write_offset_ = 0;              // Within the head sector, 0: full
    uint32_t next_id_ = 1;
    std::map<uint32_t, uint32_t> pending_;   // id -> record offset
};

#endif // NOTIFICATION_STORE_H
