#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <ctime>
#include <mutex>
#include <string>

/**
 * Keeps the system clock and time zone set, from two sources:
 *
 * - The iPhone: iOS offers the Current Time Service (0x1805) to bonded
 *   devices. AncsClient reads Current Time (local time) and Local Time
 *   Information (time zone + DST) on every connection, so the clock is set
 *   even without Wi-Fi and the time zone follows the iPhone.
 * - SNTP, while Wi-Fi is connected: more accurate, and keeps the clock right
 *   over long connections. When SNTP synced recently, the iPhone only updates
 *   the time zone.
 *
 * The UTC offset learned from the iPhone is saved in NVS (namespace "time",
 * key "utc_off", minutes), so local times are right after a reboot too. Until
 * then CONFIG_TIME_DEFAULT_UTC_OFFSET applies. All methods are thread-safe.
 */
class TimeSync {
public:
    enum class Source {
        kNone,
        kIphone,
        kNtp,
    };

    static TimeSync& GetInstance() {
        static TimeSync instance;
        return instance;
    }
    TimeSync(const TimeSync&) = delete;
    TimeSync& operator=(const TimeSync&) = delete;

    // Applies the saved time zone. Call early, before anything formats a local time.
    void Initialize();
    // After Wi-Fi connected; SNTP keeps running from then on
    void StartNtp();

    // Values of the Current Time (0x2A2B) and Local Time Information (0x2A0F)
    // characteristics; the latter is empty if the iPhone did not provide it
    void OnIphoneTime(const std::string& current_time, const std::string& local_time_info,
                      const std::string& peer_name);

    // Whether the clock holds a real date
    static bool IsClockSet();
    // Blocks until the clock is set, or the timeout passes
    bool WaitUntilSet(TickType_t timeout);

    struct Status {
        Source source;     // Of the last sync
        time_t last_sync;  // 0: never
        int utc_offset;    // Minutes east of UTC
        bool offset_from_iphone;
    };
    Status GetStatus();

private:
    TimeSync();
    void SetUtcOffset(int minutes, bool from_iphone);
    void MarkSynced(Source source);
    static void OnNtpSync(struct timeval* tv);

    std::mutex mutex_;
    EventGroupHandle_t event_group_ = nullptr;
    bool ntp_started_ = false;
    Source source_ = Source::kNone;
    time_t last_sync_ = 0;
    time_t last_ntp_sync_ = 0;
    int utc_offset_ = 0;
    bool offset_from_iphone_ = false;
};

const char* TimeSourceName(TimeSync::Source source);

#endif // TIME_SYNC_H
