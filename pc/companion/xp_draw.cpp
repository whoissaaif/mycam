#include "xp_draw.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace mycam::xp {

namespace {
Box MakeBoxCentered(float cx, float cy, float w, float h) { return Box{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; }
} // namespace

// --- Surface ----------------------------------------------------------------------------------------

ID2D1Factory1* Surface::Factory() {
    if (!factory_) {
        D2D1_FACTORY_OPTIONS options = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                          reinterpret_cast<void**>(factory_.GetAddressOf()));
    }
    return factory_.Get();
}

bool Surface::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, UINT(std::size(levels)),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr)) { // No usable GPU (or a remote session without one): software rendering.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, UINT(std::size(levels)),
                               D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    }
    if (FAILED(hr) || !Factory()) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d_.As(&dxgi)) || FAILED(factory_->CreateDevice(dxgi.Get(), &d2dDevice_)) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)) ||
        FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_))) ||
        FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_)) || FAILED(dcomp_->CreateVisual(&visual_)) ||
        FAILED(dcompTarget_->SetRoot(visual_.Get()))) {
        Reset();
        return false;
    }
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque target.
    ++generation_;
    return true;
}

bool Surface::EnsureSize(UINT w, UINT h, float dpiScale) {
    if (swapChain_ && (w != width_ || h != height_)) {
        dc_->SetTarget(nullptr);
        target_.Reset();
        if (FAILED(swapChain_->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            Reset();
            return false;
        }
        width_ = w;
        height_ = h;
    }
    if (!swapChain_) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(d3d_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = w;
        desc.Height = h;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED; // Transparent rounded top corners.
        desc.Scaling = DXGI_SCALING_STRETCH;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swapChain_))) return false;
        visual_->SetContent(swapChain_.Get());
        dcomp_->Commit();
        width_ = w;
        height_ = h;
    }
    if (!target_ || dpiScale != dpiScale_) {
        dc_->SetTarget(nullptr);
        target_.Reset();
        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * dpiScale, 96.f * dpiScale);
        if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
        dc_->SetTarget(target_.Get());
        dc_->SetDpi(96.f * dpiScale, 96.f * dpiScale);
        dpiScale_ = dpiScale;
    }
    return true;
}

ID2D1DeviceContext* Surface::Begin(float dpiScale) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = UINT(std::max<LONG>(rc.right, 1)), h = UINT(std::max<LONG>(rc.bottom, 1));
    if (!dc_ && !CreateDevice()) return nullptr;
    if (!EnsureSize(w, h, dpiScale)) {
        Reset();
        return nullptr;
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return dc_.Get();
}

void Surface::End() {
    if (!dc_) return;
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        Reset();
        return;
    }
    hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) Reset();
}

void Surface::Reset() {
    if (dc_) dc_->SetTarget(nullptr);
    target_.Reset();
    if (visual_) visual_->SetContent(nullptr);
    swapChain_.Reset();
    visual_.Reset();
    dcompTarget_.Reset();
    dcomp_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    d3d_.Reset();
    width_ = height_ = 0;
}

// --- Settings -----------------------------------------------------------------------------------------

D2D1_COLOR_F SysColor(int index, float a) {
    const COLORREF c = GetSysColor(index);
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

bool ReducedMotion() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return !on;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc = {sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool KeyboardCuesAlways() {
    BOOL on = FALSE;
    SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &on, 0);
    return on != FALSE;
}

float TextScaleFactor() {
    wchar_t env[16] = {}; // Developer override: MYCAM_UI_TEXT_SCALE=1.5
    if (GetEnvironmentVariableW(L"MYCAM_UI_TEXT_SCALE", env, 16)) return std::clamp(float(_wtof(env)), 1.f, 2.25f);
    DWORD value = 100, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD, nullptr,
                 &value, &size);
    return std::clamp(value, 100ul, 225ul) / 100.f;
}

ComPtr<ID2D1Bitmap> LoadIconBitmap(ID2D1DeviceContext* dc, int resourceId, int px) {
    ComPtr<ID2D1Bitmap> out;
    HICON icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon || !dc) return out;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wicBitmap;
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &wicBitmap)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                        0, WICBitmapPaletteTypeCustom))) {
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties();
        dc->CreateBitmapFromWicBitmap(converter.Get(), &props, &out);
    }
    DestroyIcon(icon);
    return out;
}

// --- Painter: text --------------------------------------------------------------------------------------

void Painter::Init(IDWriteFactory* dwrite, float textScale) {
    dwrite_ = dwrite;
    textScale_ = textScale;
    CreateFormats();
    SetPalette(HighContrastOn());
}

void Painter::SetTextScale(float textScale) {
    if (textScale == textScale_) return;
    textScale_ = textScale;
    CreateFormats();
}

void Painter::CreateFormats() {
    // Tahoma and Trebuchet MS ship with Windows (never with MyCam); Segoe UI if they're missing (§9).
    static const FontSpec specs[int(Font::Count)] = {
        {L"Tahoma", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 8.f},
        {L"Tahoma", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 8.f},
        {L"Trebuchet MS", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 10.f},
        {L"Trebuchet MS", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 13.f},
        {L"Trebuchet MS", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 24.f},
    };
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(&fonts);
    for (int i = 0; i < int(Font::Count); ++i) {
        const FontSpec& s = specs[i];
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts) fonts->FindFamilyName(s.family, &index, &exists);
        const wchar_t* family = exists ? s.family : s.fallback;
        // Tahoma 8 pt renders as 11 px at 96 dpi; keep that size for the Segoe UI fallback too.
        const float px = std::round(s.points * 96.f / 72.f) * textScale_;
        for (ComPtr<IDWriteTextFormat>* out : {std::addressof(formats_[i]), std::addressof(ellipsisFormat_[i])}) {
            out->Reset();
            dwrite_->CreateTextFormat(family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                      L"en-us", out->GetAddressOf());
        }
        formats_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        ellipsisFormat_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        dwrite_->CreateEllipsisTrimmingSign(ellipsisFormat_[i].Get(), &sign);
        ellipsisFormat_[i]->SetTrimming(&trimming, sign.Get());
    }
    measureCache_.clear();
}

Size Painter::Measure(Font font, const std::wstring& text, float maxWidth) {
    const auto key = std::make_tuple(int(font), int(maxWidth * 4), text);
    auto it = measureCache_.find(key);
    if (it != measureCache_.end()) return it->second;
    ComPtr<IDWriteTextLayout> layout;
    Size size;
    if (SUCCEEDED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), formats_[int(font)].Get(), maxWidth, 10000.f,
                                            &layout))) {
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        size = {std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height)};
    }
    if (measureCache_.size() > 2000) measureCache_.clear();
    measureCache_[key] = size;
    return size;
}

float Painter::LineHeight(Font font) { return Measure(font, L"Ag").h; }

void Painter::Text(Font font, const std::wstring& text, const Box& box, D2D1_COLOR_F color, Align align, int underline,
                   bool ellipsis, bool vcenter) {
    if (text.empty() || !dc_) return;
    IDWriteTextFormat* format = (ellipsis ? ellipsisFormat_ : formats_)[int(font)].Get();
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), format, std::max(box.W(), 1.f),
                                         std::max(box.H(), 1.f), &layout))) {
        return;
    }
    layout->SetTextAlignment(align == Align::Center   ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                        : DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (underline >= 0 && underline < int(text.size())) layout->SetUnderline(TRUE, {UINT32(underline), 1});
    dc_->DrawTextLayout(D2D1::Point2F(box.l, box.t), layout.Get(), Brush(color), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

// --- Painter: primitives ------------------------------------------------------------------------------

ID2D1SolidColorBrush* Painter::Brush(D2D1_COLOR_F c) {
    if (!brush_ || brushOwner_ != dc_) {
        brush_.Reset();
        dc_->CreateSolidColorBrush(c, &brush_);
        brushOwner_ = dc_;
    }
    brush_->SetColor(c);
    return brush_.Get();
}

void Painter::Fill(const Box& b, D2D1_COLOR_F c) { dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), Brush(c)); }

void Painter::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width) {
    dc_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush(c), width);
}

void Painter::Frame(const Box& b, D2D1_COLOR_F c, float width) {
    const float h = width / 2;
    dc_->DrawRectangle(D2D1::RectF(b.l + h, b.t + h, b.r - h, b.b - h), Brush(c), width);
}

ComPtr<ID2D1LinearGradientBrush> Painter::Gradient(float x0, float y0, float x1, float y1, std::initializer_list<Stop> stops) {
    std::vector<D2D1_GRADIENT_STOP> s;
    for (const Stop& st : stops) s.push_back({st.pos, Rgb(st.rgb, st.alpha)});
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    dc_->CreateGradientStopCollection(s.data(), UINT32(s.size()), &collection);
    if (collection) {
        dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)),
                                       collection.Get(), &brush);
    }
    return brush;
}

void Painter::VGradient(const Box& b, std::initializer_list<Stop> stops, float radius) {
    auto brush = Gradient(0, b.t, 0, b.b, stops);
    if (!brush) return;
    if (radius > 0) dc_->FillRoundedRectangle({D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius}, brush.Get());
    else dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

void Painter::HGradient(const Box& b, std::initializer_list<Stop> stops) {
    auto brush = Gradient(b.l, 0, b.r, 0, stops);
    if (brush) dc_->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

ComPtr<ID2D1PathGeometry> Painter::TopRounded(const Box& b, float r) {
    ComPtr<ID2D1PathGeometry> path;
    factory_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    if (!path || FAILED(path->Open(&sink))) return path;
    sink->BeginFigure(D2D1::Point2F(b.l, b.b), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(b.l, b.t + r));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(b.l + r, b.t), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(b.r - r, b.t));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(b.r, b.t + r), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(b.r, b.b));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

void Painter::SetPalette(bool hc) {
    pal_.highContrast = hc;
    if (hc) {
        pal_.surface = SysColor(COLOR_WINDOW);
        pal_.text = SysColor(COLOR_WINDOWTEXT);
        pal_.subtle = SysColor(COLOR_WINDOWTEXT);
        pal_.disabledText = SysColor(COLOR_GRAYTEXT);
        pal_.link = SysColor(COLOR_HOTLIGHT);
        pal_.linkHot = SysColor(COLOR_HOTLIGHT);
        pal_.selection = SysColor(COLOR_HIGHLIGHT);
        pal_.selectionText = SysColor(COLOR_HIGHLIGHTTEXT);
        pal_.paneTop = pal_.paneBottom = SysColor(COLOR_WINDOW);
        pal_.groupBody = SysColor(COLOR_WINDOW);
        pal_.groupBorder = SysColor(COLOR_WINDOWTEXT);
        pal_.groupTitle = SysColor(COLOR_BTNTEXT);
        pal_.heroTitle = SysColor(COLOR_HIGHLIGHTTEXT);
        pal_.frame = SysColor(COLOR_ACTIVEBORDER);
        pal_.focus = SysColor(COLOR_WINDOWTEXT);
        pal_.card = SysColor(COLOR_WINDOW);
        return;
    }
    // redesign.md §3.2 (Luna Blue).
    pal_.surface = Rgb(0xECE9D8);
    pal_.text = Rgb(0x000000);
    pal_.subtle = Rgb(0x4D4D4D);
    pal_.disabledText = Rgb(0xACA899);
    pal_.link = Rgb(0x215DC6);
    pal_.linkHot = Rgb(0x428EFF);
    pal_.selection = Rgb(0x316AC5);
    pal_.selectionText = Rgb(0xFFFFFF);
    pal_.paneTop = Rgb(0x7BA2E7);
    pal_.paneBottom = Rgb(0x6375D6);
    pal_.groupBody = Rgb(0xD6DFF7);
    pal_.groupBorder = Rgb(0xFFFFFF);
    pal_.groupTitle = Rgb(0x215DC6);
    pal_.heroTitle = Rgb(0xFFFFFF);
    pal_.frame = Rgb(0x0831D9);
    pal_.focus = Rgb(0x000000);
    pal_.card = Rgb(0xFFFFFF);
}

// --- Painter: XP widgets ---------------------------------------------------------------------------

void Painter::TitleBar(const Box& b, bool active, float radius) {
    auto shape = TopRounded(b, radius);
    if (pal_.highContrast) {
        dc_->FillGeometry(shape.Get(), Brush(SysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION)));
        return;
    }
    // §3.2 TitleBar: #3D8AF7 → #0A5DEB (8 %) → #0053E1 (50 %) → #0047D0, highlight #7FB6FF, edge #0831D9.
    auto fill = active ? Gradient(0, b.t, 0, b.b, {{0.f, 0x3D8AF7}, {0.08f, 0x0A5DEB}, {0.5f, 0x0053E1}, {0.92f, 0x0047D0}, {1.f, 0x0041C0}})
                       : Gradient(0, b.t, 0, b.b, {{0.f, 0xA9C0F2}, {0.08f, 0x8FAAEC}, {0.5f, 0x7E9BE6}, {1.f, 0x7792E0}});
    if (fill) dc_->FillGeometry(shape.Get(), fill.Get());
    // The XP gloss: a lighter top band and a 1 px highlight line under the top edge.
    dc_->PushAxisAlignedClip(D2D1::RectF(b.l, b.t, b.r, b.t + b.H() * 0.42f), D2D1_ANTIALIAS_MODE_ALIASED);
    auto gloss = Gradient(0, b.t, 0, b.t + b.H() * 0.42f, {{0.f, 0xFFFFFF, active ? 0.22f : 0.18f}, {1.f, 0xFFFFFF, 0.f}});
    if (gloss) dc_->FillGeometry(shape.Get(), gloss.Get());
    dc_->PopAxisAlignedClip();
    Line(b.l + radius * 0.6f, b.t + 1.5f, b.r - radius * 0.6f, b.t + 1.5f, Rgb(active ? 0x7FB6FF : 0xD3DEF8, 0.9f));
    dc_->DrawGeometry(shape.Get(), Brush(Rgb(active ? 0x0831D9 : 0x7A93DF)), 1.f);
}

void Painter::CaptionButton(const Box& b, bool isClose, const ButtonState& s) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(s.hot ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawRoundedRectangle(rr, Brush(SysColor(COLOR_BTNTEXT)));
    } else {
        ComPtr<ID2D1LinearGradientBrush> fill;
        if (isClose) { // §3.2 ErrorRed #FF7B6B → #C81E0F.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0xC8462F}, {0.5f, 0xB52A15}, {1.f, 0xD45A3E}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0xFFA898}, {0.45f, 0xF26A50}, {1.f, 0xD8331C}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0xFF8E7E}, {0.45f, 0xE9563E}, {1.f, 0xC81E0F}});
        } else {
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0x1F4FC0}, {0.5f, 0x2A5FD4}, {1.f, 0x3F78E6}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0x8FB8FF}, {0.45f, 0x5590F8}, {1.f, 0x2D68E2}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0x6EA1F8}, {0.45f, 0x3B78EE}, {1.f, 0x1E56D8}});
        }
        if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
        // Soft inner light at the top-left, the white XP rim.
        auto shine = Gradient(b.l, b.t, b.r, b.b, {{0.f, 0xFFFFFF, 0.35f}, {0.5f, 0xFFFFFF, 0.f}});
        if (shine && !s.pressed) dc_->FillRoundedRectangle({D2D1::RectF(b.l + 1.5f, b.t + 1.5f, b.r - 1.5f, b.b - 1.5f), 2, 2}, shine.Get());
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(0xFFFFFF, 0.95f)), 1.f);
    }
    const float shift = s.pressed ? 1.f : 0.f;
    const float cx = (b.l + b.r) / 2 + shift, cy = (b.t + b.b) / 2 + shift;
    const D2D1_COLOR_F ink = pal_.highContrast ? SysColor(s.hot ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT) : Rgb(0xFFFFFF);
    if (isClose) {
        const float d = b.W() * 0.2f;
        auto cross = [&](D2D1_COLOR_F c, float w, float o) {
            Line(cx - d + o, cy - d + o, cx + d + o, cy + d + o, c, w);
            Line(cx + d + o, cy - d + o, cx - d + o, cy + d + o, c, w);
        };
        if (!pal_.highContrast) cross(Rgb(0x7A1405, 0.45f), 2.2f, 0.7f);
        cross(ink, 2.2f, 0);
    } else {
        const float w = b.W() * 0.36f;
        const Box bar = MakeBoxCentered(cx - b.W() * 0.08f, b.b - b.H() * 0.3f + shift, w, 2.5f);
        if (!pal_.highContrast) Fill(bar.Offset(0.6f, 0.6f), Rgb(0x0A2C8C, 0.45f));
        Fill(bar, ink);
    }
}

void Painter::PushButton(const Box& b, const std::wstring& label, int underline, const ButtonState& s, ButtonStyle style,
                         Font font) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    const float shift = s.pressed && !s.disabled ? 1.f : 0.f; // XP pressed state: contents move 1 px.
    D2D1_COLOR_F ink;
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(s.hot && !s.disabled ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawRoundedRectangle(rr, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT)), s.isDefault ? 2.f : 1.f);
        ink = SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT);
    } else if (s.disabled) {
        dc_->FillRoundedRectangle(rr, Brush(Rgb(0xF5F4EA)));
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(0xC9C7BA)));
        ink = Rgb(0xACA899);
    } else {
        const bool green = style == ButtonStyle::Green;
        ComPtr<ID2D1LinearGradientBrush> fill;
        if (green) { // §3.2 GoGreen #3FAA3F → #2A7F2A, border #1D5E1D.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0x2A7F2A}, {1.f, 0x3A9E3A}})
                 : s.hot     ? Gradient(0, b.t, 0, b.b, {{0.f, 0x5CC45C}, {0.5f, 0x43AE43}, {1.f, 0x2E872E}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0x4DB84D}, {0.5f, 0x3FAA3F}, {1.f, 0x2A7F2A}});
        } else { // Button normal: #FFFFFF → #ECEBE6 (85 %) → #D6D0C5.
            fill = s.pressed ? Gradient(0, b.t, 0, b.b, {{0.f, 0xE3E2DA}, {0.8f, 0xE6E5DE}, {1.f, 0xF1F0EB}})
                             : Gradient(0, b.t, 0, b.b, {{0.f, 0xFFFFFF}, {0.85f, 0xECEBE6}, {1.f, 0xD6D0C5}});
        }
        if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
        // Hot track (XP orange) or default ring (blue): a 2 px inner ring.
        if ((s.hot && !s.pressed) || s.isDefault) {
            auto ring = s.hot ? Gradient(0, b.t, 0, b.b, {{0.f, 0xFFF0CF}, {0.3f, 0xFFCF6B}, {1.f, 0xE5A01A}})
                              : Gradient(0, b.t, 0, b.b, {{0.f, 0xCEE7FF}, {0.4f, 0xA9C9FC}, {1.f, 0x6982EE}});
            if (ring) dc_->DrawRoundedRectangle({D2D1::RectF(b.l + 2, b.t + 2, b.r - 2, b.b - 2), 2, 2}, ring.Get(), 2.f);
        }
        dc_->DrawRoundedRectangle(rr, Brush(Rgb(green ? 0x1D5E1D : 0x003C74)));
        ink = green ? Rgb(0xFFFFFF) : Rgb(0x000000);
    }
    Text(font, label, b.Offset(shift, shift), ink, Align::Center, underline, false, true);
}

void Painter::Checkbox(const Box& box, bool checked, const ButtonState& s) {
    const D2D1_RECT_F r = D2D1::RectF(box.l + 0.5f, box.t + 0.5f, box.r - 0.5f, box.b - 0.5f);
    D2D1_COLOR_F checkInk;
    if (pal_.highContrast) {
        Fill(box, SysColor(COLOR_WINDOW));
        dc_->DrawRectangle(r, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)));
        checkInk = SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT);
    } else {
        if (s.disabled) Fill(box, Rgb(0xFFFFFF));
        else if (s.pressed) {
            auto g = Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xB0B0A7}, {1.f, 0xE3E1D4}});
            if (g) dc_->FillRectangle(r, g.Get());
        } else {
            auto g = Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xDCDCD7}, {1.f, 0xFFFFFF}});
            if (g) dc_->FillRectangle(r, g.Get());
        }
        if (s.hot && !s.disabled && !s.pressed) {
            auto ring = Gradient(0, box.t, 0, box.b, {{0.f, 0xFFF0CF}, {1.f, 0xF8B330}});
            if (ring) dc_->DrawRectangle(D2D1::RectF(box.l + 2, box.t + 2, box.r - 2, box.b - 2), ring.Get(), 2.f);
        }
        dc_->DrawRectangle(r, Brush(Rgb(s.disabled ? 0xCAC8BB : 0x1C5180)));
        checkInk = s.disabled ? Rgb(0xCAC8BB) : Rgb(0x21A121); // XP checks are green.
    }
    if (!checked) return;
    ComPtr<ID2D1PathGeometry> path;
    factory_->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    if (!path || FAILED(path->Open(&sink))) return;
    const float u = box.W() / 13.f;
    sink->BeginFigure(D2D1::Point2F(box.l + 3.2f * u, box.t + 6.2f * u), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(D2D1::Point2F(box.l + 5.4f * u, box.t + 8.6f * u));
    sink->AddLine(D2D1::Point2F(box.l + 9.9f * u, box.t + 3.9f * u));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> round;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                            D2D1_LINE_JOIN_ROUND),
                                nullptr, 0, &round);
    dc_->DrawGeometry(path.Get(), Brush(checkInk), 2.2f * u, round.Get());
}

void Painter::Radio(const Box& box, bool selected, const ButtonState& s, float dotScale) {
    const float cx = (box.l + box.r) / 2, cy = (box.t + box.b) / 2, rad = box.W() / 2 - 0.5f;
    const D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    if (pal_.highContrast) {
        dc_->FillEllipse(outer, Brush(SysColor(COLOR_WINDOW)));
        dc_->DrawEllipse(outer, Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : s.hot ? COLOR_HIGHLIGHT : COLOR_WINDOWTEXT)));
        if (selected) {
            const float d = rad * 0.45f * dotScale;
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), Brush(SysColor(s.disabled ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT)));
        }
        return;
    }
    if (s.disabled) dc_->FillEllipse(outer, Brush(Rgb(0xFFFFFF)));
    else {
        auto g = s.pressed ? Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xB0B0A7}, {1.f, 0xE3E1D4}})
                           : Gradient(box.l, box.t, box.r, box.b, {{0.f, 0xDCDCD7}, {1.f, 0xFFFFFF}});
        if (g) dc_->FillEllipse(outer, g.Get());
    }
    if (s.hot && !s.disabled && !s.pressed) {
        auto ring = Gradient(0, box.t, 0, box.b, {{0.f, 0xFFF0CF}, {1.f, 0xF8B330}});
        if (ring) dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad - 1.5f, rad - 1.5f), ring.Get(), 2.f);
    }
    dc_->DrawEllipse(outer, Brush(Rgb(s.disabled ? 0xCAC8BB : 0x1C5180)));
    if (selected && dotScale > 0) { // Green dot #7BD87B → #21A121.
        const float d = rad * 0.46f * dotScale;
        if (s.disabled) {
            dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), Brush(Rgb(0xCAC8BB)));
        } else {
            auto dot = Gradient(cx - d, cy - d, cx + d, cy + d, {{0.f, 0x7BD87B}, {1.f, 0x21A121}});
            if (dot) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d, d), dot.Get());
        }
    }
}

void Painter::GroupHeader(const Box& b, const std::wstring& title, bool hero, Chevron chevron, float turn, bool hot) {
    auto shape = TopRounded(b, 4);
    if (pal_.highContrast) {
        dc_->FillGeometry(shape.Get(), Brush(SysColor(hero ? COLOR_HIGHLIGHT : COLOR_BTNFACE)));
        dc_->DrawGeometry(shape.Get(), Brush(SysColor(COLOR_WINDOWTEXT)));
    } else {
        // §3.2 GroupHeader #FFFFFF → #C6D3F7 (title #215DC6), GroupHeaderHero #0055E5 → #2463D6 (white title).
        auto fill = hero ? Gradient(b.l, 0, b.r, 0, {{0.f, 0x0055E5}, {1.f, 0x2463D6}})
                         : Gradient(b.l, 0, b.r, 0, {{0.f, 0xFFFFFF}, {1.f, 0xC6D3F7}});
        if (fill) dc_->FillGeometry(shape.Get(), fill.Get());
    }
    const D2D1_COLOR_F titleInk = pal_.highContrast ? SysColor(hero ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
                                  : hero            ? Rgb(0xFFFFFF)
                                  : hot             ? Rgb(0x428EFF)
                                                    : Rgb(0x215DC6);
    const float chevronSpace = chevron == Chevron::None ? 0.f : 24.f;
    Text(Font::BodyBold, title, Box{b.l + 12, b.t, b.r - chevronSpace - 4, b.b}, titleInk, Align::Leading, -1, true, true);
    if (chevron == Chevron::None) return;

    // The round XP chevron button with a double chevron, rotating as the group opens and closes.
    const float cx = b.r - 14, cy = (b.t + b.b) / 2, rad = 8.5f;
    const D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    D2D1_COLOR_F ink;
    if (pal_.highContrast) {
        dc_->DrawEllipse(circle, Brush(titleInk));
        ink = titleInk;
    } else if (hero) {
        dc_->FillEllipse(circle, Brush(Rgb(0xFFFFFF, hot ? 0.3f : 0.15f)));
        dc_->DrawEllipse(circle, Brush(Rgb(0xFFFFFF, 0.85f)));
        ink = Rgb(0xFFFFFF);
    } else {
        auto g = Gradient(0, cy - rad, 0, cy + rad, {{0.f, 0xFFFFFF}, {1.f, hot ? 0xFFE7B0u : 0xD9E2F9u}});
        if (g) dc_->FillEllipse(circle, g.Get());
        dc_->DrawEllipse(circle, Brush(Rgb(hot ? 0xE5A01A : 0xA3B4E6)));
        ink = Rgb(hot ? 0x428EFF : 0x215DC6);
    }
    D2D1_MATRIX_3X2_F saved;
    dc_->GetTransform(&saved);
    dc_->SetTransform(D2D1::Matrix3x2F::Rotation(180.f * turn, D2D1::Point2F(cx, cy)) * saved);
    for (float dy : {-2.5f, 1.5f}) { // Two stacked "^" chevrons.
        Line(cx - 3.2f, cy + dy + 2.f, cx, cy + dy - 1.2f, ink, 1.6f);
        Line(cx, cy + dy - 1.2f, cx + 3.2f, cy + dy + 2.f, ink, 1.6f);
    }
    dc_->SetTransform(saved);
}

void Painter::GroupBody(const Box& b, bool hero) {
    if (pal_.highContrast) {
        Fill(b, SysColor(COLOR_WINDOW));
        Frame(b, SysColor(COLOR_WINDOWTEXT));
        return;
    }
    Fill(b, hero ? Rgb(0xEEF3FF) : Rgb(0xD6DFF7));
    // White 1 px border on the sides and bottom (the header covers the top).
    Line(b.l + 0.5f, b.t, b.l + 0.5f, b.b, Rgb(0xFFFFFF));
    Line(b.r - 0.5f, b.t, b.r - 0.5f, b.b, Rgb(0xFFFFFF));
    Line(b.l, b.b - 0.5f, b.r, b.b - 0.5f, Rgb(0xFFFFFF));
}

void Painter::ProgressTrack(const Box& b) {
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
    dc_->FillRoundedRectangle(rr, Brush(pal_.highContrast ? SysColor(COLOR_WINDOW) : Rgb(0xFFFFFF)));
    dc_->DrawRoundedRectangle(rr, Brush(pal_.highContrast ? SysColor(COLOR_WINDOWTEXT) : Rgb(0xACA899)));
}

namespace {
Box Interior(const Box& b) { return Box{b.l + 3, b.t + 2.5f, b.r - 3, b.b - 2.5f}; }
} // namespace

void Painter::ProgressMarquee(const Box& b, float offset) {
    ProgressTrack(b);
    const Box in = Interior(b);
    dc_->PushAxisAlignedClip(D2D1::RectF(in.l, in.t, in.r, in.b), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < 3; ++i) {
        const float x = in.l + offset + i * (kChunk + kChunkGap);
        const Box chunk = {x, in.t, x + kChunk, in.b};
        if (pal_.highContrast) Fill(chunk, SysColor(COLOR_HIGHLIGHT));
        else VGradient(chunk, {{0.f, 0xE2F8E2}, {0.45f, 0x6FD86F}, {0.55f, 0x2DB52D}, {1.f, 0x5ACD5A}});
    }
    dc_->PopAxisAlignedClip();
}

void Painter::ProgressChunks(const Box& b, int chunks) {
    ProgressTrack(b);
    const Box in = Interior(b);
    dc_->PushAxisAlignedClip(D2D1::RectF(in.l, in.t, in.r, in.b), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < chunks; ++i) {
        const float x = in.l + i * (kChunk + kChunkGap);
        const Box chunk = {x, in.t, std::min(x + kChunk, in.r), in.b};
        if (pal_.highContrast) Fill(chunk, SysColor(COLOR_HIGHLIGHT));
        else VGradient(chunk, {{0.f, 0xE2F8E2}, {0.45f, 0x6FD86F}, {0.55f, 0x2DB52D}, {1.f, 0x5ACD5A}});
    }
    dc_->PopAxisAlignedClip();
}

void Painter::LivePill(const Box& b, float glow, float opacity) {
    const float rad = b.H() / 2;
    const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), rad, rad};
    if (pal_.highContrast) {
        dc_->FillRoundedRectangle(rr, Brush(SysColor(COLOR_HIGHLIGHT)));
        Text(Font::BodyBold, L"LIVE", b, SysColor(COLOR_HIGHLIGHTTEXT), Align::Center, -1, false, true);
        return;
    }
    auto fill = Gradient(0, b.t, 0, b.b, {{0.f, 0x4DB84D, opacity}, {0.5f, 0x3FAA3F, opacity}, {1.f, 0x2A7F2A, opacity}});
    if (fill) dc_->FillRoundedRectangle(rr, fill.Get());
    auto top = Gradient(0, b.t, 0, (b.t + b.b) / 2, {{0.f, 0xFFFFFF, 0.35f * opacity}, {1.f, 0xFFFFFF, 0.f}});
    if (top) dc_->FillRoundedRectangle({D2D1::RectF(b.l + 1.5f, b.t + 1.5f, b.r - 1.5f, (b.t + b.b) / 2), rad - 1, rad - 1}, top.Get());
    if (glow >= 0 && glow <= 1) { // The one-shot gloss sweep when streaming starts (§6.3).
        const float x = b.l - b.W() * 0.6f + (b.W() * 2.2f) * glow;
        auto band = Gradient(x - 14, 0, x + 14, 0, {{0.f, 0xFFFFFF, 0.f}, {0.5f, 0xFFFFFF, 0.75f * opacity}, {1.f, 0xFFFFFF, 0.f}});
        if (band) dc_->FillRoundedRectangle(rr, band.Get());
    }
    dc_->DrawRoundedRectangle(rr, Brush(Rgb(0x1D5E1D, opacity)));
    Text(Font::BodyBold, L"LIVE", b, Rgb(0xFFFFFF, opacity), Align::Center, -1, false, true);
}

void Painter::Badge(float cx, float cy, float radius, BadgeKind kind, float scale) {
    if (kind == BadgeKind::None || scale <= 0) return;
    const float r = radius * scale;
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
    if (pal_.highContrast) {
        dc_->FillEllipse(e, Brush(SysColor(COLOR_WINDOWTEXT)));
    } else {
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2 * scale, r + 2 * scale), Brush(Rgb(0xFFFFFF)));
        auto fill = kind == BadgeKind::Play  ? Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0x7BD87B}, {1.f, 0x1E8E1E}})
                  : kind == BadgeKind::Pause ? Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0xFFD86A}, {1.f, 0xE08A00}})
                                             : Gradient(cx - r, cy - r, cx + r, cy + r, {{0.f, 0xFF7B6B}, {1.f, 0xC81E0F}});
        if (fill) dc_->FillEllipse(e, fill.Get());
        auto gloss = Gradient(0, cy - r, 0, cy, {{0.f, 0xFFFFFF, 0.55f}, {1.f, 0xFFFFFF, 0.f}});
        if (gloss) dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - r * 0.35f), r * 0.75f, r * 0.55f), gloss.Get());
    }
    const D2D1_COLOR_F ink = pal_.highContrast ? SysColor(COLOR_WINDOW) : Rgb(0xFFFFFF);
    if (kind == BadgeKind::Play) {
        ComPtr<ID2D1PathGeometry> tri;
        factory_->CreatePathGeometry(&tri);
        ComPtr<ID2D1GeometrySink> sink;
        if (tri && SUCCEEDED(tri->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx - r * 0.3f, cy - r * 0.45f), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(cx + r * 0.5f, cy));
            sink->AddLine(D2D1::Point2F(cx - r * 0.3f, cy + r * 0.45f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc_->FillGeometry(tri.Get(), Brush(ink));
        }
    } else if (kind == BadgeKind::Pause) {
        Fill(Box{cx - r * 0.42f, cy - r * 0.45f, cx - r * 0.12f, cy + r * 0.45f}, ink);
        Fill(Box{cx + r * 0.12f, cy - r * 0.45f, cx + r * 0.42f, cy + r * 0.45f}, ink);
    } else {
        const float d = r * 0.38f;
        Line(cx - d, cy - d, cx + d, cy + d, ink, r * 0.28f);
        Line(cx + d, cy - d, cx - d, cy + d, ink, r * 0.28f);
    }
}

void Painter::FocusRect(const Box& b) {
    ComPtr<ID2D1StrokeStyle> dots;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                            D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_DOT),
                                nullptr, 0, &dots);
    dc_->DrawRectangle(D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), Brush(pal_.focus), 1.f, dots.Get());
}

Box CloseButtonBox(float width) { return ui::MakeBox(width - 5 - 21, 5, 21, 21); }
Box MinimizeButtonBox(float width) { return ui::MakeBox(width - 5 - 21 - 2 - 21, 5, 21, 21); }

void WindowChrome(Painter& p, float w, float h, const std::wstring& caption, bool active, ID2D1Bitmap* icon) {
    ID2D1DeviceContext* dc = p.Dc();
    const bool hc = p.HighContrast();
    // Borders: everything below the rounded corners, then the title bar over the top.
    const Box frame = {0, kCornerRadius, w, h};
    if (hc) {
        p.Fill(frame, SysColor(active ? COLOR_ACTIVEBORDER : COLOR_INACTIVEBORDER));
        p.Frame(frame, SysColor(COLOR_WINDOWFRAME));
    } else {
        p.Fill(frame, Rgb(active ? 0x0B4BD9 : 0x7C98E2));
        p.Frame(frame, Rgb(active ? 0x002EA8 : 0x6A86D6));
        p.Frame(frame.Inset(1, 1), Rgb(active ? 0x2A6DF0 : 0x93ACEA));
    }
    p.TitleBar(Box{0, 0, w, kTitleBarH}, active, kCornerRadius);
    float x = 8;
    if (icon) {
        dc->DrawBitmap(icon, D2D1::RectF(x, 7, x + 16, 23), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        x += 16 + 6;
    }
    const Box text = {x, 0, w - 60, kTitleBarH};
    if (!hc) p.Text(Font::Caption, caption, text.Offset(1, 1), Rgb(0x0A1E78, active ? 0.9f : 0.4f), Align::Leading, -1, true, true);
    p.Text(Font::Caption, caption, text, hc ? SysColor(active ? COLOR_CAPTIONTEXT : COLOR_INACTIVECAPTIONTEXT) : Rgb(0xFFFFFF),
           Align::Leading, -1, true, true);
}

void Painter::SunkenFrame(const Box& b) {
    // XP's list/preview border (#7F9DB9), with a hairline shadow inside.
    Frame(b, pal_.highContrast ? SysColor(COLOR_WINDOWTEXT) : Rgb(0x7F9DB9));
}

} // namespace mycam::xp
