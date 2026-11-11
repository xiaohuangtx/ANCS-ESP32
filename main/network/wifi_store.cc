#include "wifi_store.h"

#include <esp_log.h>
#include <nvs.h>

static const char* TAG = "WifiStore";
static const char* kNamespace = "wifi";

// "ssid", "ssid1", ... "ssid9"
static std::string MakeKey(const char* prefix, int index) {
    return index == 0 ? prefix : prefix + std::to_string(index);
}

WifiStore::WifiStore() {
    Load();
}

void WifiStore::Load() {
    nvs_handle_t nvs;
    if (nvs_open(kNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No saved networks");
        return;
    }
    for (int i = 0; i < kMaxNetworks; i++) {
        char ssid[33];
        char password[65];
        size_t length = sizeof(ssid);
        if (nvs_get_str(nvs, MakeKey("ssid", i).c_str(), ssid, &length) != ESP_OK) {
            continue;
        }
        length = sizeof(password);
        if (nvs_get_str(nvs, MakeKey("password", i).c_str(), password, &length) != ESP_OK) {
            continue;
        }
        uint8_t channel = 0;
        nvs_get_u8(nvs, MakeKey("channel", i).c_str(), &channel);
        networks_.push_back({ssid, password, channel});
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "%u saved networks", (unsigned)networks_.size());
}

// Called with mutex_ held. Rewrites every slot so the list stays contiguous.
void WifiStore::Save() {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    for (int i = 0; i < kMaxNetworks; i++) {
        auto ssid_key = MakeKey("ssid", i);
        auto password_key = MakeKey("password", i);
        auto channel_key = MakeKey("channel", i);
        if (i < (int)networks_.size()) {
            nvs_set_str(nvs, ssid_key.c_str(), networks_[i].ssid.c_str());
            nvs_set_str(nvs, password_key.c_str(), networks_[i].password.c_str());
            nvs_set_u8(nvs, channel_key.c_str(), networks_[i].channel);
        } else {
            nvs_erase_key(nvs, ssid_key.c_str());
            nvs_erase_key(nvs, password_key.c_str());
            nvs_erase_key(nvs, channel_key.c_str());
        }
    }
    err = nvs_commit(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save networks: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

int WifiStore::IndexOf(const std::string& ssid) const {
    for (size_t i = 0; i < networks_.size(); i++) {
        if (networks_[i].ssid == ssid) {
            return i;
        }
    }
    return -1;
}

std::vector<WifiCredential> WifiStore::GetAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return networks_;
}

bool WifiStore::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return networks_.empty();
}

std::optional<WifiCredential> WifiStore::Find(const std::string& ssid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int index = IndexOf(ssid);
    if (index < 0) {
        return std::nullopt;
    }
    return networks_[index];
}

std::string WifiStore::Validate(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size() > 32) {
        return "Wi-Fi 名称长度必须为 1–32 字节";
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 64)) {
        return "密码长度必须为 8–64 位（开放网络留空）";
    }
    return "";
}

static std::string FullMessage() {
    return "最多保存 " + std::to_string(WifiStore::kMaxNetworks) + " 个 Wi-Fi，请先删除不用的";
}

std::string WifiStore::Add(const std::string& ssid, const std::string& password) {
    auto error = Validate(ssid, password);
    if (!error.empty()) {
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (IndexOf(ssid) >= 0) {
        return "已保存过 " + ssid;
    }
    if ((int)networks_.size() >= kMaxNetworks) {
        return FullMessage();
    }
    networks_.push_back({ssid, password, 0});
    Save();
    ESP_LOGI(TAG, "Network %s added", ssid.c_str());
    return "";
}

std::string WifiStore::Update(const std::string& ssid, const std::string& new_ssid, const std::string* password) {
    std::lock_guard<std::mutex> lock(mutex_);
    int index = IndexOf(ssid);
    if (index < 0) {
        return "未找到 " + ssid;
    }
    auto& network = networks_[index];
    std::string new_password = password ? *password : network.password;
    auto error = Validate(new_ssid, new_password);
    if (!error.empty()) {
        return error;
    }
    if (new_ssid != ssid) {
        if (IndexOf(new_ssid) >= 0) {
            return "已保存过 " + new_ssid;
        }
        // A different network: the saved channel no longer applies
        network.ssid = new_ssid;
        network.channel = 0;
    }
    network.password = new_password;
    Save();
    ESP_LOGI(TAG, "Network %s updated", new_ssid.c_str());
    return "";
}

std::string WifiStore::Remove(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(mutex_);
    int index = IndexOf(ssid);
    if (index < 0) {
        return "未找到 " + ssid;
    }
    networks_.erase(networks_.begin() + index);
    Save();
    ESP_LOGI(TAG, "Network %s removed", ssid.c_str());
    return "";
}

std::string WifiStore::CheckPut(const std::string& ssid, const std::string& password) const {
    auto error = Validate(ssid, password);
    if (!error.empty()) {
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (IndexOf(ssid) < 0 && (int)networks_.size() >= kMaxNetworks) {
        return FullMessage();
    }
    return "";
}

std::string WifiStore::Put(const std::string& ssid, const std::string& password, uint8_t channel) {
    auto error = CheckPut(ssid, password);
    if (!error.empty()) {
        return error;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    int index = IndexOf(ssid);
    if (index >= 0) {
        networks_[index].password = password;
        networks_[index].channel = channel;
    } else {
        networks_.push_back({ssid, password, channel});
    }
    Save();
    ESP_LOGI(TAG, "Network %s saved", ssid.c_str());
    return "";
}

void WifiStore::SetChannel(const std::string& ssid, uint8_t channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    int index = IndexOf(ssid);
    if (index < 0 || channel == 0 || networks_[index].channel == channel) {
        return;
    }
    networks_[index].channel = channel;
    Save();
}
