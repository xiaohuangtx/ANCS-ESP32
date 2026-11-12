#include "notification_store.h"

#include <esp_log.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#define TAG "NotificationStore"

static constexpr const char* kPartitionLabel = "storage";
static constexpr uint32_t kSectorSize = 4096;
static constexpr int kMinSectors = 4;
static constexpr uint32_t kSectorMagic = 0x3146544E;  // "NTF1"
static constexpr uint16_t kRecordMagic = 0x4352;      // "RC"
static constexpr uint8_t kPayloadVersion = 1;

struct SectorHeader {
    uint32_t magic;
    uint32_t sequence;
    uint32_t reserved;
    uint32_t crc;  // Of the fields above
};

struct RecordHeader {
    uint16_t magic;
    uint16_t length;       // Payload bytes
    uint32_t id;
    uint32_t crc;          // Of magic, length, id and the payload
    uint8_t upload_state;  // UploadState, not in the CRC: cleared in place
    uint8_t reserved[3];
};

static_assert(sizeof(SectorHeader) == 16 && sizeof(RecordHeader) == 16, "Flash layout");

// Header + payload, padded to 4 bytes
static uint32_t RecordSize(uint32_t length) {
    return (sizeof(RecordHeader) + length + 3) & ~3u;
}

static uint32_t RecordCrc(const RecordHeader& header, const uint8_t* payload) {
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&header), offsetof(RecordHeader, crc));
    return esp_rom_crc32_le(crc, payload, header.length);
}

static bool IsErased(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    return std::all_of(p, p + size, [](uint8_t b) { return b == 0xFF; });
}

static UploadState ToUploadState(uint8_t value) {
    switch (static_cast<UploadState>(value)) {
        case UploadState::kPending:
        case UploadState::kDelivered:
        case UploadState::kFailed:
        case UploadState::kSkipped:
            return static_cast<UploadState>(value);
    }
    return UploadState::kFailed;  // Torn state write
}

// Payload: little-endian fields, strings prefixed with a uint16 length

template <typename T>
static void Put(std::string& out, T value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

static void PutString(std::string& out, const std::string& s) {
    uint16_t length = std::min<size_t>(s.size(), UINT16_MAX);
    Put(out, length);
    out.append(s, 0, length);
}

class PayloadReader {
public:
    PayloadReader(const uint8_t* data, size_t size) : p_(data), left_(size) {}

    template <typename T>
    T Get() {
        T value{};
        if (left_ < sizeof(T)) {
            ok_ = false;
            return value;
        }
        memcpy(&value, p_, sizeof(T));
        p_ += sizeof(T);
        left_ -= sizeof(T);
        return value;
    }

    std::string GetString() {
        uint16_t length = Get<uint16_t>();
        if (left_ < length) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_), length);
        p_ += length;
        left_ -= length;
        return s;
    }

    bool ok() const { return ok_; }

private:
    const uint8_t* p_;
    size_t left_;
    bool ok_ = true;
};

static std::string Serialize(const AncsNotification& n, time_t received_time) {
    std::string out;
    out.reserve(64 + n.title.size() + n.subtitle.size() + n.message.size());
    Put<uint8_t>(out, kPayloadVersion);
    Put<uint8_t>(out, n.event_id);
    Put<uint8_t>(out, n.event_flags);
    Put<uint8_t>(out, n.category_id);
    Put<uint8_t>(out, n.category_count);
    Put<uint8_t>(out, n.has_attributes);
    Put<uint32_t>(out, n.uid);
    Put<int32_t>(out, n.message_size);
    Put<int64_t>(out, received_time);
    for (auto* s : {&n.peer_name, &n.peer_address, &n.app_id, &n.app_name, &n.title, &n.subtitle,
                    &n.message, &n.date, &n.positive_action, &n.negative_action}) {
        PutString(out, *s);
    }
    return out;
}

static bool Deserialize(const uint8_t* data, size_t size, StoredNotification* stored) {
    PayloadReader reader(data, size);
    if (reader.Get<uint8_t>() != kPayloadVersion) {
        return false;
    }
    auto& n = stored->notification;
    n.event_id = reader.Get<uint8_t>();
    n.event_flags = reader.Get<uint8_t>();
    n.category_id = reader.Get<uint8_t>();
    n.category_count = reader.Get<uint8_t>();
    n.has_attributes = reader.Get<uint8_t>() != 0;
    n.uid = reader.Get<uint32_t>();
    n.message_size = reader.Get<int32_t>();
    stored->received_time = reader.Get<int64_t>();
    for (auto* s : {&n.peer_name, &n.peer_address, &n.app_id, &n.app_name, &n.title, &n.subtitle,
                    &n.message, &n.date, &n.positive_action, &n.negative_action}) {
        *s = reader.GetString();
    }
    return reader.ok();
}

// Parses the record at data[0]. Returns its size, or 0 if there is none
// (*end_of_records set) or it is damaged.
static uint32_t ParseRecord(const uint8_t* data, size_t available, bool parse,
                            StoredNotification* stored, bool* valid, bool* end_of_records) {
    *valid = false;
    *end_of_records = false;
    RecordHeader header;
    if (available < sizeof(header)) {
        return 0;
    }
    memcpy(&header, data, sizeof(header));
    if (IsErased(&header, sizeof(header))) {
        *end_of_records = true;
        return 0;
    }
    uint32_t size = RecordSize(header.length);
    if (header.magic != kRecordMagic || size > available ||
        RecordCrc(header, data + sizeof(header)) != header.crc) {
        return 0;
    }
    stored->id = header.id;
    stored->upload_state = ToUploadState(header.upload_state);
    // A payload of an unknown version is skipped, the next record still counts
    *valid = !parse || Deserialize(data + sizeof(header), header.length, stored);
    return size;
}

NotificationStore::NotificationStore() {
    const esp_partition_t* partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
    if (partition == nullptr) {
        ESP_LOGI(TAG, "No \"%s\" partition, notifications are kept in RAM only", kPartitionLabel);
        return;
    }
    if (partition->encrypted) {
        // Encrypted writes cannot clear single bits in place
        ESP_LOGW(TAG, "Partition \"%s\" is encrypted, notifications are kept in RAM only", kPartitionLabel);
        return;
    }
    // The rest of the partition is left for other data
    sector_count_ = std::min<size_t>(partition->size, kMaxSize) / kSectorSize;
    if (sector_count_ < kMinSectors) {
        ESP_LOGW(TAG, "Partition \"%s\" is too small", kPartitionLabel);
        return;
    }
    ESP_LOGI(TAG, "Partition \"%s\" at 0x%lx, %lu KB, subtype 0x%02x, using %d KB", kPartitionLabel,
             (unsigned long)partition->address, (unsigned long)(partition->size / 1024), partition->subtype,
             (int)(sector_count_ * kSectorSize / 1024));
    partition_ = partition;
    sector_sequence_.assign(sector_count_, 0);
    buffer_.resize(kSectorSize);
    Scan();
}

void NotificationStore::Scan() {
    for (int sector = 0; sector < sector_count_; sector++) {
        SectorHeader header;
        esp_err_t err = esp_partition_read(partition_, sector * kSectorSize, &header, sizeof(header));
        if (err != ESP_OK) {
            errors_++;
            last_error_ = err;
            continue;
        }
        if (header.magic != kSectorMagic || header.sequence == 0 ||
            esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&header), offsetof(SectorHeader, crc)) != header.crc) {
            continue;
        }
        sector_sequence_[sector] = header.sequence;
        if (head_ < 0 || header.sequence > sequence_) {
            head_ = sector;
            sequence_ = header.sequence;
        }
    }

    int used_sectors = 0;
    int record_count = 0;
    std::vector<Record> records;
    for (int sector = 0; sector < sector_count_; sector++) {
        if (sector_sequence_[sector] == 0) {
            continue;
        }
        uint32_t end = 0;
        records.clear();
        if (!ReadSector(sector, &records, &end, false)) {
            continue;
        }
        used_sectors++;
        record_count += records.size();
        for (auto& record : records) {
            next_id_ = std::max(next_id_, record.stored.id + 1);
            if (record.stored.upload_state == UploadState::kPending) {
                pending_[record.stored.id] = record.offset;
            }
        }
        if (sector == head_) {
            write_offset_ = end;
        }
    }
    ESP_LOGI(TAG, "%d notifications (%d pending upload) in %d of %d sectors", record_count,
             (int)pending_.size(), used_sectors, sector_count_);
}

bool NotificationStore::ReadSector(int sector, std::vector<Record>* records, uint32_t* end, bool parse) {
    *end = 0;
    uint32_t base = sector * kSectorSize;
    esp_err_t err = esp_partition_read(partition_, base, buffer_.data(), kSectorSize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read sector %d: %s", sector, esp_err_to_name(err));
        errors_++;
        last_error_ = err;
        return false;
    }
    uint32_t pos = sizeof(SectorHeader);
    while (pos < kSectorSize) {
        Record record{base + pos, {}};
        bool valid;
        bool end_of_records;
        uint32_t size = ParseRecord(buffer_.data() + pos, kSectorSize - pos, parse, &record.stored, &valid,
                                    &end_of_records);
        if (end_of_records) {
            *end = pos;
            break;
        }
        if (size == 0) {
            // Torn write: the rest of the sector cannot be trusted or appended to
            ESP_LOGW(TAG, "Damaged record in sector %d at 0x%03lx", sector, (unsigned long)pos);
            break;
        }
        if (valid) {
            records->push_back(std::move(record));
        }
        pos += size;
    }
    return true;
}

bool NotificationStore::OpenNextSector() {
    int next = (head_ + 1) % sector_count_;
    uint32_t begin = next * kSectorSize;

    // The oldest records go; so do their uploads if still pending
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second >= begin && it->second < begin + kSectorSize) {
            ESP_LOGW(TAG, "Notification %lu overwritten before it was uploaded", (unsigned long)it->first);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    // Whatever happens, the next attempt moves on to the following sector
    head_ = next;
    write_offset_ = 0;
    sector_sequence_[next] = 0;

    esp_err_t err = esp_partition_erase_range(partition_, begin, kSectorSize);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase sector %d: %s", next, esp_err_to_name(err));
        errors_++;
        last_error_ = err;
        return false;
    }
    SectorHeader header = {kSectorMagic, sequence_ + 1, 0xFFFFFFFF, 0};
    header.crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&header), offsetof(SectorHeader, crc));
    err = esp_partition_write(partition_, begin, &header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write sector %d: %s", next, esp_err_to_name(err));
        errors_++;
        last_error_ = err;
        return false;
    }
    sequence_ = header.sequence;
    sector_sequence_[next] = sequence_;
    write_offset_ = sizeof(header);
    return true;
}

uint32_t NotificationStore::Append(const AncsNotification& notification, UploadState state, time_t received_time) {
    if (!IsEnabled()) {
        return 0;
    }
    std::string payload = Serialize(notification, received_time);
    uint32_t size = RecordSize(payload.size());
    if (size > kSectorSize - sizeof(SectorHeader)) {
        ESP_LOGE(TAG, "Notification too large to store (%u bytes)", (unsigned)payload.size());
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (head_ < 0 || write_offset_ == 0 || write_offset_ + size > kSectorSize) {
        if (!OpenNextSector()) {
            return 0;
        }
    }

    uint32_t id = next_id_++;
    RecordHeader header = {};
    header.magic = kRecordMagic;
    header.length = payload.size();
    header.id = id;
    header.crc = RecordCrc(header, reinterpret_cast<const uint8_t*>(payload.data()));
    header.upload_state = static_cast<uint8_t>(state);
    memset(header.reserved, 0xFF, sizeof(header.reserved));

    std::string data(reinterpret_cast<const char*>(&header), sizeof(header));
    data += payload;
    data.resize(size, '\xFF');

    uint32_t offset = head_ * kSectorSize + write_offset_;
    esp_err_t err = esp_partition_write(partition_, offset, data.data(), data.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write notification: %s", esp_err_to_name(err));
        errors_++;
        last_error_ = err;
        write_offset_ = 0;  // The rest of this sector is suspect
        return 0;
    }
    write_offset_ += size;
    if (state == UploadState::kPending) {
        pending_[id] = offset;
    }
    return id;
}

void NotificationStore::SetUploadState(uint32_t id, UploadState state) {
    if (!IsEnabled() || state == UploadState::kPending) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        return;
    }
    uint8_t value = static_cast<uint8_t>(state);
    esp_err_t err = esp_partition_write(partition_, it->second + offsetof(RecordHeader, upload_state), &value, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update notification %lu: %s", (unsigned long)id, esp_err_to_name(err));
        errors_++;
        last_error_ = err;
    }
    pending_.erase(it);
}

void NotificationStore::ForEach(const std::function<bool(const StoredNotification&)>& callback) {
    if (!IsEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> sectors;
    for (int sector = 0; sector < sector_count_; sector++) {
        if (sector_sequence_[sector] != 0) {
            sectors.push_back(sector);
        }
    }
    std::sort(sectors.begin(), sectors.end(), [this](int a, int b) {
        return sector_sequence_[a] > sector_sequence_[b];
    });

    std::vector<Record> records;
    for (int sector : sectors) {
        uint32_t end;
        records.clear();
        ReadSector(sector, &records, &end, true);
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (!callback(it->stored)) {
                return;
            }
        }
    }
}

NotificationStore::Stats NotificationStore::GetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats = {sector_count_, 0, head_, errors_, last_error_};
    for (uint32_t sequence : sector_sequence_) {
        stats.used_sectors += sequence != 0;
    }
    return stats;
}

std::vector<uint32_t> NotificationStore::GetPendingIds() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> ids;
    for (auto& [id, offset] : pending_) {
        ids.push_back(id);
    }
    return ids;
}

bool NotificationStore::ReadPending(uint32_t id, StoredNotification* record) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        return false;
    }
    // Records never span sectors: read up to the end of this one
    uint32_t available = kSectorSize - it->second % kSectorSize;
    esp_err_t err = esp_partition_read(partition_, it->second, buffer_.data(), available);
    if (err != ESP_OK) {
        errors_++;
        last_error_ = err;
        return false;
    }
    bool valid;
    bool end_of_records;
    return ParseRecord(buffer_.data(), available, true, record, &valid, &end_of_records) != 0 && valid &&
           record->id == id;
}
