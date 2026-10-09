#include "settings_window.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

#include "protocol.h"
#include "status_text.h"

using Microsoft::WRL::ComPtr;

namespace mycam {

namespace {

// --- Aero palette (IMPROVEMENTS.md 7.2) ----------------------------------------------------------
constexpr UINT32 kBody = 0xFFFFFF;          // Content area
constexpr UINT32 kCommandArea = 0xF0F0F0;   // Bottom button strip
constexpr UINT32 kCommandLine = 0xDFDFDF;
constexpr UINT32 kMainInstruction = 0x003399;
constexpr UINT32 kText = 0x000000;
constexpr UINT32 kSubtle = 0x5A5A5A;
constexpr UINT32 kLink = 0x0066CC;
constexpr UINT32 kLinkHot = 0x3399FF;
constexpr UINT32 kRule = 0xE2E2E2;

// Layout in DIPs.
constexpr float kWidth = 420, kHeight = 756;
constexpr float kTitleH = 30, kFrame = 7, kCommandH = 46;

enum Id {
    kNone = 0, kPause, kBack, kFront,
    kQ720, kQ1080, kQ4K, kFps30, kFps60, kFps120, kZoomOut, kZoomIn, kZoomReset, kEvDown, kEvUp, kFocusAuto, kFocusLock,
    kTorch, kAuto,
    kMirror, kFill, kAutostart, kReconnect, kOpenLog, kClose, kTitleClose, kTitleMin };
enum class Kind { Segment, Checkbox, Link, Button, TitleClose, TitleMin };

struct Element {
    Id id;
    Kind kind;
    D2D1_RECT_F rect;
    std::wstring text;
};

D2D1_COLOR_F Rgb(UINT32 rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }
D2D1_RECT_F Rect(float l, float t, float w, float h) { return D2D1::RectF(l, t, l + w, t + h); }
bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

} // namespace

struct SettingsWindow::Impl {
    SettingsModel* model;
    HWND hwnd = nullptr;
    float scale = 1.f;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<IDWriteTextFormat> fontBody, fontTitle, fontHeading, fontInstruction;
    ComPtr<ID2D1Bitmap> appIcon16, statusIcon;
    int statusIconId = 0;

    std::vector<Element> elements;
    Id hot = kNone, pressed = kNone, focus = kBack;
    bool keyboardCues = false;
    bool tracking = false;

    // --- Setup ---------------------------------------------------------------------------------------

    void CreateFactories() {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        auto font = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"", &out);
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            out->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        };
        font(12.f, DWRITE_FONT_WEIGHT_NORMAL, fontBody);        // Segoe UI 9pt, the Win7 UI font
        font(12.f, DWRITE_FONT_WEIGHT_NORMAL, fontTitle);
        font(12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fontHeading);
        font(16.f, DWRITE_FONT_WEIGHT_NORMAL, fontInstruction); // Segoe UI 12pt "main instruction"
    }

    void Layout() {
        elements.clear();
        const float x = kFrame + 20;
        elements.push_back({kTitleClose, Kind::TitleClose, Rect(kWidth - kFrame - 45, 1, 43, 19), L""});
        elements.push_back({kTitleMin, Kind::TitleMin, Rect(kWidth - kFrame - 45 - 27, 1, 28, 19), L""});
        elements.push_back({kPause, Kind::Button, Rect(kWidth - kFrame - 20 - 92, kTitleH + 22, 92, 28), L"Pause"});
        elements.push_back({kBack, Kind::Segment, Rect(x, 164, 150, 28), L"Back camera"});
        elements.push_back({kFront, Kind::Segment, Rect(x + 149, 164, 150, 28), L"Front camera"});
        // Video (y 222).
        elements.push_back({kQ720, Kind::Segment, Rect(x, 246, 100, 28), L"720p"});
        elements.push_back({kQ1080, Kind::Segment, Rect(x + 99, 246, 100, 28), L"1080p"});
        elements.push_back({kQ4K, Kind::Segment, Rect(x + 198, 246, 100, 28), L"4K"});
        elements.push_back({kFps30, Kind::Segment, Rect(x, 284, 100, 26), L"30 fps"});
        elements.push_back({kFps60, Kind::Segment, Rect(x + 99, 284, 100, 26), L"60 fps"});
        elements.push_back({kFps120, Kind::Segment, Rect(x + 198, 284, 100, 26), L"120 fps"});
        // Camera controls (y 350). Values are drawn between the buttons in Paint().
        const float cx = x + 120;
        elements.push_back({kZoomOut, Kind::Button, Rect(cx, 374, 32, 26), L"−"});
        elements.push_back({kZoomIn, Kind::Button, Rect(cx + 102, 374, 32, 26), L"+"});
        elements.push_back({kZoomReset, Kind::Button, Rect(cx + 144, 374, 44, 26), L"1×"});
        elements.push_back({kEvDown, Kind::Button, Rect(cx, 410, 32, 26), L"−"});
        elements.push_back({kEvUp, Kind::Button, Rect(cx + 102, 410, 32, 26), L"+"});
        elements.push_back({kFocusAuto, Kind::Segment, Rect(cx, 446, 70, 26), L"Auto"});
        elements.push_back({kFocusLock, Kind::Segment, Rect(cx + 69, 446, 70, 26), L"Lock"});
        elements.push_back({kTorch, Kind::Checkbox, Rect(x, 482, 300, 20), L"Torch (phone light)"});
        elements.push_back({kAuto, Kind::Button, Rect(kWidth - kFrame - 20 - 56, 345, 56, 23), L"Auto"});
        // Picture (y 530), General (y 614).
        elements.push_back({kMirror, Kind::Checkbox, Rect(x, 554, 300, 20), L"Mirror the image"});
        elements.push_back({kFill, Kind::Checkbox, Rect(x, 578, 330, 20), L"Fill the frame (crop instead of black bars)"});
        elements.push_back({kAutostart, Kind::Checkbox, Rect(x, 638, 300, 20), L"Start MyCam with Windows"});
        elements.push_back({kReconnect, Kind::Link, Rect(x, 672, 110, 20), L"Reconnect phone"});
        elements.push_back({kOpenLog, Kind::Link, Rect(x + 130, 672, 110, 20), L"Open log folder"});
        const float bottom = kHeight - kFrame;
        elements.push_back({kClose, Kind::Button, Rect(kWidth - kFrame - 12 - 86, bottom - kCommandH + 11, 86, 24), L"Close"});
    }

    const Element* Find(Id id) const {
        for (const auto& e : elements) if (e.id == id) return &e;
        return nullptr;
    }

    // Pause needs a connected phone, and is moot while Windows is locked (already paused).
    bool PauseEnabled() const {
        const LinkStatus s = model->status();
        return s.state >= LinkState::Idle && !s.lockPaused;
    }

    static int EffectiveFps(const proto::CameraInfo& c) {
        const uint8_t mask = c.width > 0 ? proto::FpsMask(c, c.quality) : proto::kFps30Bit;
        if (c.fps >= 120 && (mask & proto::kFps120Bit)) return 120;
        if (c.fps >= 60 && (mask & proto::kFps60Bit)) return 60;
        return 30;
    }

    // Whether a control can be used right now (connected phone, and the phone's camera supports it).
    bool Enabled(Id id) const {
        const LinkStatus s = model->status();
        const proto::CameraInfo& c = s.camera;
        const bool connected = s.state >= LinkState::Idle && c.valid;
        const bool known = connected && c.width > 0; // Capabilities arrive once the camera has started.
        switch (id) {
        case kPause: return PauseEnabled();
        case kQ720: case kQ1080: return connected;
        case kQ4K: return connected && (!known || (c.flags & proto::kCamHas4K));
        // Frame rates: only what the phone reports working at the current quality (30 until it has reported).
        case kFps30: return connected;
        case kFps60: return known && (proto::FpsMask(c, c.quality) & proto::kFps60Bit);
        case kFps120: return known && (proto::FpsMask(c, c.quality) & proto::kFps120Bit);
        case kZoomOut: return known && c.zoomX100 > c.zoomMinX100;
        case kZoomIn: return known && c.zoomX100 < c.zoomMaxX100;
        case kZoomReset: return known && c.zoomX100 != 100 && c.zoomMinX100 <= 100 && c.zoomMaxX100 >= 100;
        case kEvDown: return known && c.evMax > c.evMin && c.ev > c.evMin;
        case kEvUp: return known && c.evMax > c.evMin && c.ev < c.evMax;
        case kFocusAuto: case kFocusLock: return known && (c.flags & proto::kCamHasAutofocus);
        case kTorch: return known && (c.flags & proto::kCamTorchAvailable);
        case kAuto: // Something differs from the defaults (1x, 0 EV, auto focus, torch off).
            return known && (c.zoomX100 != std::clamp<uint16_t>(100, c.zoomMinX100, c.zoomMaxX100) || c.ev != 0 ||
                             (c.flags & (proto::kCamTorchOn | proto::kCamFocusLocked)));
        default: return true;
        }
    }

    Id HitTest(float x, float y) const {
        for (const auto& e : elements) {
            if (!Enabled(e.id)) continue;
            if (Contains(e.rect, x, y)) return e.id;
        }
        return kNone;
    }

    // Loads an icon resource at the exact pixel size and converts it to a D2D bitmap.
    ComPtr<ID2D1Bitmap> LoadIconBitmap(int resourceId, int px) {
        ComPtr<ID2D1Bitmap> out;
        HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), IMAGE_ICON,
                                                   px, px, LR_DEFAULTCOLOR));
        if (!icon || !wic || !rt) return out;
        ComPtr<IWICBitmap> wicBitmap;
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &wicBitmap)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0, WICBitmapPaletteTypeCustom))) {
            rt->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &out);
        }
        DestroyIcon(icon);
        return out;
    }

    void EnsureTarget() {
        if (rt) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = 96.f * scale;
        d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &rt);
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        appIcon16 = LoadIconBitmap(kIconApp, int(16 * scale + 0.5f));
        statusIcon.Reset();
        statusIconId = 0;
    }

    // --- Drawing primitives ----------------------------------------------------------------------

    ComPtr<ID2D1SolidColorBrush> Solid(D2D1_COLOR_F c) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(c, &b);
        return b;
    }

    ComPtr<ID2D1LinearGradientBrush> Vertical(float top, float bottom, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ComPtr<ID2D1GradientStopCollection> collection;
        rt->CreateGradientStopCollection(stops.begin(), UINT32(stops.size()), &collection);
        ComPtr<ID2D1LinearGradientBrush> b;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, top), D2D1::Point2F(0, bottom)),
                                      collection.Get(), &b);
        return b;
    }

    void Text(const std::wstring& s, IDWriteTextFormat* format, D2D1_RECT_F r, D2D1_COLOR_F color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        format->SetTextAlignment(align);
        rt->DrawText(s.c_str(), UINT32(s.size()), format, r, Solid(color).Get());
    }

    // Window frame: Aero "glass" (opaque approximation): sky-blue gradient with soft aurora sheens.
    void DrawFrame() {
        const D2D1_RECT_F all = Rect(0, 0, kWidth, kHeight);
        rt->FillRectangle(all, Vertical(0, kHeight, {{0.f, Rgb(0xC9DDF3)}, {0.08f, Rgb(0xA9C6EA)}, {1.f, Rgb(0x8FB2DD)}}).Get());

        // Aurora: two soft diagonal light streaks.
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP s[] = {{0.f, D2D1::ColorF(1, 1, 1, 0.55f)}, {1.f, D2D1::ColorF(1, 1, 1, 0.f)}};
        rt->CreateGradientStopCollection(s, 2, &stops);
        ComPtr<ID2D1RadialGradientBrush> glow;
        rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(110, 6), {}, 150, 26), stops.Get(), &glow);
        rt->FillRectangle(Rect(0, 0, kWidth, kTitleH), glow.Get());
        rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(330, 20), {}, 120, 18), stops.Get(), &glow);
        rt->FillRectangle(Rect(0, 0, kWidth, kTitleH), glow.Get());

        // Highlight along the top edge, dark outer edge.
        rt->DrawLine(D2D1::Point2F(1, 1.5f), D2D1::Point2F(kWidth - 1, 1.5f), Solid(D2D1::ColorF(1, 1, 1, 0.8f)).Get());
        rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, kWidth - 0.5f, kHeight - 0.5f), Solid(Rgb(0x3E5F8A, 0.9f)).Get());

        // Title: app icon and text with the Win7 white glow behind it.
        if (appIcon16) rt->DrawBitmap(appIcon16.Get(), Rect(kFrame + 2, 7, 16, 16));
        ComPtr<ID2D1RadialGradientBrush> textGlow;
        D2D1_GRADIENT_STOP gs[] = {{0.f, D2D1::ColorF(1, 1, 1, 0.75f)}, {1.f, D2D1::ColorF(1, 1, 1, 0.f)}};
        ComPtr<ID2D1GradientStopCollection> glowStops;
        rt->CreateGradientStopCollection(gs, 2, &glowStops);
        rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(80, 15), {}, 62, 13), glowStops.Get(), &textGlow);
        rt->FillRectangle(Rect(14, 0, 140, kTitleH), textGlow.Get());
        Text(L"MyCam Settings", fontTitle.Get(), Rect(kFrame + 24, 0, 200, kTitleH), Rgb(kText));

        // Content area with a thin dark edge where it meets the glass.
        const D2D1_RECT_F content = D2D1::RectF(kFrame, kTitleH, kWidth - kFrame, kHeight - kFrame);
        rt->FillRectangle(content, Solid(Rgb(kBody)).Get());
        const D2D1_RECT_F command = D2D1::RectF(kFrame, kHeight - kFrame - kCommandH, kWidth - kFrame, kHeight - kFrame);
        rt->FillRectangle(command, Solid(Rgb(kCommandArea)).Get());
        rt->DrawLine(D2D1::Point2F(command.left, command.top + 0.5f), D2D1::Point2F(command.right, command.top + 0.5f),
                     Solid(Rgb(kCommandLine)).Get());
        rt->DrawRectangle(D2D1::RectF(content.left - 0.5f, content.top - 0.5f, content.right + 0.5f, content.bottom + 0.5f),
                          Solid(Rgb(0x6F8DB5, 0.85f)).Get());
    }

    // Win7 close caption button: glossy red, brighter when hot.
    void DrawTitleClose(const Element& e) {
        const D2D1_RECT_F r = e.rect;
        const bool isHot = hot == e.id, isDown = pressed == e.id && isHot;
        D2D1_ROUNDED_RECT rr = {r, 3.5f, 3.5f};
        auto fill = isDown ? Vertical(r.top, r.bottom, {{0.f, Rgb(0xD1987F)}, {0.5f, Rgb(0xB0402A)}, {0.51f, Rgb(0x8E1A08)}, {1.f, Rgb(0xC2481E)}})
                  : isHot  ? Vertical(r.top, r.bottom, {{0.f, Rgb(0xF4B9A6)}, {0.5f, Rgb(0xE7614A)}, {0.51f, Rgb(0xD2280E)}, {1.f, Rgb(0xEF8B5C)}})
                           : Vertical(r.top, r.bottom, {{0.f, Rgb(0xE6AE9E)}, {0.5f, Rgb(0xD07560)}, {0.51f, Rgb(0xB8391F)}, {1.f, Rgb(0xD2805D)}});
        rt->FillRoundedRectangle(rr, fill.Get());
        rt->DrawRoundedRectangle(rr, Solid(Rgb(0x5A1A10, 0.8f)).Get());
        D2D1_ROUNDED_RECT inner = {D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 2.5f, 2.5f};
        rt->DrawRoundedRectangle(inner, Solid(D2D1::ColorF(1, 1, 1, 0.35f)).Get());

        // White "x" with a dark outline.
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, d = 4.f;
        auto cross = [&](ID2D1Brush* b, float width) {
            rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), b, width);
            rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), b, width);
        };
        cross(Solid(Rgb(0x4A1208, 0.8f)).Get(), 3.6f);
        cross(Solid(Rgb(0xFFFFFF)).Get(), 2.0f);
    }

    // Win7 caption "minimize": clear glass that turns light blue on hover, with a white dash near the bottom.
    // It sits flush against the red close button, so only its left corners are rounded.
    void DrawTitleMin(const Element& e) {
        const D2D1_RECT_F r = e.rect;
        const bool isHot = hot == e.id, isDown = pressed == e.id && isHot;
        auto fill = isDown ? Vertical(r.top, r.bottom, {{0.f, Rgb(0x9DB9D9)}, {0.5f, Rgb(0x5E89BC)}, {0.51f, Rgb(0x2E5E98)}, {1.f, Rgb(0x5FA0D6)}})
                  : isHot  ? Vertical(r.top, r.bottom, {{0.f, Rgb(0xD6E7F8)}, {0.5f, Rgb(0xA6C7EA)}, {0.51f, Rgb(0x6E9FD6)}, {1.f, Rgb(0xA9DDF8)}})
                           : Vertical(r.top, r.bottom, {{0.f, Rgb(0xE3ECF6, 0.9f)}, {0.5f, Rgb(0xC2D5EC, 0.85f)}, {0.51f, Rgb(0xA4BEDF, 0.85f)}, {1.f, Rgb(0xC5DAF0, 0.9f)}});
        // Rounded on the left only: draw a rounded rect wider than the button, clipped to the button.
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        D2D1_ROUNDED_RECT rr = {D2D1::RectF(r.left, r.top, r.right + 6, r.bottom), 3.5f, 3.5f};
        rt->FillRoundedRectangle(rr, fill.Get());
        rt->DrawRoundedRectangle(rr, Solid(Rgb(0x2A4A70, 0.85f)).Get());
        D2D1_ROUNDED_RECT inner = {D2D1::RectF(r.left + 1, r.top + 1, r.right + 6, r.bottom - 1), 2.5f, 2.5f};
        rt->DrawRoundedRectangle(inner, Solid(D2D1::ColorF(1, 1, 1, 0.5f)).Get());
        rt->PopAxisAlignedClip();

        // White dash with a dark outline, low in the button like Windows 7's.
        const float cx = (r.left + r.right) / 2, y = r.bottom - 6.5f;
        rt->FillRectangle(D2D1::RectF(cx - 5.5f, y - 2.5f, cx + 5.5f, y + 2.f), Solid(Rgb(0x1E3550, 0.85f)).Get());
        rt->FillRectangle(D2D1::RectF(cx - 4.5f, y - 1.5f, cx + 4.5f, y + 1.f), Solid(Rgb(0xFFFFFF)).Get());
    }

    // Aero push button: two-tone gloss split at the middle (IMPROVEMENTS.md 7.2 button tokens).
    void DrawAeroButton(D2D1_RECT_F r, const std::wstring& label, bool isHot, bool isDown, bool selected,
                        bool enabled = true) {
        if (!enabled) {
            D2D1_ROUNDED_RECT rr = {D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3, 3};
            rt->FillRoundedRectangle(rr, Solid(Rgb(0xF4F4F4)).Get());
            rt->DrawRoundedRectangle(rr, Solid(Rgb(0xADB2B5)).Get());
            Text(label, fontBody.Get(), r, Rgb(0x838383), DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        const bool lit = isDown || selected;
        auto fill = lit   ? Vertical(r.top, r.bottom, {{0.f, Rgb(0xE5F4FC)}, {0.5f, Rgb(0xC4E5F6)}, {0.51f, Rgb(0x98D1EF)}, {1.f, Rgb(0x68B3DB)}})
                  : isHot ? Vertical(r.top, r.bottom, {{0.f, Rgb(0xEAF6FD)}, {0.5f, Rgb(0xD9F0FC)}, {0.51f, Rgb(0xBEE6FD)}, {1.f, Rgb(0xA7D9F5)}})
                          : Vertical(r.top, r.bottom, {{0.f, Rgb(0xF2F2F2)}, {0.5f, Rgb(0xEBEBEB)}, {0.51f, Rgb(0xDDDDDD)}, {1.f, Rgb(0xCFCFCF)}});
        const UINT32 border = lit ? 0x2C628B : isHot ? 0x3C7FB1 : 0x707070;
        const float radius = 3;
        D2D1_ROUNDED_RECT rr = {D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), radius, radius};
        rt->FillRoundedRectangle(rr, fill.Get());
        D2D1_ROUNDED_RECT inner = {D2D1::RectF(r.left + 1.5f, r.top + 1.5f, r.right - 1.5f, r.bottom - 1.5f), radius - 1, radius - 1};
        rt->DrawRoundedRectangle(inner, Solid(D2D1::ColorF(1, 1, 1, lit ? 0.35f : 0.8f)).Get());
        rt->DrawRoundedRectangle(rr, Solid(Rgb(border)).Get());
        Text(label, fontBody.Get(), r, Rgb(kText), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Win7 checkbox: 13px box with a soft inner gradient and a dark blue check.
    void DrawCheckbox(const Element& e, bool checked, bool enabled = true) {
        const bool isHot = enabled && hot == e.id;
        const float y = (e.rect.top + e.rect.bottom) / 2 - 6.5f;
        const D2D1_RECT_F box = Rect(e.rect.left, y, 13, 13);
        rt->FillRectangle(box, Solid(Rgb(0xFFFFFF)).Get());
        rt->DrawRectangle(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f),
                          Solid(Rgb(isHot ? 0x3C7FB1 : 0x8E8F8F)).Get());
        const D2D1_RECT_F well = D2D1::RectF(box.left + 2, box.top + 2, box.right - 2, box.bottom - 2);
        rt->FillRectangle(well, (isHot ? Vertical(well.top, well.bottom, {{0.f, Rgb(0xB9DEF5)}, {1.f, Rgb(0xE9F6FD)}})
                                       : Vertical(well.top, well.bottom, {{0.f, Rgb(0xCBCFD5)}, {1.f, Rgb(0xF6F6F6)}})).Get());
        if (checked) {
            ComPtr<ID2D1PathGeometry> path;
            d2d->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            sink->BeginFigure(D2D1::Point2F(box.left + 3, box.top + 6.5f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(box.left + 5.5f, box.top + 9.5f));
            sink->AddLine(D2D1::Point2F(box.left + 10.5f, box.top + 3));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt->DrawGeometry(path.Get(), Solid(Rgb(0x1B3D82)).Get(), 2.f);
        }
        Text(e.text, fontBody.Get(), D2D1::RectF(e.rect.left + 19, e.rect.top, e.rect.right, e.rect.bottom), Rgb(enabled ? kText : 0x838383));
    }

    void DrawLink(const Element& e) {
        const bool isHot = hot == e.id;
        Text(e.text, fontBody.Get(), e.rect, Rgb(isHot ? kLinkHot : kLink));
        if (isHot) {
            ComPtr<IDWriteTextLayout> layout;
            dwrite->CreateTextLayout(e.text.c_str(), UINT32(e.text.size()), fontBody.Get(), 400, 20, &layout);
            DWRITE_TEXT_METRICS m;
            layout->GetMetrics(&m);
            const float y = (e.rect.top + e.rect.bottom) / 2 + 7.5f;
            rt->DrawLine(D2D1::Point2F(e.rect.left, y), D2D1::Point2F(e.rect.left + m.width, y), Solid(Rgb(kLinkHot)).Get());
        }
    }

    void DrawFocus(const Element& e) {
        D2D1_RECT_F r = e.rect;
        if (e.kind == Kind::Checkbox || e.kind == Kind::Link) {
            ComPtr<IDWriteTextLayout> layout;
            dwrite->CreateTextLayout(e.text.c_str(), UINT32(e.text.size()), fontBody.Get(), 400, 20, &layout);
            DWRITE_TEXT_METRICS m;
            layout->GetMetrics(&m);
            const float left = e.kind == Kind::Checkbox ? r.left + 17 : r.left - 2;
            r = D2D1::RectF(left, r.top, left + m.width + 4, r.bottom);
        } else {
            r = D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3);
        }
        ComPtr<ID2D1StrokeStyle> dots;
        d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                           D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DOT),
                               nullptr, 0, &dots);
        rt->DrawRectangle(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f),
                          Solid(Rgb(0x000000, 0.7f)).Get(), 1.f, dots.Get());
    }

    // Row labels and current values for the Video / Camera controls sections.
    void DrawControlValues(const LinkStatus& status) {
        const proto::CameraInfo& c = status.camera;
        const bool known = status.state >= LinkState::Idle && c.valid && c.width > 0;
        const float x = kFrame + 20, cx = x + 120;
        const auto ink = Rgb(known ? kText : 0x838383);
        Text(L"Zoom", fontBody.Get(), Rect(x, 374, 110, 26), ink);
        Text(L"Brightness", fontBody.Get(), Rect(x, 410, 110, 26), ink);
        Text(L"Focus", fontBody.Get(), Rect(x, 446, 110, 26), ink);
        wchar_t buf[32];
        if (known) {
            swprintf_s(buf, L"%.1f×", c.zoomX100 / 100.0);
            Text(buf, fontBody.Get(), Rect(cx + 32, 374, 70, 26), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            const double ev = c.ev * c.evStepX100 / 100.0;
            swprintf_s(buf, L"%s%.1f EV", ev > 0 ? L"+" : L"", ev);
            Text(c.evMax > c.evMin ? buf : L"—", fontBody.Get(), Rect(cx + 32, 410, 70, 26), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            swprintf_s(buf, L"Now: %u×%u at %u fps", c.width, c.height, c.actualFps);
            Text(buf, fontBody.Get(), Rect(x, 316, 300, 20), Rgb(kSubtle)); // Own row under the frame rates.
        } else {
            Text(L"—", fontBody.Get(), Rect(cx + 32, 374, 70, 26), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(L"—", fontBody.Get(), Rect(cx + 32, 410, 70, 26), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(L"Controls appear once the phone camera has started.", fontBody.Get(), Rect(x, 466, 330, 20), Rgb(kSubtle));
        }
    }

    void SectionHeading(const wchar_t* text, float y) {
        const float x = kFrame + 20;
        Text(text, fontHeading.Get(), Rect(x, y, 200, 20), Rgb(0x1E3287));
        ComPtr<IDWriteTextLayout> layout;
        dwrite->CreateTextLayout(text, UINT32(wcslen(text)), fontHeading.Get(), 300, 20, &layout);
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        rt->DrawLine(D2D1::Point2F(x + m.width + 8, y + 10.5f), D2D1::Point2F(kWidth - kFrame - 20, y + 10.5f),
                     Solid(Rgb(kRule)).Get());
    }

    // "LIVE" pill: the Win7 progress-bar green with gloss.
    void DrawLivePill(float x, float y) {
        const D2D1_RECT_F r = Rect(x, y, 40, 17);
        D2D1_ROUNDED_RECT rr = {r, 8.5f, 8.5f};
        rt->FillRoundedRectangle(rr, Vertical(r.top, r.bottom, {{0.f, Rgb(0x8BE07A)}, {0.5f, Rgb(0x37C12B)}, {0.51f, Rgb(0x06B025)}, {1.f, Rgb(0x3CCB47)}}).Get());
        rt->DrawRoundedRectangle(rr, Solid(Rgb(0x0A7A1A)).Get());
        Text(L"LIVE", fontHeading.Get(), r, Rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void Paint() {
        EnsureTarget();
        const LinkStatus status = model->status();
        const StatusView view = DescribeStatus(status, model->cameraRegistered());

        rt->BeginDraw();
        rt->Clear(Rgb(kBody));
        DrawFrame();

        // Status: big state icon + main instruction + detail.
        if (statusIconId != view.icon) {
            statusIcon = LoadIconBitmap(view.icon, int(48 * scale + 0.5f));
            statusIconId = view.icon;
        }
        const float x = kFrame + 20;
        if (statusIcon) rt->DrawBitmap(statusIcon.Get(), Rect(x, kTitleH + 20, 48, 48));
        const float textW = kWidth - kFrame - 20 - 92 - 10 - (x + 62); // Up to the Pause button.
        Text(view.headline, fontInstruction.Get(), Rect(x + 62, kTitleH + 18, textW, 26), Rgb(kMainInstruction));
        if (status.state == LinkState::Streaming) {
            ComPtr<IDWriteTextLayout> layout;
            dwrite->CreateTextLayout(view.headline.c_str(), UINT32(view.headline.size()), fontInstruction.Get(), 300, 26, &layout);
            DWRITE_TEXT_METRICS m;
            layout->GetMetrics(&m);
            DrawLivePill(x + 62 + m.width + 10, kTitleH + 23);
        }
        Text(view.detail, fontBody.Get(), Rect(x + 62, kTitleH + 44, textW, 50), Rgb(kSubtle));

        SectionHeading(L"Camera", 140);
        SectionHeading(L"Video", 222);
        SectionHeading(L"Camera controls", 350);
        SectionHeading(L"Picture", 530);
        SectionHeading(L"General", 614);
        DrawControlValues(status);

        const bool front = status.facing == proto::kFacingFront;
        for (const auto& e : elements) {
            const bool isHot = hot == e.id, isDown = pressed == e.id && isHot;
            switch (e.kind) {
            case Kind::TitleClose: DrawTitleClose(e); break;
            case Kind::TitleMin: DrawTitleMin(e); break;
            case Kind::Segment: {
                const proto::CameraInfo& c = status.camera;
                bool selected = false;
                switch (e.id) {
                case kBack: case kFront: selected = (e.id == kFront) == front; break;
                case kQ720: selected = c.valid && c.quality == proto::kQuality720p; break;
                case kQ1080: selected = c.valid && c.quality == proto::kQuality1080p; break;
                case kQ4K: selected = c.valid && c.quality == proto::kQuality4K; break;
                // What this quality will run at: the chosen rate, or the best working one below it.
                case kFps30: selected = c.valid && EffectiveFps(c) == 30; break;
                case kFps60: selected = c.valid && EffectiveFps(c) == 60; break;
                case kFps120: selected = c.valid && EffectiveFps(c) == 120; break;
                case kFocusAuto: selected = c.valid && !(c.flags & proto::kCamFocusLocked); break;
                case kFocusLock: selected = c.valid && (c.flags & proto::kCamFocusLocked); break;
                default: break;
                }
                const bool on = Enabled(e.id);
                DrawAeroButton(e.rect, e.text, isHot && on, isDown && on, selected, on);
                break;
            }
            case Kind::Checkbox: {
                const proto::CameraInfo& c = status.camera;
                bool checked = e.id == kMirror ? model->mirror()
                             : e.id == kFill   ? model->fill()
                             : e.id == kTorch  ? (c.valid && (c.flags & proto::kCamTorchOn))
                                               : model->autostart();
                DrawCheckbox(e, checked, Enabled(e.id));
                break;
            }
            case Kind::Link: DrawLink(e); break;
            case Kind::Button:
                if (e.id == kPause) DrawAeroButton(e.rect, model->paused() ? L"Resume" : L"Pause", isHot, isDown, false, PauseEnabled());
                else DrawAeroButton(e.rect, e.text, isHot && Enabled(e.id), isDown && Enabled(e.id), false, Enabled(e.id));
                break;
            }
        }
        Text(L"The camera turns on only while an app is using it.", fontBody.Get(),
             Rect(x, 194, 320, 16), Rgb(kSubtle));
        if (keyboardCues) if (const Element* f = Find(focus)) DrawFocus(*f);

        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            rt.Reset();
            appIcon16.Reset();
            statusIcon.Reset();
        }
    }

    // --- Behaviour -------------------------------------------------------------------------------

    void Activate(Id id) {
        switch (id) {
        case kPause:
            if (PauseEnabled()) model->setPaused(!model->paused());
            break;
        case kBack: model->setFacing(proto::kFacingBack); break;
        case kFront: model->setFacing(proto::kFacingFront); break;
        case kMirror: model->setMirror(!model->mirror()); break;
        case kFill: model->setFill(!model->fill()); break;
        case kQ720: model->command(proto::kCmdSetQuality, proto::kQuality720p); break;
        case kQ1080: model->command(proto::kCmdSetQuality, proto::kQuality1080p); break;
        case kQ4K: model->command(proto::kCmdSetQuality, proto::kQuality4K); break;
        case kFps30: model->command(proto::kCmdSetFps, 30); break;
        case kFps60: model->command(proto::kCmdSetFps, 60); break;
        case kFps120: model->command(proto::kCmdSetFps, 120); break;
        case kZoomOut: case kZoomIn: case kZoomReset: {
            const proto::CameraInfo c = model->status().camera;
            double z = id == kZoomReset ? 1.0 : c.zoomX100 / 100.0 * (id == kZoomIn ? 1.25 : 0.8);
            z = std::clamp(z, c.zoomMinX100 / 100.0, c.zoomMaxX100 / 100.0);
            model->command(proto::kCmdSetZoom, uint8_t(std::clamp(int(z * 10 + 0.5), 1, 255)));
            break;
        }
        case kEvDown: model->command(proto::kCmdSetExposure, uint8_t(model->status().camera.ev - 1)); break;
        case kEvUp: model->command(proto::kCmdSetExposure, uint8_t(model->status().camera.ev + 1)); break;
        case kFocusAuto: model->command(proto::kCmdSetFocus, 0); break;
        case kFocusLock: model->command(proto::kCmdSetFocus, 1); break;
        case kTorch: model->command(proto::kCmdSetTorch, (model->status().camera.flags & proto::kCamTorchOn) ? 0 : 1); break;
        case kAuto:
            model->command(proto::kCmdSetZoom, 10);
            model->command(proto::kCmdSetExposure, 0);
            model->command(proto::kCmdSetFocus, 0);
            model->command(proto::kCmdSetTorch, 0);
            break;
        case kAutostart: model->setAutostart(!model->autostart()); break;
        case kReconnect: model->reconnect(); break;
        case kOpenLog: model->openLogFolder(); break;
        case kClose:
        case kTitleClose: DestroyWindow(hwnd); return;
        case kTitleMin: ShowWindow(hwnd, SW_MINIMIZE); return;
        default: return;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void MoveFocus(int step) {
        static const Id order[] = {kPause, kBack, kFront, kQ720, kQ1080, kQ4K, kFps30, kFps60, kFps120, kZoomOut, kZoomIn, kZoomReset,
                                   kEvDown, kEvUp, kFocusAuto, kFocusLock, kTorch, kAuto, kMirror, kFill, kAutostart,
                                   kReconnect, kOpenLog, kClose};
        constexpr int n = int(sizeof(order) / sizeof(order[0]));
        int i = 0;
        for (int k = 0; k < n; ++k) if (order[k] == focus) i = k;
        focus = order[(i + step + n) % n];
        keyboardCues = true;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void ToDips(LPARAM lp, float* x, float* y) const {
        *x = GET_X_LPARAM(lp) / scale;
        *y = GET_Y_LPARAM(lp) / scale;
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0; // The whole window is client area; we draw our own frame.
            break;
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x / scale, y = pt.y / scale;
            if (y < kTitleH && HitTest(x, y) != kTitleClose && HitTest(x, y) != kTitleMin) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (rt) rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;
        case WM_DPICHANGED: {
            scale = HIWORD(wp) / 96.f;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            rt.Reset();
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            float x, y;
            ToDips(lp, &x, &y);
            Id h = HitTest(x, y);
            if (h != hot) {
                hot = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, Find(h) && Find(h)->kind == Kind::Link ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            hot = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE; // Set in WM_MOUSEMOVE.
            break;
        case WM_LBUTTONDOWN: {
            float x, y;
            ToDips(lp, &x, &y);
            pressed = HitTest(x, y);
            if (pressed != kNone && pressed != kTitleClose && pressed != kTitleMin) focus = pressed;
            keyboardCues = false;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x, y;
            ToDips(lp, &x, &y);
            Id target = pressed;
            pressed = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (target != kNone && HitTest(x, y) == target) Activate(target);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
            case VK_TAB: MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
            case VK_RIGHT: case VK_DOWN: MoveFocus(1); return 0;
            case VK_LEFT: case VK_UP: MoveFocus(-1); return 0;
            case VK_SPACE: case VK_RETURN: keyboardCues = true; Activate(focus); return 0;
            case VK_ESCAPE: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_DESTROY:
            rt.Reset();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Create() {
        static bool registered = false;
        HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc = {sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 32, 32, 0));
            wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconApp), IMAGE_ICON, 16, 16, 0));
            wc.lpszClassName = L"MyCamSettings";
            RegisterClassExW(&wc);
            registered = true;
        }
        if (!d2d) CreateFactories();
        Layout();

        // Centre on the work area of the monitor under the cursor, at that monitor's DPI.
        POINT cursor;
        GetCursorPos(&cursor);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(monitor, &mi);
        UINT dpi = 96;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
                UINT dx = 96, dy = 96;
                if (SUCCEEDED(fn(monitor, 0, &dx, &dy))) dpi = dx;
            }
            FreeLibrary(shcore);
        }
        scale = dpi / 96.f;
        const int w = int(kWidth * scale + 0.5f), h = int(kHeight * scale + 0.5f);
        const RECT& wa = mi.rcWork;
        const int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;

        CreateWindowExW(WS_EX_APPWINDOW, L"MyCamSettings", L"MyCam Settings",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr, instance, this);
        if (!hwnd) return;
        scale = GetDpiForWindow(hwnd) / 96.f;

        // Keep the DWM drop shadow and use small rounded corners with a glass-coloured border.
        MARGINS margins = {0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        const DWORD corners = 3; // DWMWCP_ROUNDSMALL
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        const COLORREF border = RGB(0x3E, 0x5F, 0x8A);
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    }
};

SettingsWindow::~SettingsWindow() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
    delete impl_;
}

void SettingsWindow::Show() {
    if (!impl_) {
        impl_ = new Impl();
        impl_->model = &model_;
    }
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, IsIconic(impl_->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        return;
    }
    impl_->Create();
}

void SettingsWindow::Refresh() {
    if (impl_ && impl_->hwnd) InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

} // namespace mycam
