#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "notification.h"
#include "button.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * Starts the webhook task, Wi-Fi (asynchronously) and BLE.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     */
    void Run();

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    // For the admin page: where notifications get stuck, if anywhere
    struct Diagnostics {
        uint32_t notifications_received;  // From BLE
        uint32_t notifications_handled;   // Recorded and sent to the webhook
        uint32_t notifications_skipped;   // Replays already reported
        uint32_t notifications_dropped;   // By a filter rule
        int pending_tasks;                // Waiting for the main task
        int busy_seconds;                 // Time the main task spent on the current task batch, 0 when idle
    };
    Diagnostics GetDiagnostics();

private:
    Application();
    ~Application();

    void InitializeButtons();
    void LoadReportedNotifications();
    void ResendPendingNotifications();
    void OnNotification(const AncsNotification& notification);
    // Webhook target slots (bit mask) a notification goes to, by the filter rules
    uint32_t GetWebhookTargets(const AncsNotification& notification);

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::atomic<uint32_t> notifications_received_ = 0;
    std::atomic<uint32_t> notifications_handled_ = 0;
    std::atomic<uint32_t> notifications_skipped_ = 0;
    std::atomic<uint32_t> notifications_dropped_ = 0;
    std::atomic<int64_t> busy_since_us_ = 0;  // 0 when idle
    EventGroupHandle_t event_group_ = nullptr;
    std::unique_ptr<Button> boot_button_;
    // Fingerprints of the notifications reported so far, oldest first, also
    // from before the reboot when stored in flash. Only used by the main task.
    std::deque<size_t> reported_notifications_;
};

#endif // _APPLICATION_H_
