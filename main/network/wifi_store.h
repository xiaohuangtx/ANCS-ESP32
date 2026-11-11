#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct WifiCredential {
    std::string ssid;
    std::string password;  // "" for an open network
    uint8_t channel = 0;   // Last channel seen, 0 = unknown
};

/**
 * Saved Wi-Fi networks, kept in NVS namespace "wifi" as ssid / password /
 * channel, ssid1 / password1 / channel1, ... ssid9 (the layout used by
 * older firmware, so networks saved by it are kept).
 *
 * Thread-safe. The setters return an error message, or "" on success.
 */
class WifiStore {
public:
    static constexpr int kMaxNetworks = 10;

    WifiStore();

    std::vector<WifiCredential> GetAll() const;
    bool IsEmpty() const;
    std::optional<WifiCredential> Find(const std::string& ssid) const;

    std::string Add(const std::string& ssid, const std::string& password);
    // password == nullptr keeps the current one
    std::string Update(const std::string& ssid, const std::string& new_ssid, const std::string* password);
    std::string Remove(const std::string& ssid);
    // Adds the network, or replaces its password if it is already saved
    std::string Put(const std::string& ssid, const std::string& password, uint8_t channel);
    // Whether Put() would succeed
    std::string CheckPut(const std::string& ssid, const std::string& password) const;
    void SetChannel(const std::string& ssid, uint8_t channel);

    static std::string Validate(const std::string& ssid, const std::string& password);

private:
    void Load();
    void Save();
    int IndexOf(const std::string& ssid) const;

    mutable std::mutex mutex_;
    std::vector<WifiCredential> networks_;
};

#endif // WIFI_STORE_H
