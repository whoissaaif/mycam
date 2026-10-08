#pragma once
// Wire protocol shared with the Android app (app/src/main/java/com/example/mycam/Protocol.kt).
#include <stdint.h>

namespace mycam::proto {

constexpr uint32_t kPacketMagic = 0x4D43414D; // 'MCAM'
constexpr size_t kHeaderSize = 20;            // u32 magic, u8 type, u8 flags, u16 reserved, i64 ptsUs, u32 length
constexpr uint32_t kMaxPayload = 8 * 1024 * 1024;

enum PacketType : uint8_t {
    kHello = 0,   // u16 protocol version
    kConfig = 1,  // u16 width, u16 height, u16 sensorOrientation, u8 facing, then SPS/PPS (Annex-B)
    kFrame = 2,   // H.264 Annex-B access unit
    kOrient = 3,  // u16 device rotation (degrees)
    kState = 4,   // u8 state, [u8 facing]
    kLog = 5,     // UTF-8 diagnostic text from the phone
};
constexpr uint8_t kMaxPacketType = kLog;

constexpr uint8_t kFlagKeyFrame = 0x01;

enum PhoneStreamState : uint8_t { kStateIdle = 0, kStateStreaming = 1, kStateError = 2 };
enum Facing : uint8_t { kFacingBack = 0, kFacingFront = 1 };

constexpr uint32_t kCommandMagic = 0x4D434D44; // 'MCMD'
constexpr size_t kCommandSize = 8;

enum Command : uint8_t {
    kCmdHello = 1,
    kCmdStart = 2,
    kCmdStop = 3,
    kCmdKeyFrame = 4,
    kCmdSetFacing = 5,
};

inline uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline int64_t ReadI64(const uint8_t* p) { return int64_t(uint64_t(ReadU32(p)) << 32 | ReadU32(p + 4)); }

inline void MakeCommand(uint8_t out[kCommandSize], Command cmd, uint8_t arg = 0) {
    out[0] = 0x4D; out[1] = 0x43; out[2] = 0x4D; out[3] = 0x44;
    out[4] = cmd; out[5] = arg; out[6] = 0; out[7] = 0;
}

// Accessory identification sent during the AOA handshake. Must match res/xml/accessory_filter.xml.
constexpr const char* kAccessoryManufacturer = "MyCam";
constexpr const char* kAccessoryModel = "MyCam Webcam";
constexpr const char* kAccessoryDescription = "Use this phone as a USB webcam";
constexpr const char* kAccessoryVersion = "1";
constexpr const char* kAccessoryUri = "https://github.com/";
constexpr const char* kAccessorySerial = "0001";

} // namespace mycam::proto
