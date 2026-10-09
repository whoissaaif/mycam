#include "usbmux_proto.h"

#include <algorithm>
#include <ctype.h>
#include <string.h>
#include <sstream>

namespace mycam::usbmux {

namespace {

constexpr uint32_t kHeaderSize = 16;

std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string Unescape(std::string s) {
    struct Entity { const char* from; const char* to; };
    const Entity entities[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    for (const Entity& e : entities) {
        size_t pos = 0;
        while ((pos = s.find(e.from, pos)) != std::string::npos) {
            s.replace(pos, strlen(e.from), e.to);
            pos += strlen(e.to);
        }
    }
    return s;
}

void SkipWs(const std::string& s, size_t* pos) {
    while (*pos < s.size() && isspace(static_cast<unsigned char>(s[*pos]))) ++*pos;
}

bool Starts(const std::string& s, size_t pos, const char* token) {
    return s.compare(pos, strlen(token), token) == 0;
}

bool Consume(const std::string& s, size_t* pos, const char* token) {
    SkipWs(s, pos);
    if (!Starts(s, *pos, token)) return false;
    *pos += strlen(token);
    return true;
}

bool ReadUntil(const std::string& s, size_t* pos, const char* end, std::string* out) {
    size_t e = s.find(end, *pos);
    if (e == std::string::npos) return false;
    *out = s.substr(*pos, e - *pos);
    *pos = e + strlen(end);
    return true;
}

bool ParseValue(const std::string& s, size_t* pos, PlistValue* out);

bool ParseDict(const std::string& s, size_t* pos, PlistValue* out) {
    if (!Consume(s, pos, "<dict>")) return false;
    out->type = PlistValue::Type::Dict;
    out->dict.clear();
    while (true) {
        SkipWs(s, pos);
        if (Consume(s, pos, "</dict>")) return true;
        if (!Consume(s, pos, "<key>")) return false;
        std::string key;
        if (!ReadUntil(s, pos, "</key>", &key)) return false;
        PlistValue value;
        if (!ParseValue(s, pos, &value)) return false;
        out->dict[Unescape(key)] = value;
    }
}

bool ParseValue(const std::string& s, size_t* pos, PlistValue* out) {
    SkipWs(s, pos);
    if (Starts(s, *pos, "<dict>")) return ParseDict(s, pos, out);
    if (Consume(s, pos, "<string>")) {
        out->type = PlistValue::Type::String;
        return ReadUntil(s, pos, "</string>", &out->text) && (out->text = Unescape(out->text), true);
    }
    if (Consume(s, pos, "<integer>")) {
        std::string v;
        if (!ReadUntil(s, pos, "</integer>", &v)) return false;
        out->type = PlistValue::Type::Integer;
        out->integer = std::stoull(v);
        return true;
    }
    if (Consume(s, pos, "<data>")) {
        out->type = PlistValue::Type::Data;
        return ReadUntil(s, pos, "</data>", &out->text);
    }
    return false;
}

const PlistValue* Get(const PlistValue& dict, const char* key) {
    if (dict.type != PlistValue::Type::Dict) return nullptr;
    auto it = dict.dict.find(key);
    return it == dict.dict.end() ? nullptr : &it->second;
}

std::string HeaderXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n";
}

std::string FooterXml() {
    return "</plist>\n";
}

void AddString(std::ostringstream& out, const char* key, const std::string& value) {
    out << "<key>" << key << "</key><string>" << Escape(value) << "</string>\n";
}

void AddInteger(std::ostringstream& out, const char* key, uint64_t value) {
    out << "<key>" << key << "</key><integer>" << value << "</integer>\n";
}

} // namespace

void WriteLe32(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 24));
}

uint32_t ReadLe32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

Bytes EncodePacket(uint32_t tag, const std::string& plist) {
    Bytes out;
    WriteLe32(out, uint32_t(kHeaderSize + plist.size()));
    WriteLe32(out, kVersionPlist);
    WriteLe32(out, kTypePlist);
    WriteLe32(out, tag);
    out.insert(out.end(), plist.begin(), plist.end());
    return out;
}

bool DecodeHeader(const uint8_t* data, size_t size, Header* out) {
    if (size < kHeaderSize) return false;
    out->length = ReadLe32(data);
    out->version = ReadLe32(data + 4);
    out->type = ReadLe32(data + 8);
    out->tag = ReadLe32(data + 12);
    return out->length >= kHeaderSize;
}

std::string BuildListenPlist() {
    std::ostringstream out;
    out << HeaderXml() << "<dict>\n";
    AddString(out, "ClientVersionString", "MyCam");
    AddString(out, "MessageType", "Listen");
    AddString(out, "ProgName", "MyCam");
    out << "</dict>\n" << FooterXml();
    return out.str();
}

std::string BuildConnectPlist(uint32_t deviceId, uint16_t port) {
    uint16_t networkPort = uint16_t((port << 8) | (port >> 8));
    std::ostringstream out;
    out << HeaderXml() << "<dict>\n";
    AddInteger(out, "DeviceID", deviceId);
    AddString(out, "MessageType", "Connect");
    AddInteger(out, "PortNumber", networkPort);
    out << "</dict>\n" << FooterXml();
    return out.str();
}

Bytes BuildListenPacket(uint32_t tag) {
    return EncodePacket(tag, BuildListenPlist());
}

Bytes BuildConnectPacket(uint32_t tag, uint32_t deviceId, uint16_t port) {
    return EncodePacket(tag, BuildConnectPlist(deviceId, port));
}

bool ParsePlist(const std::string& xml, PlistValue* out) {
    size_t pos = xml.find("<plist");
    if (pos == std::string::npos) return false;
    pos = xml.find('>', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    if (!ParseValue(xml, &pos, out)) return false;
    return true;
}

bool ParseMessage(const std::string& xml, Message* out) {
    PlistValue root;
    if (!ParsePlist(xml, &root)) return false;
    if (const PlistValue* v = Get(root, "MessageType"); v && v->type == PlistValue::Type::String) out->messageType = v->text;
    if (const PlistValue* v = Get(root, "Number"); v && v->type == PlistValue::Type::Integer) out->number = uint32_t(v->integer);
    if (const PlistValue* v = Get(root, "DeviceID"); v && v->type == PlistValue::Type::Integer) out->deviceId = uint32_t(v->integer);
    if (const PlistValue* props = Get(root, "Properties")) {
        if (const PlistValue* v = Get(*props, "SerialNumber"); v && v->type == PlistValue::Type::String) out->serialNumber = v->text;
        if (const PlistValue* v = Get(*props, "ConnectionType"); v && v->type == PlistValue::Type::String) out->connectionType = v->text;
    }
    return !out->messageType.empty();
}

void PacketReader::Feed(const uint8_t* data, size_t size, const Callback& onPacket) {
    buffer_.insert(buffer_.end(), data, data + size);
    while (buffer_.size() >= kHeaderSize) {
        Header h;
        if (!DecodeHeader(buffer_.data(), buffer_.size(), &h)) return;
        if (h.length < kHeaderSize || h.length > 1024 * 1024) {
            buffer_.erase(buffer_.begin());
            continue;
        }
        if (buffer_.size() < h.length) return;
        std::string payload(reinterpret_cast<const char*>(buffer_.data() + kHeaderSize), h.length - kHeaderSize);
        onPacket(h, payload);
        buffer_.erase(buffer_.begin(), buffer_.begin() + h.length);
    }
}

} // namespace mycam::usbmux
