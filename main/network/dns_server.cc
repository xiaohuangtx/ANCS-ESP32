#include "dns_server.h"

#include <esp_log.h>
#include <lwip/sockets.h>

#include <cstring>

static const char* TAG = "DnsServer";

static constexpr int kPort = 53;
static constexpr int kHeaderSize = 12;
static constexpr int kAnswerSize = 16;
static constexpr int kTtlSeconds = 60;
// recvfrom() wakes up this often to check whether Stop() was called
static constexpr int kPollTimeoutMs = 500;

static uint16_t ReadU16(const uint8_t* p) {
    return (p[0] << 8) | p[1];
}

// Turns the query in packet into a response in place. Returns its length, or
// -1 if the packet should be ignored.
static int BuildResponse(uint8_t* packet, int length, int capacity, uint32_t ip) {
    if (length < kHeaderSize) {
        return -1;
    }
    bool is_query = (packet[2] & 0x80) == 0;
    int opcode = (packet[2] >> 3) & 0x0F;
    if (!is_query || opcode != 0 || ReadU16(packet + 4) != 1) {
        return -1;
    }

    // Question name: a sequence of labels ending with a zero byte
    int pos = kHeaderSize;
    while (pos < length && packet[pos] != 0) {
        if ((packet[pos] & 0xC0) != 0) {
            return -1;  // Compression pointers are not expected in a question
        }
        pos += packet[pos] + 1;
    }
    pos += 1;
    if (pos + 4 > length) {
        return -1;
    }
    uint16_t qtype = ReadU16(packet + pos);
    uint16_t qclass = ReadU16(packet + pos + 2);
    // Everything after the question (e.g. an EDNS record) is dropped
    pos += 4;

    bool answer = qtype == 1 /* A */ && qclass == 1 /* IN */;
    if (answer && pos + kAnswerSize > capacity) {
        return -1;
    }

    packet[2] = 0x84 | (packet[2] & 0x01);  // Response, authoritative, keep RD
    packet[3] = 0x80;                       // RA, no error
    packet[6] = 0;
    packet[7] = answer ? 1 : 0;             // Answer count
    memset(packet + 8, 0, 4);               // No authority / additional records
    if (!answer) {
        return pos;
    }

    const uint8_t record[] = {
        0xC0, 0x0C,              // Name: pointer to the question
        0x00, 0x01, 0x00, 0x01,  // Type A, class IN
        0x00, 0x00, 0x00, kTtlSeconds,
        0x00, 0x04,              // Data length
    };
    memcpy(packet + pos, record, sizeof(record));
    memcpy(packet + pos + sizeof(record), &ip, 4);
    return pos + kAnswerSize;
}

DnsServer::DnsServer() {
    stopped_ = xSemaphoreCreateBinary();
}

DnsServer::~DnsServer() {
    Stop();
    vSemaphoreDelete(stopped_);
}

bool DnsServer::Start(uint32_t ip) {
    if (running_) {
        return true;
    }
    ip_ = ip;

    fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        ESP_LOGE(TAG, "Failed to create socket, errno %d", errno);
        return false;
    }
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = kPollTimeoutMs * 1000,
    };
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kPort);
    if (bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind port %d, errno %d", kPort, errno);
        close(fd_);
        fd_ = -1;
        return false;
    }

    running_ = true;
    xSemaphoreTake(stopped_, 0);
    if (xTaskCreate([](void* arg) {
            static_cast<DnsServer*>(arg)->Run();
            vTaskDelete(nullptr);
        }, "dns_server", 3072, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        running_ = false;
        close(fd_);
        fd_ = -1;
        return false;
    }
    ESP_LOGI(TAG, "Started");
    return true;
}

void DnsServer::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    xSemaphoreTake(stopped_, portMAX_DELAY);
    ESP_LOGI(TAG, "Stopped");
}

void DnsServer::Run() {
    uint8_t packet[512];
    while (running_) {
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        int length = recvfrom(fd_, packet, sizeof(packet), 0, (struct sockaddr*)&client, &client_length);
        if (length <= 0) {
            continue;  // Timeout
        }
        length = BuildResponse(packet, length, sizeof(packet), ip_);
        if (length > 0) {
            sendto(fd_, packet, length, 0, (struct sockaddr*)&client, client_length);
        }
    }
    close(fd_);
    fd_ = -1;
    xSemaphoreGive(stopped_);
}
