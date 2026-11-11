#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>

#define TAG "Application"

// Heap statistics interval
static constexpr int kClockTickSeconds = 60;

Application::Application() {
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            auto* app = static_cast<Application*>(arg);
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

void Application::Initialize() {
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s starting", app_desc->project_name, app_desc->version);
    PrintHeapStats();

    InitializeButtons();

    // Webhook task waits for Wi-Fi before sending anything
    WebhookClient::GetInstance().Start();

    // Wi-Fi: connect to a stored network, or open the provisioning hotspot
    auto& wifi = WifiService::GetInstance();
    wifi.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        if (event == NetworkEvent::Connected) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
        } else if (event == NetworkEvent::Disconnected) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
        }
    });
    wifi.Start();

    // BLE: advertise and wait for the iPhone
    auto& ble = BleManager::GetInstance();
    ble.SetConnectionCallback([](bool connected, int connection_count) {
        ESP_LOGI(TAG, "Device %s, %d/%d connected", connected ? "connected" : "disconnected",
                 connection_count, BleManager::kMaxConnections);
    });
    ble.SetNotificationCallback([this](const AncsNotification& notification) {
        // Called from the NimBLE host task: hand over to the main task
        Schedule([this, notification]() {
            OnNotification(notification);
        });
    });
    ble.Initialize(CONFIG_BLE_DEVICE_NAME);

    esp_timer_start_periodic(clock_timer_handle_, kClockTickSeconds * 1000000ULL);
}

void Application::PrintHeapStats() {
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Heap: internal free %u KB (min %u KB) / %u KB, PSRAM free %u KB / %u KB",
             (unsigned)(internal_free / 1024), (unsigned)(internal_min / 1024), (unsigned)(internal_total / 1024),
             (unsigned)(psram_free / 1024), (unsigned)(psram_total / 1024));
}

void Application::InitializeButtons() {
#if CONFIG_BOOT_BUTTON_GPIO >= 0
    boot_button_ = std::make_unique<Button>(static_cast<gpio_num_t>(CONFIG_BOOT_BUTTON_GPIO));
    boot_button_->OnLongPress([this]() {
        Schedule([]() {
            WifiService::GetInstance().EnterWifiConfigMode();
        });
    });
    boot_button_->OnMultipleClick([]() {
        BleManager::GetInstance().ClearBonds();
    }, 3);
#endif
}

void Application::OnNotification(const AncsNotification& notification) {
    notification.Print();
    WebhookClient::GetInstance().Post(notification.ToJson());
}

void Application::Run() {
    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_CLOCK_TICK;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            WebhookClient::GetInstance().SetNetworkReady(true);
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            WebhookClient::GetInstance().SetNetworkReady(false);
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            PrintHeapStats();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}
