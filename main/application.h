#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "notification.h"
#include "button.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 1)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 2)

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

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    std::unique_ptr<Button> boot_button_;
};

#endif // _APPLICATION_H_
