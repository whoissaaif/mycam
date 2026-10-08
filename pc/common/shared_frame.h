#pragma once
// Frame hand-off between MyCamCompanion.exe (user session) and MyCamVCam.dll, which Windows loads
// into the Camera Frame Server service. The section lives in the Global namespace so both sides can
// see it; the DLL creates it (services hold SeCreateGlobalPrivilege) and the companion opens it.

#include <windows.h>
#include <stdint.h>

namespace mycam {

constexpr wchar_t kSharedFrameName[] = L"Global\\MyCamFrame";
constexpr uint32_t kSharedMagic = 0x4D434652; // 'MCFR'
constexpr uint32_t kSharedVersion = 1;
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 3 / 2;
constexpr uint32_t kSharedBytes = kHeaderBytes + kMaxFrameBytes;

// Allow SYSTEM, LOCAL SERVICE, administrators and interactive users to read/write.
constexpr wchar_t kSharedSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)";

enum PhoneState : uint32_t {
    kPhoneNone = 0,       // No phone connected.
    kPhoneWaiting = 1,    // Phone in accessory mode, waiting for the MyCam app to respond.
    kPhoneIdle = 2,       // MyCam app connected, camera off.
    kPhoneStreaming = 3,  // Frames are flowing.
};

struct SharedHeader {
    uint32_t magic;
    uint32_t version;
    // Seqlock: odd while the companion is writing a frame, even when the frame is stable.
    volatile LONG seq;
    uint32_t width;     // NV12 frame, tightly packed (stride == width).
    uint32_t height;
    uint32_t rotation;  // Clockwise degrees (0/90/180/270) the consumer must rotate the frame by.
    uint32_t mirror;    // Non-zero: flip horizontally after rotating.
    uint32_t phoneState;
    volatile uint64_t frameTick;     // GetTickCount64() when the frame was written (companion).
    volatile uint64_t consumerTick;  // GetTickCount64() of the last frame request (virtual camera).
};
static_assert(sizeof(SharedHeader) <= kHeaderBytes, "header too large");

inline uint8_t* FrameData(void* base) { return static_cast<uint8_t*>(base) + kHeaderBytes; }

} // namespace mycam
