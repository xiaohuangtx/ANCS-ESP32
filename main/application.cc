#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"
#include "notification_history.h"
#include "web_server.h"
#include "ancs_defs.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>

#include <algorithm>

#define TAG "Application"

// Heap statistics interval
static constexpr int kClockTickSeconds = 60;
// Notifications remembered to recognize them when iOS replays them
static constexpr size_t kMaxReportedNotifications = 256;

// Identifies a notification across reconnections: the uid alone could repeat
// after the iPhone restarts, so the content is part of it
static size_t Fingerprint(const AncsNotification& notification) {
    std::string key = notification.peer_address;
    for (auto* field : {&notification.app_id, &notification.date, &notification.title,
                        &notification.subtitle, &notification.message}) {
        key += '\0';
        key += *field;
    }
    key += '\0';
    key += std::to_string(notification.uid);
    return std::hash<std::string>{}(key);
}

// "<prefix>-XXXXXX" with the last 3 bytes of the base MAC, so the BLE name and
// the provisioning hotspot SSID are the same
static std::string MakeDeviceName(const char* prefix) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    char name[33];
    snprintf(name, sizeof(name), "%s-%02X%02X%02X", prefix, mac[3], mac[4], mac[5]);
    return name;
}

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
        // Handled right here rather than in the main task. Entering config mode
        // emits Disconnected just before the hotspot starts its own server on
        // port 80, so ours must be gone by then. And a quick disconnect +
        // reconnect must reach the webhook in order, which two event bits
        // handled together in the main loop would not guarantee.
        if (event == NetworkEvent::Connected) {
            WebServer::GetInstance().Start();
            WebhookClient::GetInstance().SetNetworkReady(true);
        } else if (event == NetworkEvent::Disconnected) {
            WebServer::GetInstance().Stop();
            WebhookClient::GetInstance().SetNetworkReady(false);
        }
    });
    wifi.Start(MakeDeviceName(CONFIG_WIFI_AP_SSID_PREFIX));

    // BLE: advertise and wait for the iPhone
    auto& ble = BleManager::GetInstance();
    ble.SetConnectionCallback([](bool connected, int connection_count) {
        ESP_LOGI(TAG, "Device %s, %d/%d connected", connected ? "connected" : "disconnected",
                 connection_count, BleManager::GetInstance().GetMaxConnections());
    });
    ble.SetNotificationCallback([this](const AncsNotification& notification) {
        // Called from the NimBLE host task: hand over to the main task
        Schedule([this, notification]() {
            OnNotification(notification);
        });
    });
    ble.Initialize(MakeDeviceName(CONFIG_BLE_DEVICE_NAME_PREFIX));

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
    // iOS replays Notification Center, flagged PreExisting, on every connection.
    // Drop the ones already reported over an earlier connection; pre-existing
    // ones not seen yet (e.g. received while disconnected) are still reported.
    size_t fingerprint = Fingerprint(notification);
    bool reported = std::find(reported_notifications_.begin(), reported_notifications_.end(), fingerprint) !=
                    reported_notifications_.end();
    if (reported && (notification.event_flags & kAncsFlagPreExisting)) {
        ESP_LOGI(TAG, "Skipping pre-existing notification uid=%lu from %s, already reported",
                 (unsigned long)notification.uid, notification.peer_name.c_str());
        return;
    }
    if (!reported) {
        reported_notifications_.push_back(fingerprint);
        if (reported_notifications_.size() > kMaxReportedNotifications) {
            reported_notifications_.pop_front();
        }
    }

    notification.Print();
    NotificationHistory::GetInstance().Add(notification);
    WebhookClient::GetInstance().Post(notification.ToJson());
}

void Application::Run() {
    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_CLOCK_TICK;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

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
