#pragma once
#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mycam::usbmux {

constexpr uint32_t kVersionPlist = 1;
constexpr uint32_t kTypePlist = 8;

struct Header {
    uint32_t length = 0;
    uint32_t version = 0;
    uint32_t type = 0;
    uint32_t tag = 0;
};

struct Message {
    std::string messageType;
    uint32_t number = 0;
    uint32_t deviceId = 0;
    std::string serialNumber;
    std::string connectionType;
};

struct PlistValue {
    enum class Type { String, Integer, Data, Dict };
    Type type = Type::String;
    std::string text;
    uint64_t integer = 0;
    std::map<std::string, PlistValue> dict;
};

using Bytes = std::vector<uint8_t>;

void WriteLe32(Bytes& out, uint32_t v);
uint32_t ReadLe32(const uint8_t* p);
Bytes EncodePacket(uint32_t tag, const std::string& plist);
bool DecodeHeader(const uint8_t* data, size_t size, Header* out);

std::string BuildListenPlist();
std::string BuildConnectPlist(uint32_t deviceId, uint16_t port);
Bytes BuildListenPacket(uint32_t tag);
Bytes BuildConnectPacket(uint32_t tag, uint32_t deviceId, uint16_t port);

bool ParsePlist(const std::string& xml, PlistValue* out);
bool ParseMessage(const std::string& xml, Message* out);

class PacketReader {
public:
    using Callback = std::function<void(const Header&, const std::string&)>;

    void Feed(const uint8_t* data, size_t size, const Callback& onPacket);
    void Reset() { buffer_.clear(); }
    size_t Buffered() const { return buffer_.size(); }

private:
    Bytes buffer_;
};

} // namespace mycam::usbmux
