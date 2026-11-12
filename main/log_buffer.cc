#include "log_buffer.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_rom_caps.h>
#include <sdkconfig.h>
#include <sys/reent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if ESP_ROM_NEEDS_SWSETUP_WORKAROUND
extern "C" int __swsetup_r(struct _reent*, FILE*);
#endif

static constexpr size_t kPsramSize = 32 * 1024;
static constexpr size_t kInternalSize = 8 * 1024;

void LogBuffer::Start() {
#if CONFIG_LIBC_PICOLIBC
    ESP_LOGW("LogBuffer", "Not supported with picolibc, the admin page shows no log");
#else
    if (buffer_ != nullptr) {
        return;
    }
    buffer_ = static_cast<char*>(heap_caps_malloc(kPsramSize, MALLOC_CAP_SPIRAM));
    size_ = kPsramSize;
    if (buffer_ == nullptr) {
        buffer_ = static_cast<char*>(heap_caps_malloc(kInternalSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        size_ = kInternalSize;
    }
    if (buffer_ == nullptr) {
        return;
    }
    boot_id_ = esp_random();

    FILE* console = _REENT_STDOUT(_GLOBAL_REENT);
    fflush(console);
    console_fd_ = fileno(console);
    stdout_ = fwopen(this, Write);
    FILE* err = fwopen(this, Write);
    if (stdout_ == nullptr || err == nullptr) {
        return;
    }
    setvbuf(stdout_, nullptr, _IOLBF, 256);
    setvbuf(err, nullptr, _IONBF, 0);
#if ESP_ROM_NEEDS_SWSETUP_WORKAROUND
    // As esp_libc_init_global_stdio() does: keeps the ROM printf (32-bit
    // time_t) from setting up these streams itself
    __swsetup_r(_GLOBAL_REENT, stdout_);
    __swsetup_r(_GLOBAL_REENT, err);
#endif
    // Tasks copy the global streams when they are created...
    _REENT_STDOUT(_GLOBAL_REENT) = stdout_;
    _REENT_STDERR(_GLOBAL_REENT) = err;
    // ...so the running one is set by hand
    stdout = stdout_;
    stderr = err;
    // ESP_LOGx from tasks that existed before (esp_timer) still gets here
    esp_log_set_vprintf(Vprintf);

    static const char kNote[] = "--- Earlier boot messages are only on the serial console ---\n";
    Append(kNote, sizeof(kNote) - 1);
#endif
}

int LogBuffer::Write(void* cookie, const char* data, int length) {
    auto* self = static_cast<LogBuffer*>(cookie);
    // The console driver converts line endings, as for the original stream
    for (int done = 0; done < length;) {
        int n = write(self->console_fd_, data + done, length - done);
        if (n <= 0) {
            break;
        }
        done += n;
    }
    self->Append(data, length);
    return length;
}

int LogBuffer::Vprintf(const char* format, va_list args) {
    return vfprintf(GetInstance().stdout_, format, args);
}

void LogBuffer::Append(const char* data, size_t length) {
    taskENTER_CRITICAL(&lock_);
    for (size_t i = 0; i < length; i++) {
        buffer_[total_++ % size_] = data[i];
    }
    taskEXIT_CRITICAL(&lock_);
}

LogBuffer::ReadResult LogBuffer::Read(uint64_t since) {
    ReadResult result = {{}, since, false};
    if (buffer_ == nullptr) {
        return result;
    }
    taskENTER_CRITICAL(&lock_);
    uint64_t total = total_;
    taskEXIT_CRITICAL(&lock_);
    uint64_t oldest = total > size_ ? total - size_ : 0;
    result.reset = since < oldest || since > total;
    uint64_t start = result.reset ? oldest : since;
    result.next = total;

    std::string raw;
    raw.reserve(total - start);
    // Copy out in chunks so the critical section stays short
    static constexpr size_t kChunk = 512;
    char chunk[kChunk];
    for (uint64_t pos = start; pos < total; pos += kChunk) {
        size_t n = std::min<uint64_t>(kChunk, total - pos);
        taskENTER_CRITICAL(&lock_);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = buffer_[(pos + i) % size_];
        }
        taskEXIT_CRITICAL(&lock_);
        raw.append(chunk, n);
    }
    // Output written meanwhile may have overwritten the start of the copy
    taskENTER_CRITICAL(&lock_);
    uint64_t now_oldest = total_ > size_ ? total_ - size_ : 0;
    taskEXIT_CRITICAL(&lock_);
    if (now_oldest > start) {
        raw.erase(0, std::min<uint64_t>(now_oldest - start, raw.size()));
        start = now_oldest;
        result.reset = true;
    }
    if (result.reset && start > 0) {
        // The first line was partly overwritten
        size_t newline = raw.find('\n');
        raw.erase(0, newline == std::string::npos ? raw.size() : newline + 1);
    }

    // Strip "\033[0;32m" style color codes
    result.text.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\033') {
            while (i < raw.size() && raw[i] != 'm') {
                i++;
            }
            continue;
        }
        result.text += raw[i];
    }
    return result;
}
