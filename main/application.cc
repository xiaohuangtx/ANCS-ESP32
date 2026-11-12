#include "application.h"
#include "ble_manager.h"
#include "wifi_service.h"
#include "webhook_client.h"
#include "notification_history.h"
#include "notification_store.h"
#include "web_server.h"
#include "ancs_defs.h"

#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_mac.h>

#include <algorithm>

#define TAG "Application"

// Notifications remembered to recognize them when iOS replays them
static constexpr size_t kMaxReportedNotifications = 256;
// Uploads left pending by a reboot that are sent again; older ones count as
// failed, so they do not crowd out new notifications in the webhook queue
static constexpr size_t kMaxResentNotifications = std::max(1, CONFIG_WEBHOOK_QUEUE_SIZE / 2);

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
}

Application::~Application() {
    vEventGroupDelete(event_group_);
}

void Application::Initialize() {
    auto app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s starting", app_desc->project_name, app_desc->version);

    InitializeButtons();

    // Reload what survived the reboot, so a replay of Notification Center
    // is still recognized
    NotificationHistory::GetInstance();
    LoadReportedNotifications();

    // Webhook task waits for Wi-Fi before sending anything
    auto& webhook = WebhookClient::GetInstance();
    webhook.SetResultCallback([](uint32_t id, UploadState state) {
        NotificationHistory::GetInstance().SetUploadState(id, state);
    });
    webhook.Start();
    ResendPendingNotifications();

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
        notifications_received_++;
        Schedule([this, notification]() {
            OnNotification(notification);
        });
    });
    ble.Initialize(MakeDeviceName(CONFIG_BLE_DEVICE_NAME_PREFIX));
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

void Application::LoadReportedNotifications() {
    NotificationStore::GetInstance().ForEach([this](const StoredNotification& stored) {
        reported_notifications_.push_front(Fingerprint(stored.notification));
        return reported_notifications_.size() < kMaxReportedNotifications;
    });
}

void Application::ResendPendingNotifications() {
    auto& store = NotificationStore::GetInstance();
    auto& webhook = WebhookClient::GetInstance();
    auto ids = store.GetPendingIds();  // Oldest first
    if (ids.empty()) {
        return;
    }
    ESP_LOGI(TAG, "%d notifications were not uploaded before the reboot", (int)ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        StoredNotification stored;
        if (ids.size() - i <= kMaxResentNotifications && store.ReadPending(ids[i], &stored)) {
            webhook.Post(ids[i], stored.notification.ToJson());
        } else {
            NotificationHistory::GetInstance().SetUploadState(ids[i], UploadState::kFailed);
        }
    }
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
        notifications_skipped_++;
        return;
    }
    if (!reported) {
        reported_notifications_.push_back(fingerprint);
        if (reported_notifications_.size() > kMaxReportedNotifications) {
            reported_notifications_.pop_front();
        }
    }

    notification.Print();
    // Print() goes to the console only; leave a trace in the admin page log
    ESP_LOGI(TAG, "Notification uid=%lu from %s, app %s", (unsigned long)notification.uid,
             notification.peer_name.c_str(), notification.app_id.c_str());
    auto& webhook = WebhookClient::GetInstance();
    uint32_t id = NotificationHistory::GetInstance().Add(
        notification, webhook.IsEnabled() ? UploadState::kPending : UploadState::kSkipped);
    webhook.Post(id, notification.ToJson());
    notifications_handled_++;
}

void Application::Run() {
    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            busy_since_us_ = esp_timer_get_time();
            for (auto& task : tasks) {
                task();
            }
            busy_since_us_ = 0;
        }
    }
}

Application::Diagnostics Application::GetDiagnostics() {
    Diagnostics diagnostics = {};
    diagnostics.notifications_received = notifications_received_;
    diagnostics.notifications_handled = notifications_handled_;
    diagnostics.notifications_skipped = notifications_skipped_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics.pending_tasks = main_tasks_.size();
    }
    int64_t busy_since = busy_since_us_;
    if (busy_since != 0) {
        diagnostics.busy_seconds = (esp_timer_get_time() - busy_since) / 1000000;
    }
    return diagnostics;
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}
