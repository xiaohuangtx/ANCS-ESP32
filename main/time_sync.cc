#include "time_sync.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdlib>

#define TAG "TimeSync"

#define TIME_EVENT_SET (1 << 0)

// Earlier than this, the clock has not been set
static constexpr time_t kMinValidTime = 1704067200;  // 2024-01-01
// After an SNTP sync, the iPhone's clock (whole seconds over BLE) is not used for this long
static constexpr time_t kNtpPreferredSeconds = 2 * 3600;
static constexpr int kMinUtcOffset = -12 * 60;
static constexpr int kMaxUtcOffset = 14 * 60;

const char* TimeSourceName(TimeSync::Source source) {
    switch (source) {
        case TimeSync::Source::kNone: return "none";
        case TimeSync::Source::kIphone: return "iphone";
        case TimeSync::Source::kNtp: return "ntp";
    }
    return "none";
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm)
static int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// POSIX TZ for a fixed offset; the sign is inverted there ("UTC-8" is UTC+8)
static std::string MakeTz(int utc_offset) {
    int minutes = std::abs(utc_offset);
    char tz[16];
    if (minutes % 60 == 0) {
        snprintf(tz, sizeof(tz), "UTC%c%d", utc_offset >= 0 ? '-' : '+', minutes / 60);
    } else {
        snprintf(tz, sizeof(tz), "UTC%c%d:%02d", utc_offset >= 0 ? '-' : '+', minutes / 60, minutes % 60);
    }
    return tz;
}

TimeSync::TimeSync() {
    event_group_ = xEventGroupCreate();
}

void TimeSync::Initialize() {
    Settings settings("time");
    int32_t saved = settings.GetInt("utc_off", INT32_MIN);
    if (saved != INT32_MIN) {
        SetUtcOffset(saved, true);
    } else {
        SetUtcOffset(CONFIG_TIME_DEFAULT_UTC_OFFSET, false);
    }
    if (IsClockSet()) {
        // Kept across a software reset by the RTC
        xEventGroupSetBits(event_group_, TIME_EVENT_SET);
    }
}

void TimeSync::StartNtp() {
#if CONFIG_TIME_SYNC_NTP
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ntp_started_) {
            return;
        }
        ntp_started_ = true;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TIME_NTP_SERVER);
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    if (CONFIG_TIME_NTP_SERVER_2[0] != '\0') {
        config.servers[1] = CONFIG_TIME_NTP_SERVER_2;
        config.num_of_servers = 2;
    }
#endif
    config.wait_for_sync = false;
    config.sync_cb = OnNtpSync;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        std::lock_guard<std::mutex> lock(mutex_);
        ntp_started_ = false;
        return;
    }
    ESP_LOGI(TAG, "SNTP started (%s)", CONFIG_TIME_NTP_SERVER);
#endif
}

void TimeSync::OnNtpSync(struct timeval* tv) {
    auto& self = GetInstance();
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.last_ntp_sync_ = tv->tv_sec;
    }
    self.MarkSynced(Source::kNtp);
}

void TimeSync::OnIphoneTime(const std::string& current_time, const std::string& local_time_info,
                            const std::string& peer_name) {
    // Local Time Information: time zone (sint8) and DST offset (uint8), both in
    // 15 minute steps; -128 / 255 mean unknown
    if (local_time_info.size() >= 2 && (int8_t)local_time_info[0] != -128) {
        int offset = (int8_t)local_time_info[0] * 15;
        uint8_t dst = local_time_info[1];
        if (dst != 255) {
            offset += dst * 15;
        }
        SetUtcOffset(offset, true);
    }

    // Current Time: year (uint16), month, day, hours, minutes, seconds, day of
    // week, 1/256 seconds, adjust reason. In local time.
    if (current_time.size() < 7) {
        ESP_LOGW(TAG, "Invalid Current Time from \"%s\" (%u bytes)", peer_name.c_str(), (unsigned)current_time.size());
        return;
    }
    auto* p = reinterpret_cast<const uint8_t*>(current_time.data());
    int year = p[0] | (p[1] << 8);
    int month = p[2], day = p[3], hours = p[4], minutes = p[5], seconds = p[6];
    int fractions = current_time.size() >= 9 ? p[8] : 0;
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hours > 23 || minutes > 59 || seconds > 59) {
        ESP_LOGW(TAG, "Current Time from \"%s\" not usable: %04d-%02d-%02d %02d:%02d:%02d", peer_name.c_str(),
                 year, month, day, hours, minutes, seconds);
        return;
    }

    int utc_offset;
    time_t last_ntp_sync;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        utc_offset = utc_offset_;
        last_ntp_sync = last_ntp_sync_;
    }
    time_t local = DaysFromCivil(year, month, day) * 86400 + hours * 3600 + minutes * 60 + seconds;
    time_t utc = local - utc_offset * 60;

    time_t now = time(nullptr);
    if (last_ntp_sync != 0 && now - last_ntp_sync < kNtpPreferredSeconds) {
        ESP_LOGI(TAG, "Clock synced by SNTP, iPhone time only used for the time zone (off by %lld s)",
                 (long long)(utc - now));
        return;
    }

    struct timeval tv = {};
    tv.tv_sec = utc;
    tv.tv_usec = fractions * 1000000 / 256;
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "Clock set from \"%s\": %04d-%02d-%02d %02d:%02d:%02d (UTC%+d:%02d)", peer_name.c_str(), year,
             month, day, hours, minutes, seconds, utc_offset / 60, std::abs(utc_offset) % 60);
    MarkSynced(Source::kIphone);
}

void TimeSync::MarkSynced(Source source) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = source;
        last_sync_ = time(nullptr);
    }
    if (source == Source::kNtp) {
        ESP_LOGI(TAG, "Clock synced by SNTP");
    }
    xEventGroupSetBits(event_group_, TIME_EVENT_SET);
}

void TimeSync::SetUtcOffset(int minutes, bool from_iphone) {
    if (minutes < kMinUtcOffset || minutes > kMaxUtcOffset) {
        ESP_LOGW(TAG, "Ignoring UTC offset %d minutes", minutes);
        return;
    }
    bool save = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed = minutes != utc_offset_ || from_iphone != offset_from_iphone_;
        save = from_iphone && (minutes != utc_offset_ || !offset_from_iphone_);
        utc_offset_ = minutes;
        offset_from_iphone_ = from_iphone;
        if (!changed && getenv("TZ") != nullptr) {
            return;
        }
        setenv("TZ", MakeTz(minutes).c_str(), 1);
        tzset();
    }
    ESP_LOGI(TAG, "Time zone UTC%+d:%02d%s", minutes / 60, std::abs(minutes) % 60, from_iphone ? " (from iPhone)" : "");
    if (save) {
        Settings settings("time", true);
        settings.SetInt("utc_off", minutes);
    }
}

bool TimeSync::IsClockSet() {
    return time(nullptr) >= kMinValidTime;
}

bool TimeSync::WaitUntilSet(TickType_t timeout) {
    if (IsClockSet()) {
        return true;
    }
    xEventGroupWaitBits(event_group_, TIME_EVENT_SET, pdFALSE, pdTRUE, timeout);
    return IsClockSet();
}

TimeSync::Status TimeSync::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {source_, last_sync_, utc_offset_, offset_from_iphone_};
}
