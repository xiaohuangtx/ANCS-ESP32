#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();
}

Application::~Application() {
    vEventGroupDelete(event_group_);
}

void Application::Initialize() {
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s starting, free heap %u", app_desc->project_name, app_desc->version,
             (unsigned)esp_get_free_heap_size());

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
        MAIN_EVENT_NETWORK_DISCONNECTED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            WebhookClient::GetInstance().SetNetworkReady(true);
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            WebhookClient::GetInstance().SetNetworkReady(false);
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
