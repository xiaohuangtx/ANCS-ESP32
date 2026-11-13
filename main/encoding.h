#ifndef ENCODING_H
#define ENCODING_H

#include <array>
#include <cstdint>
#include <string>

// Small encoders shared by the admin page (Basic auth) and the webhook
// formats (DingTalk / Feishu signatures). Self-contained, so they do not
// depend on the mbedTLS API, which differs between ESP-IDF versions.

std::string Base64Encode(const std::string& in);
// Percent-encodes everything except the RFC 3986 unreserved characters
std::string UrlEncode(const std::string& in);

std::array<uint8_t, 32> Sha256(const std::string& data);
std::array<uint8_t, 32> HmacSha256(const std::string& key, const std::string& message);

#endif // ENCODING_H
