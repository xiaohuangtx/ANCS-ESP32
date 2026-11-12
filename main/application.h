#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "notification.h"
#include "button.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_CLOCK_TICK           (1 << 1)

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

private:
    Application();
    ~Application();

    void InitializeButtons();
    void OnNotification(const AncsNotification& notification);
    void PrintHeapStats();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    std::unique_ptr<Button> boot_button_;
    // Fingerprints of the notifications reported so far, oldest first.
    // Only used by the main task.
    std::deque<size_t> reported_notifications_;
};

#endif // _APPLICATION_H_
