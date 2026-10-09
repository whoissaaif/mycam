#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <functional>
#include <stdint.h>
#include <vector>

namespace mycam {

// Decodes H.264 Annex-B access units into tightly packed NV12 frames using the Windows
// Media Foundation decoder.
class H264Decoder {
public:
    // A decoded NV12 frame, valid only during the callback: Y and UV planes with a shared row pitch.
    struct Nv12View {
        const uint8_t* y;
        const uint8_t* uv;
        ptrdiff_t pitch;
        uint32_t width, height;
    };
    using FrameCallback = std::function<void(const Nv12View&)>;

    ~H264Decoder();

    HRESULT Init(uint32_t width, uint32_t height, FrameCallback onFrame);
    void Reset();
    bool Ready() const { return transform_ != nullptr; }

    HRESULT Decode(const uint8_t* data, size_t size, int64_t ptsUs);

private:
    HRESULT SetOutputType();
    HRESULT DrainOutput();
    void EmitFrame(IMFSample* sample);

    Microsoft::WRL::ComPtr<IMFTransform> transform_;
    FrameCallback onFrame_;
    bool providesSamples_ = false;
    DWORD outputBufferSize_ = 0;
    uint32_t codedWidth_ = 0, codedHeight_ = 0; // Buffer dimensions (may include padding rows).
    uint32_t width_ = 0, height_ = 0;           // Visible area.
    LONG stride_ = 0;
};

} // namespace mycam
