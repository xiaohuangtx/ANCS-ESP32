#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>

/**
 * Answers every A query with the same address, so whatever name a phone looks
 * up on the provisioning hotspot leads to the setup page. Other query types
 * get an empty answer, so clients fall back to IPv4.
 */
class DnsServer {
public:
    DnsServer();
    ~DnsServer();
    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;

    // ip in network byte order (esp_ip4_addr_t::addr)
    bool Start(uint32_t ip);
    // Blocks until the task has exited
    void Stop();

private:
    void Run();

    int fd_ = -1;
    uint32_t ip_ = 0;
    std::atomic<bool> running_{false};
    SemaphoreHandle_t stopped_ = nullptr;
};

#endif // DNS_SERVER_H
