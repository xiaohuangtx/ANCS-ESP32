#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <freertos/FreeRTOS.h>

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Keeps the latest console output in RAM (PSRAM when present) so it can be
 * read on the admin page of a device without a serial console.
 *
 * stdout / stderr are replaced by streams that write to the console as before
 * and copy every byte here, so ESP_LOGx and printf() show up exactly as on the
 * serial port. Not captured: output before Start() (ROM, bootloader, system
 * startup) and raw printf() from tasks created before Start() (esp_timer, ipc).
 * Thread-safe.
 */
class LogBuffer {
public:
    static LogBuffer& GetInstance() {
        static LogBuffer instance;
        return instance;
    }
    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;

    // Call once, first thing in app_main
    void Start();

    struct ReadResult {
        std::string text;  // Oldest first, ANSI color codes removed
        uint64_t next;     // Pass as "since" to get only what comes after
        bool reset;        // From the oldest line kept rather than from "since"
    };
    // Output written since position "since" (bytes written since boot). When
    // that part is gone or "since" is from another boot, everything kept.
    ReadResult Read(uint64_t since);
    // Random per boot: positions from another boot mean nothing
    uint32_t GetBootId() const { return boot_id_; }

private:
    LogBuffer() = default;

    static int Write(void* cookie, const char* data, int length);
    static int Vprintf(const char* format, va_list args);
    void Append(const char* data, size_t length);

    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    char* buffer_ = nullptr;
    size_t size_ = 0;
    uint64_t total_ = 0;  // Bytes written since Start(); the next one goes to total_ % size_
    uint32_t boot_id_ = 0;
    int console_fd_ = -1;
    FILE* stdout_ = nullptr;
};

#endif // LOG_BUFFER_H
