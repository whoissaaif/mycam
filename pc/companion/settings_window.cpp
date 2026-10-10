#include "settings_window.h"

#include <d2d1effects.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app_settings.h"
#include "capabilities.h"
#include "preview_source.h"
#include "protocol.h"
#include "status_images.h"
#include "status_text.h"
#include "ui_layout.h"
#include "ui_motion.h"
#include "xp_draw.h"

namespace mycam {

using namespace ui;
using xp::Align;
using xp::ButtonState;
using xp::Font;
using xp::Rgb;
using Microsoft::WRL::ComPtr;

namespace {

// Window geometry, DIPs (redesign.md §8.1): 720 x 500 fits a 720 DIP work area.
constexpr float kW = 720, kH = 500;
constexpr float kPaneW = 240;           // XP task pane
constexpr float kPad = 12;              // Task pane padding, gap between groups
constexpr float kHeaderH = 25;          // Task group header
constexpr float kBodyPad = 10;          // Task group body padding
constexpr float kRightPad = 16;         // Right side padding

constexpr UINT_PTR kTimerAnim = 1, kTimerPreview = 2;
constexpr uint64_t kApplyTimeoutMs = 4000;

struct GroupDef {
    int id;
    const wchar_t* title;
    bool hero;
    DWORD bit; // Expanded-state bit in HKCU\Software\MyCam\PaneGroups (0: always open).
};
const GroupDef kGroups[] = {
    {kGroupNow, L"Now", true, 0},
    {kGroupCamera, L"Camera", false, 1},
    {kGroupVideo, L"Video", false, 2},
    {kGroupPicture, L"Picture", false, 4},
    {kGroupWifi, L"Wi-Fi and startup", false, 8},
    {kGroupTasks, L"Troubleshooting", false, 16},
};
constexpr DWORD kDefaultOpenGroups = 1 | 2 | 4 | 16;

// One element as laid out and drawn.
struct Item {
    Element e;
    std::wstring label; // What is drawn (the accessible name may say more).
    float alpha = 1;    // Fades with its group's expand/collapse.
};

// Static text that isn't an element of its own (row labels).
struct Deco {
    int parent;
    Box box;
    std::wstring text;
    bool subtle;
    bool dim; // Drawn in the disabled colour.
};

struct GroupGeo {
    int id;
    std::wstring title;
    bool hero, expandable;
    Box header, body; // body: as shown (animated height)
    float open;
};

xp::BadgeKind BadgeFor(int icon) {
    switch (icon) {
    case kIconStreaming: return xp::BadgeKind::Play;
    case kIconPaused: return xp::BadgeKind::Pause;
    case kIconError: return xp::BadgeKind::Error;
    default: return xp::BadgeKind::None;
    }
}

bool InPane(const Element& e) { return e.rect.l < kPaneW; }

} // namespace

struct SettingsWindow::Impl {
    SettingsWindow* owner = nullptr;
    SettingsModel* model = nullptr;
    HWND hwnd = nullptr;
    float scale = 1.f, forcedScale = 0;
    bool active = true;

    xp::Surface surface;
    xp::Painter paint;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1Bitmap> appIcon16, statusIcon;
    int statusIconId = 0, iconGen = -1;
    float iconScale = 0;

    // Layout (rebuilt by Layout()).
    std::vector<Item> items;
    std::vector<Deco> decos;
    std::vector<GroupGeo> groups;
    Box pane, paneView, right, previewBox, sentenceBox, controlsBox;
    float scroll = 0, contentH = 0;
    size_t lastSignature = 0;

    // Interaction.
    int hot = kNone, pressed = kNone, focus = kNone;
    bool keyboardCues = false, tracking = false, sysKeyHandled = false;
    bool thumbDrag = false, thumbHot = false;
    float dragStartY = 0, dragStartScroll = 0;

    // Group expansion.
    DWORD openBits = kDefaultOpenGroups;
    Tween groupOpen[std::size(kGroups)];

    // Motion and accessibility settings.
    bool reducedMotion = false;
    Tween openFade, statusFade, badgePop, liveGlow;
    StatusView view = {}, prevView = {};
    bool viewShown = false, wasLive = false;
    uint64_t pairingSince = 0;
    std::wstring pairingCode;
    uint64_t applyingSince = 0;
    int applyKind = 0, applyValue = 0; // 1 quality, 2 fps, 3 facing

    // Preview.
    PreviewSource preview;
    std::vector<uint8_t> previewPixels;
    bool previewPending = false, previewFresh = false;
    Tween previewFade; // 0: status picture, 1: live frames.
    ComPtr<ID2D1Bitmap1> liveBitmap, artBitmap;
    UINT pw = 0, ph = 0;     // Preview size in pixels.
    int artKind = -1, artGen = -1;
    UINT artW = 0, artH = 0;
    Nv12Image pausedImage, waitingImage;
    bool imagesLoaded = false;
    ComPtr<ID2D1Effect> blur, artBlur;
    int effectGen = -1;

    static uint64_t Now() { return GetTickCount64(); }
    float Dur(float ms) const { return reducedMotion || paint.HighContrast() ? 0.f : ms; }

    // --- State ---------------------------------------------------------------------------------------

    LinkStatus Status() const { return model->status(); }
    static bool Connected(const LinkStatus& s) { return s.state >= LinkState::Idle && s.camera.valid; }

    bool PauseEnabled(const LinkStatus& s) const { return s.state >= LinkState::Idle && !s.lockPaused; }

    bool Enabled(int id, const LinkStatus& s) const {
        const proto::CameraInfo& c = s.camera;
        const bool connected = Connected(s);
        const bool known = caps::Known(connected, c);
        switch (id) {
        case kPause: return PauseEnabled(s);
        case kBack: case kFront: return true;
        case kQ720: return caps::QualityEnabled(connected, c, proto::kQuality720p);
        case kQ1080: return caps::QualityEnabled(connected, c, proto::kQuality1080p);
        case kQ4K: return caps::QualityEnabled(connected, c, proto::kQuality4K);
        case kFps30: return caps::FpsEnabled(connected, c, 30);
        case kFps60: return caps::FpsEnabled(connected, c, 60);
        case kFps120: return caps::FpsEnabled(connected, c, 120);
        case kZoomOut: return caps::ZoomOutEnabled(known, c);
        case kZoomIn: return caps::ZoomInEnabled(known, c);
        case kZoomReset: return caps::ZoomResetEnabled(known, c);
        case kEvDown: return caps::EvDownEnabled(known, c);
        case kEvUp: return caps::EvUpEnabled(known, c);
        case kFocusAuto: case kFocusLock: return caps::FocusEnabled(known, c);
        case kTorch: return caps::TorchEnabled(known, c);
        case kAuto: return caps::AutoEnabled(known, c);
        default: return true;
        }
    }

    bool Applying(const LinkStatus& s, uint64_t now) {
        if (!applyingSince) return false;
        const bool done = now - applyingSince > kApplyTimeoutMs || !Connected(s) ||
                          (applyKind == 1 && s.camera.quality == applyValue) ||
                          (applyKind == 2 && s.camera.fps == applyValue) || (applyKind == 3 && s.facing == applyValue);
        if (done) applyingSince = 0;
        return !done;
    }

    void StartApplying(int kind, int value, const LinkStatus& s) {
        const bool already = (kind == 1 && s.camera.quality == value) || (kind == 2 && s.camera.fps == value) ||
                             (kind == 3 && s.facing == value);
        if (already || !Connected(s)) return;
        applyingSince = Now();
        applyKind = kind;
        applyValue = value;
    }

    // Picks up a new status: crossfade, badge pop, LIVE glow (redesign.md §6.3).
    void UpdateStatus() {
        const uint64_t now = Now();
        const LinkStatus s = Status();
        const StatusView v = DescribeStatus(s, model->cameraRegistered());
        if (!viewShown) {
            view = v;
            viewShown = true;
        } else if (v.headline != view.headline || v.icon != view.icon || v.facts != view.facts || v.hint != view.hint) {
            prevView = view;
            statusFade.Start(1, now, Dur(200), 0);
            if (v.icon != view.icon) badgePop.Start(1, now, Dur(250), 0);
            view = v;
        }
        const bool live = s.state == LinkState::Streaming && !s.lockPaused;
        if (live && !wasLive && Dur(600) > 0) liveGlow.Start(1, now, 600, 0);
        wasLive = live;
        if (s.pairingCode != pairingCode) {
            pairingCode = s.pairingCode;
            pairingSince = pairingCode.empty() ? 0 : now;
        }
        Applying(s, now);
    }

    // --- Layout --------------------------------------------------------------------------------------

    float lineH = 13;
    Box curClip;
    float curAlpha = 1;
    bool curVisible = true;

    Item& Add(int id, Kind kind, const std::wstring& label, const Box& rect, int parent) {
        Item it;
        it.e.id = id;
        it.e.kind = kind;
        int key = -1;
        it.label = StripAccessKey(label, &key);
        it.e.name = it.label;
        it.e.accessKeyIndex = key;
        it.e.rect = rect;
        it.e.clip = curClip;
        it.e.parent = parent;
        it.e.visible = curVisible;
        it.e.focusable = kind == Kind::Button || kind == Kind::Checkbox || kind == Kind::Radio || kind == Kind::Link;
        it.alpha = curAlpha;
        items.push_back(std::move(it));
        return items.back();
    }

    Size RadioSize(const std::wstring& label) {
        int k;
        const Size t = paint.Measure(Font::Body, StripAccessKey(label, &k));
        return {13 + 5 + t.w + 2, std::max(t.h, 15.f)};
    }
    Size CheckSize(const std::wstring& label, float maxW) {
        int k;
        const Size t = paint.Measure(Font::Body, StripAccessKey(label, &k), maxW - 18);
        return {18 + t.w + 2, std::max(t.h, 15.f)};
    }
    Size LinkSize(const std::wstring& label) {
        const Size t = paint.Measure(Font::Body, label);
        return {t.w + 2, std::max(t.h, 15.f)};
    }

    void AddRadios(const std::vector<std::pair<int, std::wstring>>& options, int set, int parent, float x, float* y, float w,
                   const LinkStatus& s, int selectedId) {
        std::vector<Size> sizes;
        for (const auto& o : options) sizes.push_back(RadioSize(o.second));
        float bottom = *y;
        const std::vector<Box> boxes = Flow(sizes, x, *y, w, 14, 4, &bottom);
        for (size_t i = 0; i < options.size(); ++i) {
            Item& it = Add(options[i].first, Kind::Radio, options[i].second, boxes[i], parent);
            it.e.radioSet = set;
            it.e.selected = options[i].first == selectedId;
            it.e.enabled = Enabled(options[i].first, s);
        }
        *y = bottom;
    }

    void AddCheck(int id, const std::wstring& label, bool checked, int parent, float x, float* y, float w, bool enabled) {
        const Size sz = CheckSize(label, w);
        Item& it = Add(id, Kind::Checkbox, label, MakeBox(x, *y, sz.w, sz.h), parent);
        it.e.checked = checked;
        it.e.enabled = enabled;
        *y += sz.h;
    }

    void AddLink(int id, const std::wstring& label, int parent, float x, float* y) {
        const Size sz = LinkSize(label);
        Add(id, Kind::Link, label, MakeBox(x, *y, sz.w, sz.h), parent);
        *y += sz.h;
    }

    void Deco_(int parent, const Box& b, const std::wstring& text, bool subtle, bool dim = false) {
        decos.push_back({parent, b, text, subtle, dim});
    }

    // The "60 fps works at 720p on this camera" line (capability-driven, per quality).
    std::wstring FpsReason(const LinkStatus& s) const {
        const proto::CameraInfo& c = s.camera;
        if (!caps::Known(Connected(s), c)) return L"";
        static const wchar_t* names[3] = {L"720p", L"1080p", L"4K"};
        auto where = [&](uint8_t bit) {
            std::wstring out;
            for (uint8_t q = 0; q < 3; ++q) {
                if (q == c.quality || !(proto::FpsMask(c, q) & bit)) continue;
                if (q == proto::kQuality4K && !(c.flags & proto::kCamHas4K)) continue;
                out += (out.empty() ? L"" : L" and ") + std::wstring(names[q]);
            }
            return out;
        };
        const uint8_t mask = proto::FpsMask(c, c.quality);
        if (!(mask & proto::kFps60Bit)) {
            const std::wstring q = where(proto::kFps60Bit);
            if (!q.empty()) return L"60 fps works at " + q + L" on this camera.";
        }
        if (!(mask & proto::kFps120Bit)) {
            const std::wstring q = where(proto::kFps120Bit);
            if (!q.empty()) return L"120 fps works at " + q + L" on this camera.";
        }
        if (!(mask & (proto::kFps60Bit | proto::kFps120Bit)) && where(proto::kFps60Bit | proto::kFps120Bit).empty())
            return L"This camera does 30 fps.";
        return L"";
    }

    void Layout() {
        const uint64_t now = Now();
        const LinkStatus s = Status();
        const proto::CameraInfo& c = s.camera;
        const bool connected = Connected(s);
        const bool known = caps::Known(connected, c);
        items.clear();
        decos.clear();
        groups.clear();
        lineH = paint.LineHeight(Font::Body);

        const float border = xp::kBorder, top = xp::kTitleBarH;
        pane = {border, top, border + kPaneW, kH - border};
        paneView = pane;
        right = {pane.r, top, kW - border, kH - border};

        // Caption buttons.
        curClip = {0, 0, kW, kH};
        curAlpha = 1;
        curVisible = true;
        Add(kCaptionMin, Kind::CaptionButton, L"Minimize", xp::MinimizeButtonBox(kW), kNone);
        Add(kCaptionClose, Kind::CaptionButton, L"Close", xp::CloseButtonBox(kW), kNone);

        // --- Task pane -----------------------------------------------------------------------------
        const float gx = pane.l + kPad, gw = kPaneW - 2 * kPad, ix = gx + kBodyPad, iw = gw - 2 * kBodyPad;
        float y = pane.t + kPad - scroll;
        for (size_t gi = 0; gi < std::size(kGroups); ++gi) {
            const GroupDef& def = kGroups[gi];
            const bool expandable = def.bit != 0;
            const float open = expandable ? groupOpen[gi].Value(now) : 1.f;
            GroupGeo g{def.id, def.title, def.hero, expandable, MakeBox(gx, y, gw, kHeaderH), {}, open};
            curClip = paneView;
            curAlpha = 1;
            curVisible = true;
            {
                Item& header = Add(def.id, Kind::Group, def.title, g.header, kNone);
                header.e.expandable = expandable;
                header.e.expanded = !expandable || (openBits & def.bit);
                header.e.focusable = expandable;
            }
            const float bodyTop = y + kHeaderH;
            float cy = bodyTop + kBodyPad;
            // Children are laid out fully open, then clipped to the animated body.
            const size_t firstChild = items.size(), firstDeco = decos.size();
            curVisible = open >= 0.999f;
            curAlpha = open;
            switch (def.id) {
            case kGroupNow: LayoutNow(s, now, ix, &cy, iw, g); break;
            case kGroupCamera:
                AddRadios({{kBack, L"&Back camera"}, {kFront, L"&Front camera"}}, kSetFacing, kGroupCamera, ix, &cy, iw, s,
                          s.facing == proto::kFacingFront ? kFront : kBack);
                break;
            case kGroupVideo: {
                Deco_(kGroupVideo, MakeBox(ix, cy, iw, lineH), L"Quality", true);
                cy += lineH + 4;
                const int q = !c.valid ? kNone : c.quality == proto::kQuality720p ? kQ720 : c.quality == proto::kQuality4K ? kQ4K : kQ1080;
                AddRadios({{kQ720, L"720p"}, {kQ1080, L"1080p"}, {kQ4K, L"4K"}}, kSetQuality, kGroupVideo, ix, &cy, iw, s, q);
                cy += 8;
                Deco_(kGroupVideo, MakeBox(ix, cy, iw, lineH), L"Frame rate", true);
                cy += lineH + 4;
                const int fps = caps::EffectiveFps(c);
                const int f = !c.valid ? kNone : fps == 120 ? kFps120 : fps == 60 ? kFps60 : kFps30;
                AddRadios({{kFps30, L"30 fps"}, {kFps60, L"60 fps"}, {kFps120, L"120 fps"}}, kSetFps, kGroupVideo, ix, &cy, iw, s, f);
                const std::wstring reason = FpsReason(s);
                if (!reason.empty()) {
                    cy += 6;
                    const float h = paint.Measure(Font::Body, reason, iw).h;
                    Deco_(kGroupVideo, MakeBox(ix, cy, iw, h), reason, true);
                    cy += h;
                }
                break;
            }
            case kGroupPicture:
                AddCheck(kMirror, L"&Mirror the image", model->mirror(), kGroupPicture, ix, &cy, iw, true);
                cy += 6;
                AddCheck(kFill, L"Fill the frame (crop instead of black bars)", model->fill(), kGroupPicture, ix, &cy, iw, true);
                break;
            case kGroupWifi:
                AddCheck(kAutostart, L"Start MyCam with Windows", model->autostart(), kGroupWifi, ix, &cy, iw, true);
                cy += 6;
                AddCheck(kWireless, L"Find phones on Wi-Fi (beta)", model->wireless(), kGroupWifi, ix, &cy, iw, true);
                cy += 8;
                AddLink(kForgetPhones, L"Forget Wi-Fi phones", kGroupWifi, ix, &cy);
                items.back().e.name = L"Forget Wi-Fi phones (they pair again with a code)";
                break;
            case kGroupTasks:
                AddLink(kReconnect, L"Reconnect phone", kGroupTasks, ix, &cy);
                cy += 6;
                AddLink(kOpenLog, L"Open log folder", kGroupTasks, ix, &cy);
                break;
            }
            cy += kBodyPad;
            const float bodyH = cy - bodyTop;
            g.body = MakeBox(gx, bodyTop, gw, bodyH * open);
            const Box clip = {std::max(g.body.l, paneView.l), std::max(g.body.t, paneView.t), std::min(g.body.r, paneView.r),
                              std::min(g.body.b, paneView.b)};
            for (size_t k = firstChild; k < items.size(); ++k) items[k].e.clip = clip;
            (void)firstDeco;
            groups.push_back(g);
            y = bodyTop + bodyH * open + kPad;
        }
        contentH = y + scroll - pane.t;
        curClip = {0, 0, kW, kH};
        curAlpha = 1;
        curVisible = true;

        LayoutRight(s, c, known);

        // Default focus: the first tab stop.
        std::vector<Element> list = Elements();
        const Element* f = FindElement(list, focus);
        if (!f || !CanFocus(*f)) focus = NextTabStop(list, kNone, false);

        // Tell a UIA provider when anything changed.
        size_t sig = items.size();
        for (const auto& it : items) {
            sig = sig * 131 + size_t(it.e.id) * 7 + (it.e.enabled ? 1 : 0) + (it.e.checked ? 2 : 0) + (it.e.selected ? 4 : 0) +
                  (it.e.expanded ? 8 : 0) + (it.e.visible ? 16 : 0) + std::hash<std::wstring>()(it.e.name);
        }
        if (sig != lastSignature) {
            lastSignature = sig;
            if (owner->elementsChanged_) owner->elementsChanged_();
        }
    }

    void LayoutNow(const LinkStatus& s, uint64_t now, float x, float* y, float w, GroupGeo& g) {
        // The LIVE pill sits in the hero header, right-aligned.
        if (s.state == LinkState::Streaming && !s.lockPaused) {
            const Size t = paint.Measure(Font::BodyBold, L"LIVE");
            const float pw2 = std::max(38.f, t.w + 16), ph2 = std::min(kHeaderH - 6, std::max(16.f, t.h + 3));
            Item& pill = Add(kLivePill, Kind::Text, L"LIVE", MakeBox(g.header.r - 8 - pw2, g.header.t + (kHeaderH - ph2) / 2, pw2, ph2), kGroupNow);
            pill.e.name = L"Live";
            pill.e.clip = paneView;
        }
        const float iconW = 32, tx = x + iconW + 8, tw = w - iconW - 8;
        // At most two lines: a long phone name ("Pairing with …") gets an ellipsis.
        std::wstring shown = view.headline;
        const float maxH = paint.LineHeight(Font::Instruction) * 2 + 1;
        while (shown.size() > 4 && paint.Measure(Font::Instruction, shown, tw).h > maxH) {
            const bool dots = shown.back() == L'…';
            shown.erase(shown.size() - (dots ? 2 : 1));
            while (!shown.empty() && shown.back() == L' ') shown.pop_back();
            shown += L"…";
        }
        const Size head = paint.Measure(Font::Instruction, shown, tw);
        {
            Item& h = Add(kStatusHeadline, Kind::Text, L"", MakeBox(tx, *y, tw, head.h), kGroupNow);
            h.e.name = view.headline;
            h.label = shown;
            h.e.liveRegion = true;
        }
        float ty = *y + head.h + 2;
        if (!view.facts.empty()) {
            const Size f = paint.Measure(Font::Body, view.facts, tw);
            Item& d = Add(kStatusDetail, Kind::Text, L"", MakeBox(tx, ty, tw, f.h), kGroupNow);
            d.e.name = d.label = view.facts;
            ty += f.h;
        }
        if (!s.pairingCode.empty() && model->showPairing) {
            ty += 4;
            const Size l = LinkSize(L"Show the pairing code");
            Add(kShowPairing, Kind::Link, L"Show the pairing code", MakeBox(tx, ty, l.w, l.h), kGroupNow);
            ty += l.h;
        }
        *y = std::max(*y + iconW, ty) + 10;

        const bool applying = Applying(s, now);
        if (view.progress != StatusProgress::None || applying) {
            const bool pairing = view.progress == StatusProgress::Pairing;
            const float barH = 13;
            Item& bar = Add(kStatusProgress, Kind::ProgressBar, L"", MakeBox(x, *y, w, barH), kGroupNow);
            if (pairing) {
                const uint64_t elapsed = pairingSince ? now - pairingSince : 0;
                const uint64_t left = elapsed >= kPairingAnswerMs ? 0 : kPairingAnswerMs - elapsed;
                wchar_t t[8];
                FormatCountdown(left, t, 8);
                const float tw2 = paint.Measure(Font::Body, L"0:00").w + 6;
                bar.e.rect = MakeBox(x, *y, w - tw2, barH);
                bar.e.value = float(left) / float(kPairingAnswerMs);
                bar.label = t;
                bar.e.name = std::wstring(L"Time left to pair: ") + t;
            } else {
                bar.e.value = -1;
                bar.e.name = applying ? L"Starting the camera" : L"Working";
            }
            *y += barH + 10;
        }
        const LinkStatus& st = s;
        const bool paused = model->paused();
        Item& pause = Add(kPause, Kind::Button, paused ? L"&Resume the camera" : L"&Pause the camera", MakeBox(x, *y, w, 32), kGroupNow);
        pause.e.enabled = PauseEnabled(st);
        *y += 32;
    }

    void LayoutRight(const LinkStatus& s, const proto::CameraInfo& c, bool known) {
        const float outerX = right.l + kRightPad, outerW = right.W() - 2 * kRightPad;
        const float bottom = right.b - kPad;
        // The controls sit in an XP group box ("Camera controls") that fills the rest of the column.
        const float boxPadX = 12, boxTop = lineH / 2;
        const float x = outerX + boxPadX, w = outerW - 2 * boxPadX;

        // Everything under the preview, measured first: the preview takes the space that is left.
        const Size sentence = paint.Measure(Font::Body, view.hint, w);
        const float rowH = std::max(23.f, lineH + 8);
        const float labelW = std::max({paint.Measure(Font::Body, L"Zoom").w, paint.Measure(Font::Body, L"Brightness").w,
                                       paint.Measure(Font::Body, L"Focus").w}) + 10;
        const float btnW = std::max(24.f, paint.Measure(Font::BodyBold, L"+").w + 14);
        const float valueW = std::max(paint.Measure(Font::Body, L"10.0×").w, paint.Measure(Font::Body, L"+2.0 EV").w) + 10;
        const float resetW = paint.Measure(Font::BodyBold, L"1×").w + 18;
        const float zoomW = labelW + btnW + valueW + btnW + 6 + resetW;
        const float evW = labelW + btnW + valueW + btnW;
        const Size autoS = RadioSize(L"Auto"), lockS = RadioSize(L"Lock");
        const float focusW = labelW + autoS.w + 12 + lockS.w;
        const Size torchS = CheckSize(L"Torch (phone light)", w);
        const float autoBtnW = paint.Measure(Font::BodyBold, L"Auto").w + 28;
        const float colGap = 20;
        const bool twoCols = std::max(zoomW, evW) + colGap + std::max(focusW, torchS.w + 10 + autoBtnW) <= w;
        float controlsH;
        if (!known) controlsH = paint.Measure(Font::Body, L"Zoom, brightness, focus and torch appear when an app uses the camera.", w).h;
        else controlsH = twoCols ? 2 * rowH + 6 : 5 * rowH + 4 * 6;
        const float nowH = known ? lineH + 8 : 0;
        const float boxChrome = boxTop + lineH / 2 + 10 + 10; // Title line, padding above and below.
        const float below = 10 + sentence.h + 12 + boxChrome + controlsH + nowH;

        float pvW = outerW, pvH = std::floor(outerW * 9 / 16);
        const float avail = bottom - (right.t + 14) - below;
        if (pvH > avail) {
            pvH = std::max(90.f, avail);
            pvW = std::floor(pvH * 16 / 9);
        }
        previewBox = MakeBox(outerX + (outerW - pvW) / 2, right.t + 14, pvW, pvH);
        {
            Item& p = Add(kPreview, Kind::Image, L"", previewBox, kNone);
            p.e.name = L"Preview: the picture apps receive";
        }
        float y = previewBox.b + 10;
        const Size sentenceOuter = paint.Measure(Font::Body, view.hint, outerW);
        sentenceBox = MakeBox(outerX, y, outerW, sentenceOuter.h);
        {
            Item& t = Add(kStatusSentence, Kind::Text, L"", sentenceBox, kNone);
            t.e.name = t.label = view.hint;
            t.e.liveRegion = true;
        }
        y += sentenceOuter.h + 12;
        controlsBox = Box{outerX, y + boxTop, outerX + outerW, bottom};
        y = controlsBox.t + lineH / 2 + 10;

        if (!known) {
            Item& n = Add(kControlsNote, Kind::Text, L"", MakeBox(x, y, w, controlsH), kNone);
            n.e.name = n.label = L"Zoom, brightness, focus and torch appear when an app uses the camera.";
            return;
        }
        const LinkStatus& st = s;
        auto button = [&](int id, const wchar_t* label, const wchar_t* name, float bx, float by, float bw) {
            Item& b = Add(id, Kind::Button, label, MakeBox(bx, by, bw, rowH), kNone);
            b.e.name = name;
            b.e.enabled = Enabled(id, st);
        };
        auto valueText = [&](int id, const std::wstring& text, const wchar_t* name, float vx, float vy) {
            Item& v = Add(id, Kind::Text, L"", MakeBox(vx, vy, valueW, rowH), kNone);
            v.label = text;
            v.e.name = std::wstring(name) + L" " + text;
        };
        wchar_t buf[32];
        // Zoom row.
        float rx = x, ry = y;
        Deco_(kNone, MakeBox(rx, ry, labelW, rowH), L"Zoom", false);
        button(kZoomOut, L"−", L"Zoom out", rx + labelW, ry, btnW);
        swprintf_s(buf, L"%.1f×", c.zoomX100 / 100.0);
        valueText(kZoomValue, buf, L"Zoom", rx + labelW + btnW, ry);
        button(kZoomIn, L"+", L"Zoom in", rx + labelW + btnW + valueW, ry, btnW);
        button(kZoomReset, L"1×", L"Zoom to 1×", rx + labelW + 2 * btnW + valueW + 6, ry, resetW);
        // Brightness row.
        if (twoCols) ry += rowH + 6;
        else ry += rowH + 6;
        Deco_(kNone, MakeBox(rx, ry, labelW, rowH), L"Brightness", false);
        button(kEvDown, L"−", L"Darker", rx + labelW, ry, btnW);
        const double ev = c.ev * c.evStepX100 / 100.0;
        swprintf_s(buf, L"%s%.1f EV", ev > 0 ? L"+" : L"", ev);
        valueText(kEvValue, c.evMax > c.evMin ? std::wstring(buf) : std::wstring(L"—"), L"Brightness", rx + labelW + btnW, ry);
        button(kEvUp, L"+", L"Brighter", rx + labelW + btnW + valueW, ry, btnW);
        // Focus and torch: a second column, or rows of their own.
        float fx, fy;
        if (twoCols) {
            fx = x + std::max(zoomW, evW) + colGap;
            fy = y;
        } else {
            fx = x;
            fy = ry + rowH + 6;
        }
        Deco_(kNone, MakeBox(fx, fy, labelW, rowH), L"Focus", false);
        const bool locked = c.flags & proto::kCamFocusLocked;
        {
            Item& a = Add(kFocusAuto, Kind::Radio, L"Auto", MakeBox(fx + labelW, fy + (rowH - autoS.h) / 2, autoS.w, autoS.h), kNone);
            a.e.radioSet = kSetFocus;
            a.e.selected = !locked;
            a.e.enabled = Enabled(kFocusAuto, st);
            a.e.name = L"Auto focus";
            Item& l = Add(kFocusLock, Kind::Radio, L"Lock", MakeBox(fx + labelW + autoS.w + 12, fy + (rowH - lockS.h) / 2, lockS.w, lockS.h), kNone);
            l.e.radioSet = kSetFocus;
            l.e.selected = locked;
            l.e.enabled = Enabled(kFocusLock, st);
            l.e.name = L"Lock focus";
        }
        const float ty = twoCols ? fy + rowH + 6 : fy + rowH + 6;
        {
            Item& t = Add(kTorch, Kind::Checkbox, L"Torch (phone light)", MakeBox(fx, ty + (rowH - torchS.h) / 2, torchS.w, torchS.h), kNone);
            t.e.checked = c.flags & proto::kCamTorchOn;
            t.e.enabled = Enabled(kTorch, st);
        }
        const float ax = twoCols ? x + w - autoBtnW : x;
        const float ay = twoCols ? ty : ty + rowH + 6;
        button(kAuto, L"Auto", L"Auto: reset zoom, brightness, focus and torch", ax, ay, autoBtnW);
        y = std::max(ay, ty) + rowH + 6;

        swprintf_s(buf, L"Now: %u×%u at %u fps", c.width, c.height, c.actualFps);
        y = std::max(y, controlsBox.b - 10 - lineH); // Bottom of the group box.
        Item& n = Add(kNowMode, Kind::Text, L"", MakeBox(x, y, w, lineH), kNone);
        n.e.name = n.label = buf;
    }

    std::vector<Element> Elements() const {
        std::vector<Element> out;
        out.reserve(items.size());
        for (const auto& it : items) out.push_back(it.e);
        return out;
    }

    const Item* FindItem(int id) const {
        for (const auto& it : items) if (it.e.id == id) return &it;
        return nullptr;
    }

    float MaxScroll() const { return std::max(0.f, contentH - paneView.H()); }

    Box ThumbTrack() const { return {pane.r - 9, pane.t + 3, pane.r - 3, pane.b - 3}; }
    Box Thumb() const {
        const Box t = ThumbTrack();
        const float h = std::max(28.f, t.H() * paneView.H() / std::max(contentH, 1.f));
        const float max = MaxScroll();
        const float top = t.t + (max > 0 ? (t.H() - h) * scroll / max : 0);
        return {t.l, top, t.r, top + h};
    }

    void SetScroll(float value) {
        const float clamped = std::clamp(value, 0.f, MaxScroll());
        if (clamped == scroll) return;
        scroll = clamped;
        Invalidate();
    }

    void ScrollIntoView(int id) {
        const Item* it = FindItem(id);
        if (!it || !InPane(it->e)) return;
        const Box& r = it->e.rect;
        if (r.t < paneView.t + 4) SetScroll(scroll - (paneView.t + 4 - r.t) - 8);
        else if (r.b > paneView.b - 4) SetScroll(scroll + (r.b - (paneView.b - 4)) + 8);
        Layout();
    }

    int HitTest(float x, float y) const {
        for (const auto& it : items) {
            const Element& e = it.e;
            const bool clickable = e.kind == Kind::Button || e.kind == Kind::Checkbox || e.kind == Kind::Radio ||
                                   e.kind == Kind::Link || e.kind == Kind::CaptionButton || (e.kind == Kind::Group && e.expandable);
            if (!clickable || !e.visible || !e.enabled) continue;
            if (e.rect.Contains(x, y) && e.clip.Contains(x, y)) return e.id;
        }
        return kNone;
    }

    // --- Behaviour -------------------------------------------------------------------------------

    void ToggleGroup(int id) {
        const uint64_t now = Now();
        for (size_t gi = 0; gi < std::size(kGroups); ++gi) {
            if (kGroups[gi].id != id || !kGroups[gi].bit) continue;
            openBits ^= kGroups[gi].bit;
            const bool open = openBits & kGroups[gi].bit;
            groupOpen[gi].Start(open ? 1.f : 0.f, now, Dur(180), groupOpen[gi].Value(now));
            WriteSetting(L"PaneGroups", openBits);
        }
    }

    bool Activate(int id) {
        const LinkStatus s = Status();
        const Item* it = FindItem(id);
        if (it && (!it->e.enabled || !it->e.visible)) return false;
        switch (id) {
        case kPause:
            if (!PauseEnabled(s)) return false;
            model->setPaused(!model->paused());
            break;
        case kBack: model->setFacing(proto::kFacingBack); StartApplying(3, proto::kFacingBack, s); break;
        case kFront: model->setFacing(proto::kFacingFront); StartApplying(3, proto::kFacingFront, s); break;
        case kMirror: model->setMirror(!model->mirror()); break;
        case kFill: model->setFill(!model->fill()); break;
        case kQ720: model->command(proto::kCmdSetQuality, proto::kQuality720p); StartApplying(1, proto::kQuality720p, s); break;
        case kQ1080: model->command(proto::kCmdSetQuality, proto::kQuality1080p); StartApplying(1, proto::kQuality1080p, s); break;
        case kQ4K: model->command(proto::kCmdSetQuality, proto::kQuality4K); StartApplying(1, proto::kQuality4K, s); break;
        case kFps30: model->command(proto::kCmdSetFps, 30); StartApplying(2, 30, s); break;
        case kFps60: model->command(proto::kCmdSetFps, 60); StartApplying(2, 60, s); break;
        case kFps120: model->command(proto::kCmdSetFps, 120); StartApplying(2, 120, s); break;
        case kZoomOut: case kZoomIn: case kZoomReset: {
            const proto::CameraInfo& c = s.camera;
            double z = id == kZoomReset ? 1.0 : c.zoomX100 / 100.0 * (id == kZoomIn ? 1.25 : 0.8);
            z = std::clamp(z, c.zoomMinX100 / 100.0, c.zoomMaxX100 / 100.0);
            model->command(proto::kCmdSetZoom, uint8_t(std::clamp(int(z * 10 + 0.5), 1, 255)));
            break;
        }
        case kEvDown: model->command(proto::kCmdSetExposure, uint8_t(s.camera.ev - 1)); break;
        case kEvUp: model->command(proto::kCmdSetExposure, uint8_t(s.camera.ev + 1)); break;
        case kFocusAuto: model->command(proto::kCmdSetFocus, 0); break;
        case kFocusLock: model->command(proto::kCmdSetFocus, 1); break;
        case kTorch: model->command(proto::kCmdSetTorch, (s.camera.flags & proto::kCamTorchOn) ? 0 : 1); break;
        case kAuto:
            model->command(proto::kCmdSetZoom, 10);
            model->command(proto::kCmdSetExposure, 0);
            model->command(proto::kCmdSetFocus, 0);
            model->command(proto::kCmdSetTorch, 0);
            break;
        case kAutostart: model->setAutostart(!model->autostart()); break;
        case kWireless: model->setWireless(!model->wireless()); break;
        case kReconnect: model->reconnect(); break;
        case kOpenLog: model->openLogFolder(); break;
        case kForgetPhones: if (model->forgetPhones) model->forgetPhones(); break; // They pair again (with a code).
        case kShowPairing: if (model->showPairing) model->showPairing(); break;
        case kCaptionClose: DestroyWindow(hwnd); return true;
        case kCaptionMin: ShowWindow(hwnd, SW_MINIMIZE); return true;
        default:
            if (id >= kGroupNow && id <= kGroupTasks) {
                ToggleGroup(id);
                break;
            }
            return false;
        }
        Invalidate();
        return true;
    }

    void SetFocusTo(int id, bool cues) {
        focus = id;
        if (cues) keyboardCues = true;
        ScrollIntoView(id);
        Invalidate();
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    // --- Timers (only while something moves: no idle CPU) -------------------------------------------

    bool MarqueeRunning() const {
        const Item* bar = FindItem(kStatusProgress);
        return bar && bar->e.value < 0 && !reducedMotion && !paint.HighContrast();
    }

    bool PairingCountdown() const {
        const Item* bar = FindItem(kStatusProgress);
        return bar && bar->e.value >= 0;
    }

    bool Animating(uint64_t now) const {
        bool any = openFade.Running(now) || statusFade.Running(now) || badgePop.Running(now) || liveGlow.Running(now) ||
                   previewFade.Running(now);
        for (const auto& t : groupOpen) any = any || t.Running(now);
        return any;
    }

    void ScheduleTimers() {
        if (!hwnd) return;
        const bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        const uint64_t now = Now();
        if (visible && Animating(now)) SetTimer(hwnd, kTimerAnim, 16, nullptr);
        else if (visible && MarqueeRunning()) SetTimer(hwnd, kTimerAnim, 50, nullptr);
        else if (visible && PairingCountdown()) SetTimer(hwnd, kTimerAnim, 250, nullptr);
        else KillTimer(hwnd, kTimerAnim);
        // The preview polls the shared frame at <= 15 fps while frames are fresh, and checks twice a second
        // for an app starting the camera otherwise.
        if (visible) SetTimer(hwnd, kTimerPreview, previewFresh ? 66 : 500, nullptr);
        else {
            KillTimer(hwnd, kTimerPreview);
            preview.Close();
        }
    }

    void PollPreview() {
        if (!pw || !ph) return;
        const PreviewSource::Result r = preview.Poll(pw, ph, previewPixels);
        const uint64_t now = Now();
        if (r == PreviewSource::Result::NewFrame) {
            previewPending = true;
            if (!previewFresh) {
                previewFresh = true;
                previewFade.Start(1, now, Dur(300), previewFade.Value(now));
            }
            Invalidate();
        } else if (r == PreviewSource::Result::NoFrame && previewFresh) {
            previewFresh = false;
            previewFade.Start(0, now, Dur(300), previewFade.Value(now));
            Invalidate();
        }
    }

    // --- Painting --------------------------------------------------------------------------------

    void EnsureBitmaps(ID2D1DeviceContext* dc) {
        if (iconGen != surface.Generation() || iconScale != scale) {
            appIcon16 = xp::LoadIconBitmap(dc, kIconApp, int(16 * scale + 0.5f));
            statusIcon.Reset();
            statusIconId = 0;
            iconGen = surface.Generation();
            iconScale = scale;
            liveBitmap.Reset();
            artBitmap.Reset();
            artKind = -1;
        }
    }

    ComPtr<ID2D1Bitmap>& IconFor(ID2D1DeviceContext* dc, int icon) {
        // The window draws the plain camera (or the grey one) and pops the state badge on top itself.
        const int base = icon == kIconDisconnected ? kIconDisconnected : kIconApp;
        if (statusIconId != base) {
            statusIcon = xp::LoadIconBitmap(dc, base, int(32 * scale + 0.5f));
            statusIconId = base;
        }
        return statusIcon;
    }

    // 0 paused, 1 waiting (no phone), 2 ready (connected, no app using the camera).
    int WantedArt(const LinkStatus& s) const {
        if (s.lockPaused || s.state == LinkState::Paused) return 0;
        if (s.state == LinkState::Idle || s.state == LinkState::Streaming) return 2;
        return 1;
    }

    void UploadPreview(ID2D1DeviceContext* dc) {
        const LinkStatus s = Status();
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * scale, 96.f * scale);
        if (previewPending && previewPixels.size() == size_t(pw) * ph * 4) {
            D2D1_SIZE_U have = liveBitmap ? liveBitmap->GetPixelSize() : D2D1::SizeU(0, 0);
            if (!liveBitmap || have.width != pw || have.height != ph) {
                liveBitmap.Reset();
                dc->CreateBitmap(D2D1::SizeU(pw, ph), previewPixels.data(), pw * 4, &props, &liveBitmap);
            } else {
                liveBitmap->CopyFromMemory(nullptr, previewPixels.data(), pw * 4);
            }
            previewPending = false;
        }
        const int want = WantedArt(s);
        if (want != artKind || artW != pw || artH != ph || !artBitmap) {
            if (!imagesLoaded) {
                pausedImage = LoadStatusImage(kImagePaused);
                waitingImage = LoadStatusImage(kImageWaiting);
                imagesLoaded = true;
            }
            std::vector<uint8_t> pixels;
            PreviewSource::Render(want == 0 ? pausedImage : waitingImage, pw, ph, pixels);
            artBitmap.Reset();
            if (pixels.size() == size_t(pw) * ph * 4) dc->CreateBitmap(D2D1::SizeU(pw, ph), pixels.data(), pw * 4, &props, &artBitmap);
            artKind = want;
            artW = pw;
            artH = ph;
        }
        if (effectGen != surface.Generation()) {
            blur.Reset();
            artBlur.Reset();
            dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
            dc->CreateEffect(CLSID_D2D1GaussianBlur, &artBlur);
            for (auto* e : {blur.Get(), artBlur.Get()}) {
                if (!e) continue;
                e->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                e->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            }
            effectGen = surface.Generation();
        }
    }

    // A frosted chip over the preview (redesign.md §6.2): the picture under it blurred (σ 8), tinted.
    void Chip(ID2D1DeviceContext* dc, const Box& b, const std::wstring& text, ID2D1Image* source, D2D1_COLOR_F tint) {
        const bool hc = paint.HighContrast();
        const float rad = std::min(b.H() / 2, 11.f);
        if (hc || !source || !blur) {
            const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l, b.t, b.r, b.b), rad, rad};
            dc->FillRoundedRectangle(rr, paint.Brush(hc ? xp::SysColor(COLOR_WINDOW) : D2D1::ColorF(0, 0, 0, 0.6f)));
            if (hc) dc->DrawRoundedRectangle(rr, paint.Brush(xp::SysColor(COLOR_WINDOWTEXT)));
            paint.Text(Font::BodyBold, text, b, hc ? xp::SysColor(COLOR_WINDOWTEXT) : Rgb(0xFFFFFF), Align::Center, -1, false, true);
            return;
        }
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        surfaceFactory()->CreateRoundedRectangleGeometry({D2D1::RectF(b.l, b.t, b.r, b.b), rad, rad}, &shape);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(b.l, b.t, b.r, b.b), shape.Get()), nullptr);
        blur->SetInput(0, source);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 8.f);
        const D2D1_POINT_2F at = D2D1::Point2F(previewBox.l, previewBox.t);
        dc->DrawImage(blur.Get(), &at);
        dc->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), paint.Brush(tint));
        dc->PopLayer();
        dc->DrawRoundedRectangle({D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), rad, rad},
                                 paint.Brush(D2D1::ColorF(1, 1, 1, 0.35f)));
        paint.Text(Font::BodyBold, text, b, Rgb(0xFFFFFF), Align::Center, -1, false, true);
    }

    ID2D1Factory1* surfaceFactory() { return surface.Factory(); }

    void DrawPreview(ID2D1DeviceContext* dc, const LinkStatus& s, uint64_t now) {
        const Box& p = previewBox;
        const D2D1_RECT_F r = D2D1::RectF(p.l, p.t, p.r, p.b);
        dc->FillRectangle(r, paint.Brush(Rgb(0x000000)));
        const float live = previewFade.Value(now, EaseOut);
        ID2D1Image* shown = nullptr;
        if (artBitmap && live < 1) {
            if (artKind == 2 && artBlur && !paint.HighContrast()) { // Connected, no app: the hills, softly blurred.
                artBlur->SetInput(0, artBitmap.Get());
                artBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 6.f);
                const D2D1_POINT_2F at = D2D1::Point2F(p.l, p.t);
                dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->DrawImage(artBlur.Get(), &at);
                dc->PopAxisAlignedClip();
            } else {
                dc->DrawBitmap(artBitmap.Get(), r, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            }
            shown = artBitmap.Get();
        }
        if (liveBitmap && live > 0) {
            dc->DrawBitmap(liveBitmap.Get(), r, live, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            if (live >= 0.5f) shown = liveBitmap.Get();
        }

        // Chips, bottom-left: LIVE, the mode, Mirrored. Only over live video.
        const bool liveVideo = previewFresh && ((s.state == LinkState::Streaming && !s.lockPaused) || model->testPattern);
        if (liveVideo) {
            const float h = std::max(20.f, lineH + 8), gap = 6;
            float x = p.l + 10;
            const float y = p.b - 10 - h;
            auto chip = [&](const std::wstring& text, D2D1_COLOR_F tint) {
                const float w = paint.Measure(Font::BodyBold, text).w + 18;
                Chip(dc, MakeBox(x, y, w, h), text, shown, tint);
                x += w + gap;
            };
            chip(L"LIVE", Rgb(0x2E8B2E, 0.82f));
            const proto::CameraInfo& c = s.camera;
            std::wstring mode;
            if (caps::Known(Connected(s), c)) {
                mode = caps::QualityLabel(std::min(c.width, c.height) >= 700 ? std::min(c.width, c.height) : c.height);
                if (c.actualFps) mode += L" · " + std::to_wstring(c.actualFps);
            } else {
                mode = caps::QualityLabel(std::min(preview.FrameWidth(), preview.FrameHeight()));
            }
            if (!mode.empty()) chip(mode, D2D1::ColorF(0, 0, 0, 0.35f));
            if (preview.Mirrored()) chip(L"Mirrored", D2D1::ColorF(0, 0, 0, 0.35f));
        } else if (!previewFresh && artKind == 2) {
            const std::wstring text = L"The preview starts when an app uses the camera.";
            const Size t = paint.Measure(Font::BodyBold, text, p.W() - 40);
            const float h = t.h + 10;
            const Box b = MakeBox(p.l + (p.W() - t.w - 24) / 2, p.t + (p.H() - h) / 2, t.w + 24, h);
            Chip(dc, b, text, artBitmap.Get(), D2D1::ColorF(0, 0, 0, 0.35f));
        }
        paint.SunkenFrame(p.Inset(-1, -1));
    }

    ButtonState StateOf(const Item& it) const {
        ButtonState st;
        st.disabled = !it.e.enabled;
        st.hot = hot == it.e.id && !st.disabled && (pressed == kNone || pressed == it.e.id);
        st.pressed = pressed == it.e.id && hot == it.e.id;
        return st;
    }

    bool Cues() const { return keyboardCues || xp::KeyboardCuesAlways(); }

    void DrawItem(ID2D1DeviceContext* dc, const Item& it, const LinkStatus& s, uint64_t now) {
        const Element& e = it.e;
        const xp::Palette& pal = paint.Colors();
        const ButtonState st = StateOf(it);
        const int underline = Cues() ? e.accessKeyIndex : -1;
        switch (e.kind) {
        case Kind::CaptionButton:
            paint.CaptionButton(e.rect, e.id == kCaptionClose, st);
            break;
        case Kind::Button:
            if (e.id == kPause) DrawPauseButton(dc, it, st, underline);
            else paint.PushButton(e.rect, it.label, underline, st, xp::ButtonStyle::Normal,
                                  it.label == L"−" || it.label == L"+" ? Font::BodyBold : Font::Body);
            break;
        case Kind::Checkbox:
        case Kind::Radio: {
            const Box box = MakeBox(e.rect.l, e.rect.t + std::max(0.f, (std::min(e.rect.H(), lineH + 2) - 13) / 2), 13, 13);
            if (e.kind == Kind::Checkbox) paint.Checkbox(box, e.checked, st);
            else paint.Radio(box, e.selected, st);
            paint.Text(Font::Body, it.label, Box{e.rect.l + 18, e.rect.t, e.rect.r + 2, e.rect.b},
                       e.enabled ? pal.text : pal.disabledText, Align::Leading, underline);
            break;
        }
        case Kind::Link: {
            const bool isHot = hot == e.id;
            const D2D1_COLOR_F ink = isHot ? pal.linkHot : pal.link;
            paint.Text(Font::Body, it.label, e.rect, ink);
            if (isHot) {
                const float w = paint.Measure(Font::Body, it.label).w;
                paint.Line(e.rect.l, e.rect.t + lineH - 0.5f, e.rect.l + w, e.rect.t + lineH - 0.5f, ink);
            }
            break;
        }
        case Kind::ProgressBar:
            if (e.value >= 0) {
                paint.ProgressChunks(e.rect, DeterminateChunks(e.value, e.rect.W() - 6, xp::Painter::kChunk, xp::Painter::kChunkGap));
                paint.Text(Font::Body, it.label, Box{e.rect.r + 6, e.rect.t - 2, e.rect.r + 60, e.rect.b + 2}, pal.subtle,
                           Align::Leading, -1, false, true);
            } else if (reducedMotion || paint.HighContrast()) {
                paint.Text(Font::Body, L"Working…", e.rect.Inset(0, -2), pal.subtle, Align::Leading, -1, false, true);
            } else {
                paint.ProgressMarquee(e.rect, MarqueeOffset(now, e.rect.W() - 6, xp::Painter::kChunk, xp::Painter::kChunkGap));
            }
            break;
        case Kind::Text:
            DrawText(dc, it, s, now);
            break;
        default:
            break;
        }
    }

    void DrawPauseButton(ID2D1DeviceContext* dc, const Item& it, const ButtonState& st, int underline) {
        const bool resume = model->paused();
        paint.PushButton(it.e.rect, L"", -1, st, resume ? xp::ButtonStyle::Green : xp::ButtonStyle::Normal);
        const float shift = st.pressed ? 1.f : 0.f;
        const float tw = paint.Measure(Font::BodyBold, it.label).w;
        const float iconW = 14, total = iconW + 8 + tw;
        const float x = (it.e.rect.l + it.e.rect.r - total) / 2 + shift, cy = (it.e.rect.t + it.e.rect.b) / 2 + shift;
        const xp::Palette& pal = paint.Colors();
        const D2D1_COLOR_F ink = st.disabled ? pal.disabledText : resume && !paint.HighContrast() ? Rgb(0xFFFFFF) : pal.text;
        if (resume) { // Play triangle.
            ComPtr<ID2D1PathGeometry> tri;
            surfaceFactory()->CreatePathGeometry(&tri);
            ComPtr<ID2D1GeometrySink> sink;
            if (tri && SUCCEEDED(tri->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(x + 2, cy - 6), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x + 13, cy));
                sink->AddLine(D2D1::Point2F(x + 2, cy + 6));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tri.Get(), paint.Brush(ink));
            }
        } else { // Two amber bars (the Pause badge colour).
            const D2D1_COLOR_F bar = st.disabled ? pal.disabledText : paint.HighContrast() ? pal.text : Rgb(0xE08A00);
            paint.Fill(Box{x + 2, cy - 6, x + 6, cy + 6}, bar);
            paint.Fill(Box{x + 9, cy - 6, x + 13, cy + 6}, bar);
        }
        paint.Text(Font::BodyBold, it.label, Box{x + iconW + 8, it.e.rect.t + shift, it.e.rect.r, it.e.rect.b + shift}, ink,
                   Align::Leading, underline, false, true);
    }

    void DrawText(ID2D1DeviceContext* dc, const Item& it, const LinkStatus& s, uint64_t now) {
        const Element& e = it.e;
        const xp::Palette& pal = paint.Colors();
        (void)s;
        switch (e.id) {
        case kLivePill: {
            const float g = liveGlow.Running(now) ? liveGlow.Value(now, EaseInOut) : -1.f;
            paint.LivePill(e.rect, g);
            break;
        }
        case kStatusHeadline: case kStatusDetail: {
            // Crossfade from the previous status (200 ms).
            const float t = statusFade.Value(now, EaseOut);
            const Font font = e.id == kStatusHeadline ? Font::Instruction : Font::Body;
            D2D1_COLOR_F ink = e.id == kStatusHeadline ? (paint.HighContrast() ? pal.text : Rgb(0x1D3F8A)) : pal.subtle;
            if (t < 1) {
                const std::wstring& old = e.id == kStatusHeadline ? prevView.headline : prevView.facts;
                D2D1_COLOR_F faded = ink;
                faded.a = 1 - t;
                paint.Text(font, old, Box{e.rect.l, e.rect.t, e.rect.r, e.rect.t + 200}, faded);
                ink.a = t;
            }
            paint.Text(font, it.label, Box{e.rect.l, e.rect.t, e.rect.r, e.rect.t + 200}, ink);
            break;
        }
        case kZoomValue: case kEvValue:
            paint.Text(Font::Body, it.label, e.rect, pal.text, Align::Center, -1, false, true);
            break;
        case kStatusSentence: case kControlsNote: case kNowMode:
            paint.Text(Font::Body, it.label, e.rect, pal.subtle);
            break;
        default:
            break;
        }
        (void)dc;
    }

    void DrawStatusIcon(ID2D1DeviceContext* dc, const Box& at, uint64_t now) {
        auto& icon = IconFor(dc, view.icon);
        const float t = statusFade.Value(now, EaseOut);
        if (icon) dc->DrawBitmap(icon.Get(), D2D1::RectF(at.l, at.t, at.r, at.b), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        // The state badge pops in (scale 0.6 -> 1 with a slight overshoot, 250 ms).
        const float pop = badgePop.Running(now) ? Lerp(0.6f, 1.f, BackOut(badgePop.Progress(now))) : 1.f;
        paint.Badge(at.r - 5, at.b - 5, 6.5f, BadgeFor(view.icon), pop * std::max(t, 0.6f));
    }

    void DrawGroups(ID2D1DeviceContext* dc, const LinkStatus& s, uint64_t now) {
        const xp::Palette& pal = paint.Colors();
        dc->PushAxisAlignedClip(D2D1::RectF(paneView.l, paneView.t, paneView.r, paneView.b), D2D1_ANTIALIAS_MODE_ALIASED);
        for (const GroupGeo& g : groups) {
            if (g.header.b < paneView.t - 40 && g.body.b < paneView.t) continue;
            const Item* header = FindItem(g.id);
            const bool headerHot = hot == g.id;
            if (g.open > 0.001f) paint.GroupBody(g.body, g.hero);
            paint.GroupHeader(g.header, g.title, g.hero, g.expandable ? xp::Chevron::Up : xp::Chevron::None, 1 - g.open, headerHot);
            (void)header;
            if (g.id == kGroupNow) if (const Item* pill = FindItem(kLivePill)) DrawItem(dc, *pill, s, now);
            if (g.open <= 0.001f) continue;

            const Box clip = {g.body.l, g.body.t, g.body.r, g.body.b};
            dc->PushAxisAlignedClip(D2D1::RectF(clip.l, clip.t, clip.r, clip.b), D2D1_ANTIALIAS_MODE_ALIASED);
            const bool fading = g.open < 0.999f;
            if (fading) {
                D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
                lp.opacity = g.open;
                dc->PushLayer(lp, nullptr);
            }
            if (g.id == kGroupNow) {
                const Item* head = FindItem(kStatusHeadline);
                if (head) DrawStatusIcon(dc, MakeBox(head->e.rect.l - 40, head->e.rect.t + 2, 32, 32), now);
            }
            for (const Deco& d : decos) {
                if (d.parent == g.id) paint.Text(Font::Body, d.text, d.box, d.dim ? pal.disabledText : d.subtle ? pal.subtle : pal.text);
            }
            for (const Item& it : items) {
                if (it.e.parent == g.id && it.e.id != kLivePill) DrawItem(dc, it, s, now);
            }
            if (fading) dc->PopLayer();
            dc->PopAxisAlignedClip();
        }
        // Thin XP-style scrollbar when the groups overflow.
        if (MaxScroll() > 0) {
            const Box track = ThumbTrack(), thumb = Thumb();
            if (paint.HighContrast()) {
                paint.Fill(track, xp::SysColor(COLOR_SCROLLBAR));
                paint.Fill(thumb, xp::SysColor(thumbHot || thumbDrag ? COLOR_HIGHLIGHT : COLOR_BTNTEXT));
            } else {
                dc->FillRoundedRectangle({D2D1::RectF(track.l, track.t, track.r, track.b), 3, 3}, paint.Brush(Rgb(0xFFFFFF, 0.18f)));
                const D2D1_ROUNDED_RECT rr = {D2D1::RectF(thumb.l, thumb.t, thumb.r, thumb.b), 3, 3};
                auto fill = paint.Gradient(thumb.l, 0, thumb.r, 0, thumbHot || thumbDrag
                                                                      ? std::initializer_list<xp::Stop>{{0.f, 0xE6EEFF}, {1.f, 0xB8CCF7}}
                                                                      : std::initializer_list<xp::Stop>{{0.f, 0xD4E0FB}, {1.f, 0xA9BFF2}});
                if (fill) dc->FillRoundedRectangle(rr, fill.Get());
                dc->DrawRoundedRectangle(rr, paint.Brush(Rgb(0xFFFFFF, 0.8f)));
            }
        }
        dc->PopAxisAlignedClip();
    }

    // XP group box: a rounded etched frame with the title in XP's group-box blue on the top edge.
    void DrawGroupBox(const std::wstring& title, const Box& b) {
        const xp::Palette& pal = paint.Colors();
        const Size t = paint.Measure(Font::Body, title);
        const D2D1_ROUNDED_RECT rr = {D2D1::RectF(b.l + 0.5f, b.t + 0.5f, b.r - 0.5f, b.b - 0.5f), 3, 3};
        paint.Dc()->DrawRoundedRectangle(rr, paint.Brush(pal.highContrast ? pal.text : Rgb(0xD0D0BF)));
        const Box label = MakeBox(b.l + 8, b.t - t.h / 2, t.w + 6, t.h);
        paint.Fill(label, pal.surface);
        paint.Text(Font::Body, title, label.Offset(3, 0), pal.highContrast ? pal.text : Rgb(0x0046D5));
    }

    void DrawFocus() {
        if (!Cues() || GetFocus() != hwnd) return;
        const Item* it = FindItem(focus);
        if (!it || !CanFocus(it->e)) return;
        const Element& e = it->e;
        Box r;
        switch (e.kind) {
        case Kind::Checkbox: case Kind::Radio: {
            const Size t = paint.Measure(Font::Body, it->label, e.rect.W() - 18);
            r = Box{e.rect.l + 16, e.rect.t - 1, e.rect.l + 18 + t.w + 3, e.rect.t + t.h + 1};
            break;
        }
        case Kind::Link: r = e.rect.Inset(-2, -1); break;
        case Kind::Group: r = Box{e.rect.l + 8, e.rect.t + 4, e.rect.r - 26, e.rect.b - 4}; break;
        default: r = e.rect.Inset(3, 3); break;
        }
        const bool clip = InPane(e);
        if (clip) paint.Dc()->PushAxisAlignedClip(D2D1::RectF(paneView.l, paneView.t, paneView.r, paneView.b), D2D1_ANTIALIAS_MODE_ALIASED);
        paint.FocusRect(r);
        if (clip) paint.Dc()->PopAxisAlignedClip();
    }

    void Paint() {
        const uint64_t now = Now();
        ID2D1DeviceContext* dc = surface.Begin(scale);
        if (!dc) return;
        paint.BeginFrame(dc, surface.Factory());
        Layout();
        const float maxScroll = MaxScroll();
        if (scroll > maxScroll) { // Content shrank (a group closed): pull it back down.
            scroll = maxScroll;
            Layout();
        }
        EnsureBitmaps(dc);
        pw = UINT(previewBox.W() * scale) & ~1u;
        ph = UINT(previewBox.H() * scale) & ~1u;
        UploadPreview(dc);
        const LinkStatus s = Status();

        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // Window open: fade in from 96 % scale (150 ms).
        const bool opening = openFade.Running(now);
        if (opening) {
            const float t = openFade.Value(now, EaseOut);
            const float k = Lerp(0.96f, 1.f, t);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(kW / 2, kH / 2)));
            D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1();
            lp.opacity = t;
            dc->PushLayer(lp, nullptr);
        }

        xp::WindowChrome(paint, kW, kH, L"MyCam", active, appIcon16.Get());
        const xp::Palette& pal = paint.Colors();
        // Task pane (blue gradient) and the beige surface.
        if (pal.highContrast) paint.Fill(pane, pal.paneTop);
        else paint.VGradient(pane, {{0.f, 0x7BA2E7}, {1.f, 0x6375D6}});
        paint.Fill(right, pal.surface);
        if (!pal.highContrast) paint.Line(right.l + 0.5f, right.t, right.l + 0.5f, right.b, Rgb(0x5A6FC9));

        DrawGroups(dc, s, now);
        DrawPreview(dc, s, now);
        DrawGroupBox(L"Camera controls", controlsBox);
        for (const Deco& d : decos) {
            if (d.parent == kNone) paint.Text(Font::Body, d.text, d.box, pal.text, Align::Leading, -1, false, true);
        }
        for (const Item& it : items) {
            if (it.e.parent == kNone && it.e.kind != Kind::Group) DrawItem(dc, it, s, now);
        }
        DrawFocus();

        if (opening) dc->PopLayer();
        surface.End();
        ScheduleTimers();
    }

    // --- Window procedure --------------------------------------------------------------------------

    void ToDips(LPARAM lp, float* x, float* y) const {
        *x = GET_X_LPARAM(lp) / scale;
        *y = GET_Y_LPARAM(lp) / scale;
    }

    void ReadSystemSettings() {
        reducedMotion = xp::ReducedMotion();
        paint.SetPalette(xp::HighContrastOn());
        paint.SetTextScale(xp::TextScaleFactor());
    }

    bool HandleKey(WPARAM key) {
        std::vector<Element> list = Elements();
        switch (key) {
        case VK_TAB:
            SetFocusTo(NextTabStop(list, focus, GetKeyState(VK_SHIFT) < 0), true);
            return true;
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: {
            const int delta = key == VK_LEFT || key == VK_UP ? -1 : 1;
            const int target = ArrowTarget(list, focus, delta);
            const Element* t = FindElement(list, target);
            SetFocusTo(target, true);
            if (t && t->kind == Kind::Radio && !t->selected) Activate(target); // Arrows select within a radio group.
            return true;
        }
        case VK_SPACE: case VK_RETURN: {
            keyboardCues = true;
            const Element* f = FindElement(list, focus);
            if (f && (key == VK_SPACE || f->kind != Kind::Checkbox)) Activate(focus);
            return true;
        }
        case VK_PRIOR: SetScroll(scroll - paneView.H() * 0.8f); return true;
        case VK_NEXT: SetScroll(scroll + paneView.H() * 0.8f); return true;
        case VK_ESCAPE: DestroyWindow(hwnd); return true;
        }
        return false;
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
            const int h = HitTest(x, y);
            if (y < xp::kTitleBarH && h != kCaptionClose && h != kCaptionMin) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_NCACTIVATE:
            active = wp != FALSE;
            Invalidate();
            return TRUE; // Nothing non-client to draw.
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
            if (wp == SIZE_MINIMIZED) ScheduleTimers();
            else Invalidate();
            return 0;
        case WM_SHOWWINDOW:
            if (!wp) {
                KillTimer(hwnd, kTimerAnim);
                KillTimer(hwnd, kTimerPreview);
                preview.Close();
            } else {
                Invalidate();
            }
            break;
        case WM_TIMER:
            if (wp == kTimerPreview) PollPreview();
            else Invalidate();
            if (wp == kTimerAnim && !Animating(Now()) && !MarqueeRunning() && !PairingCountdown()) KillTimer(hwnd, kTimerAnim);
            return 0;
        case WM_DPICHANGED: {
            if (forcedScale <= 0) scale = HIWORD(wp) / 96.f;
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE: case WM_THEMECHANGED:
            ReadSystemSettings();
            Invalidate();
            break;
        case WM_SETFOCUS: case WM_KILLFOCUS:
            Invalidate();
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            float x, y;
            ToDips(lp, &x, &y);
            if (thumbDrag) {
                const Box track = ThumbTrack(), thumb = Thumb();
                const float span = track.H() - thumb.H();
                if (span > 0) SetScroll(dragStartScroll + (y - dragStartY) * MaxScroll() / span);
                return 0;
            }
            const int h = HitTest(x, y);
            const bool overThumb = MaxScroll() > 0 && Thumb().Inset(-2, 0).Contains(x, y);
            if (h != hot || overThumb != thumbHot) {
                hot = h;
                thumbHot = overThumb;
                Invalidate();
            }
            const Item* it = FindItem(h);
            SetCursor(LoadCursorW(nullptr, it && it->e.kind == Kind::Link ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            hot = kNone;
            thumbHot = false;
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE; // Set in WM_MOUSEMOVE.
            break;
        case WM_MOUSEWHEEL:
            if (MaxScroll() > 0) SetScroll(scroll - GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA) * 48.f);
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            float x, y;
            ToDips(lp, &x, &y);
            SetFocus(hwnd);
            keyboardCues = false;
            if (MaxScroll() > 0 && ThumbTrack().Inset(-2, 0).Contains(x, y)) {
                const Box thumb = Thumb();
                if (thumb.Inset(-2, 0).Contains(x, y)) {
                    thumbDrag = true;
                    dragStartY = y;
                    dragStartScroll = scroll;
                    SetCapture(hwnd);
                } else {
                    SetScroll(scroll + (y < thumb.t ? -1 : 1) * paneView.H() * 0.8f);
                }
                Invalidate();
                return 0;
            }
            pressed = HitTest(x, y);
            const Item* it = FindItem(pressed);
            if (it && it->e.focusable) focus = pressed;
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            if (thumbDrag) {
                thumbDrag = false;
                Invalidate();
                return 0;
            }
            float x, y;
            ToDips(lp, &x, &y);
            const int target = pressed;
            pressed = kNone;
            Invalidate();
            if (target != kNone && HitTest(x, y) == target) Activate(target);
            return 0;
        }
        case WM_CAPTURECHANGED:
            thumbDrag = false;
            return 0;
        case WM_KEYDOWN:
            if (HandleKey(wp)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wp == VK_MENU || wp == VK_F10) { // Alt shows the access-key underlines.
                if (!keyboardCues) {
                    keyboardCues = true;
                    Invalidate();
                }
                if (wp == VK_MENU) return 0;
            } else if (wp >= 'A' && wp <= 'Z' && (lp & (1 << 29))) {
                const int target = AccessKeyTarget(Elements(), wchar_t(wp));
                if (target != kNone) {
                    sysKeyHandled = true;
                    keyboardCues = true;
                    focus = target;
                    ScrollIntoView(target);
                    Activate(target);
                    Invalidate();
                    return 0;
                }
            }
            break;
        case WM_SYSCHAR:
            if (sysKeyHandled) { // Handled in WM_SYSKEYDOWN: no beep.
                sysKeyHandled = false;
                return 0;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerAnim);
            KillTimer(hwnd, kTimerPreview);
            preview.Close();
            surface.Reset();
            liveBitmap.Reset();
            artBitmap.Reset();
            blur.Reset();
            artBlur.Reset();
            appIcon16.Reset();
            statusIcon.Reset();
            previewFresh = false;
            previewFade.Set(0);
            hwnd = nullptr;
            if (owner->elementsChanged_) owner->elementsChanged_();
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
        if (!dwrite) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
            paint.Init(dwrite.Get(), xp::TextScaleFactor());
            openBits = ReadSetting(L"PaneGroups", kDefaultOpenGroups);
            for (size_t gi = 0; gi < std::size(kGroups); ++gi) groupOpen[gi].Set(!kGroups[gi].bit || (openBits & kGroups[gi].bit) ? 1.f : 0.f);
        }
        ReadSystemSettings();
        // Developer override for checking layouts at other scales without changing Windows' settings:
        // MYCAM_UI_SCALE=1.5 draws the window as at 150 %.
        wchar_t env[16] = {};
        if (GetEnvironmentVariableW(L"MYCAM_UI_SCALE", env, 16)) forcedScale = float(_wtof(env));

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
        scale = forcedScale > 0 ? forcedScale : dpi / 96.f;
        const int w = int(kW * scale + 0.5f), h = int(kH * scale + 0.5f);
        const RECT& wa = mi.rcWork;
        const int x = wa.left + std::max(0, int(wa.right - wa.left - w) / 2);
        const int y = wa.top + std::max(0, int(wa.bottom - wa.top - h) / 2);

        CreateWindowExW(WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"MyCamSettings", L"MyCam",
                        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, nullptr, nullptr, instance, this);
        if (!hwnd) return;
        scale = forcedScale > 0 ? forcedScale : GetDpiForWindow(hwnd) / 96.f;
        surface.Attach(hwnd);

        // XP silhouette: square bottom corners from DWM (the rounded top ones are drawn, transparent
        // outside), no Windows 11 accent border, and keep the DWM drop shadow.
        MARGINS margins = {0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
        const DWORD corners = 1; // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        const COLORREF none = 0xFFFFFFFE; // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

        viewShown = false;
        UpdateStatus();
        statusFade.Set(1);
        badgePop.Set(1);
        openFade.Start(1, Now(), Dur(150), Dur(150) > 0 ? 0.f : 1.f);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
};

SettingsWindow::~SettingsWindow() {
    if (impl_ && impl_->hwnd) DestroyWindow(impl_->hwnd);
    delete impl_;
}

void SettingsWindow::Show() {
    if (!impl_) {
        impl_ = new Impl();
        impl_->owner = this;
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
    if (!impl_ || !impl_->hwnd) return;
    impl_->UpdateStatus();
    impl_->Invalidate();
}

HWND SettingsWindow::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

std::vector<ui::Element> SettingsWindow::Elements() const {
    if (!impl_ || !impl_->hwnd) return {};
    return impl_->Elements();
}

bool SettingsWindow::Invoke(int id) { return impl_ && impl_->hwnd && impl_->Activate(id); }

bool SettingsWindow::Focus(int id) {
    if (!impl_ || !impl_->hwnd) return false;
    const auto list = impl_->Elements();
    const ui::Element* e = ui::FindElement(list, id);
    if (!e || !ui::CanFocus(*e)) return false;
    impl_->SetFocusTo(id, true);
    return true;
}

int SettingsWindow::FocusedId() const { return impl_ && impl_->hwnd ? impl_->focus : ui::kNone; }

RECT SettingsWindow::ClientDipsToScreen(const ui::Box& box) const {
    RECT r = {};
    if (!impl_ || !impl_->hwnd) return r;
    const float s = impl_->scale;
    POINT a = {LONG(std::floor(box.l * s)), LONG(std::floor(box.t * s))};
    POINT b = {LONG(std::ceil(box.r * s)), LONG(std::ceil(box.b * s))};
    ClientToScreen(impl_->hwnd, &a);
    ClientToScreen(impl_->hwnd, &b);
    return {a.x, a.y, b.x, b.y};
}

void SettingsWindow::SetElementsChanged(std::function<void()> callback) { elementsChanged_ = std::move(callback); }

} // namespace mycam
