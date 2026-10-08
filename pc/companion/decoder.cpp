#include "decoder.h"

#include <mferror.h>
#include <string.h>

using Microsoft::WRL::ComPtr;

namespace mycam {

H264Decoder::~H264Decoder() { Reset(); }

void H264Decoder::Reset() {
    if (transform_) {
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform_.Reset();
    }
    width_ = height_ = codedWidth_ = codedHeight_ = 0;
}

HRESULT H264Decoder::Init(uint32_t width, uint32_t height, FrameCallback onFrame) {
    Reset();
    onFrame_ = std::move(onFrame);

    MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO out = {MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &in, &out, &activates, &count);
    if (FAILED(hr)) return hr;
    if (count == 0) {
        CoTaskMemFree(activates);
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&transform_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(transform_->GetAttributes(&attrs))) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(&inType);
    if (FAILED(hr)) return hr;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 30, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = transform_->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) { transform_.Reset(); return hr; }

    hr = SetOutputType();
    if (FAILED(hr)) { transform_.Reset(); return hr; }

    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
}

HRESULT H264Decoder::SetOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = transform_->GetOutputAvailableType(0, i, &type);
        if (FAILED(hr)) return hr;
        GUID subtype;
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        hr = transform_->SetOutputType(0, type.Get(), 0);
        if (FAILED(hr)) return hr;

        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &codedWidth_, &codedHeight_);
        width_ = codedWidth_;
        height_ = codedHeight_;
        MFVideoArea area;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            width_ = area.Area.cx;
            height_ = area.Area.cy;
        }
        UINT32 stride = 0;
        stride_ = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? LONG(int32_t(stride)) : LONG(codedWidth_);
        if (stride_ <= 0) stride_ = LONG(codedWidth_);

        MFT_OUTPUT_STREAM_INFO info = {};
        transform_->GetOutputStreamInfo(0, &info);
        providesSamples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outputBufferSize_ = info.cbSize ? info.cbSize : DWORD(stride_) * codedHeight_ * 3 / 2;
        return S_OK;
    }
}

HRESULT H264Decoder::Decode(const uint8_t* data, size_t size, int64_t ptsUs) {
    if (!transform_) return E_UNEXPECTED;
    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(DWORD(size), &buffer);
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(size));
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(ptsUs * 10);

    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        DrainOutput();
        hr = transform_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    return DrainOutput();
}

HRESULT H264Decoder::DrainOutput() {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out = {};
        ComPtr<IMFSample> ownSample;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = MFCreateSample(&ownSample);
            if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(outputBufferSize_, &buffer);
            if (FAILED(hr)) return hr;
            ownSample->AddBuffer(buffer.Get());
            out.pSample = ownSample.Get();
        }
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            hr = SetOutputType();
            if (FAILED(hr)) return hr;
            continue;
        }
        if (FAILED(hr)) return hr;

        EmitFrame(out.pSample);
        if (providesSamples_ && out.pSample) out.pSample->Release();
    }
}

void H264Decoder::EmitFrame(IMFSample* sample) {
    if (!sample || !onFrame_ || width_ == 0 || height_ == 0) return;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;

    const uint32_t w = width_ & ~1u, h = height_ & ~1u;
    packed_.resize(size_t(w) * h * 3 / 2);

    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* base = nullptr;
    LONG pitch = 0;
    bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&base, &pitch));
    DWORD length = 0;
    if (!locked2d) {
        if (FAILED(buffer->Lock(&base, nullptr, &length))) return;
        pitch = stride_;
        if (length < DWORD(pitch) * codedHeight_ * 3 / 2) { buffer->Unlock(); return; }
    }

    // NV12: Y plane of codedHeight rows, then interleaved UV plane.
    const BYTE* srcY = base;
    const BYTE* srcUV = base + size_t(pitch) * codedHeight_;
    uint8_t* dstY = packed_.data();
    uint8_t* dstUV = dstY + size_t(w) * h;
    for (uint32_t y = 0; y < h; ++y) memcpy(dstY + size_t(y) * w, srcY + size_t(y) * pitch, w);
    for (uint32_t y = 0; y < h / 2; ++y) memcpy(dstUV + size_t(y) * w, srcUV + size_t(y) * pitch, w);

    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();

    onFrame_(packed_.data(), w, h);
}

} // namespace mycam
