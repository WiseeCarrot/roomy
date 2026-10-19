#include "ui.h"

#include "clean.h"
#include "format.h"
#include "fs.h"
#include "scan.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace roomy {
namespace {

constexpr UINT WM_APP_CATEGORY = WM_APP + 1;
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 2;
constexpr UINT WM_APP_CLEAN_DONE = WM_APP + 3;
constexpr UINT_PTR kTimer = 1;

constexpr float kFooterH = 76, kRowH = 78, kItemH = 48, kMaxWidth = 1080;

D2D1_COLOR_F rgb(uint32_t hex, float a = 1.f) { return D2D1::ColorF(hex, a); }

D2D1_COLOR_F mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

COLORREF colorref(uint32_t hex) { return RGB((hex >> 16) & 255, (hex >> 8) & 255, hex & 255); }

struct Theme {
    bool dark = false;
    uint32_t bgHex = 0;
    D2D1_COLOR_F bg, card, cardHover, border, divider, footer, dialog;
    D2D1_COLOR_F text, text2, text3, checkBorder, track, meterUsed, scroll;
    D2D1_COLOR_F accent, accentHover, onAccent, accentInk, disabledBg, disabledText;
    D2D1_COLOR_F safe, rebuilds, checkFirst;
};

Theme makeTheme(bool dark) {
    Theme t;
    t.dark = dark;
    if (dark) {
        t.bgHex = 0x1F1F1F;
        t.bg = rgb(0x1F1F1F);
        t.card = rgb(0x2A2A2A);
        t.cardHover = rgb(0x323232);
        t.border = rgb(0x363636);
        t.divider = rgb(0x363636);
        t.footer = rgb(0x242424);
        t.dialog = rgb(0x2C2C2C);
        t.text = rgb(0xF2F2F2);
        t.text2 = rgb(0xB8B8B8);
        t.text3 = rgb(0x8C8C8C);
        t.checkBorder = rgb(0x9A9A9A);
        t.track = rgb(0x3A3A3A);
        t.meterUsed = rgb(0x8A8A8A);
        t.scroll = rgb(0x5C5C5C);
        t.accent = rgb(0x4CC38A);
        t.accentHover = rgb(0x62D29C);
        t.onAccent = rgb(0x0C2619);
        t.accentInk = rgb(0x62D29C);
        t.disabledBg = rgb(0x343434);
        t.disabledText = rgb(0x7A7A7A);
        t.safe = rgb(0x62D29C);
        t.rebuilds = rgb(0x6CB4FF);
        t.checkFirst = rgb(0xF0B54A);
    } else {
        t.bgHex = 0xF3F3F3;
        t.bg = rgb(0xF3F3F3);
        t.card = rgb(0xFFFFFF);
        t.cardHover = rgb(0xF5F5F5);
        t.border = rgb(0xE3E3E3);
        t.divider = rgb(0xECECEC);
        t.footer = rgb(0xFAFAFA);
        t.dialog = rgb(0xFFFFFF);
        t.text = rgb(0x1B1B1B);
        t.text2 = rgb(0x5A5A5A);
        t.text3 = rgb(0x8A8A8A);
        t.checkBorder = rgb(0x8A8A8A);
        t.track = rgb(0xE2E2E2);
        t.meterUsed = rgb(0x9E9E9E);
        t.scroll = rgb(0xC4C4C4);
        t.accent = rgb(0x1E8A5A);
        t.accentHover = rgb(0x239A66);
        t.onAccent = rgb(0xFFFFFF);
        t.accentInk = rgb(0x1A7A4F);
        t.disabledBg = rgb(0xE6E6E6);
        t.disabledText = rgb(0xA0A0A0);
        t.safe = rgb(0x1A7A4F);
        t.rebuilds = rgb(0x1F6FD1);
        t.checkFirst = rgb(0xA35F00);
    }
    return t;
}

bool systemDark() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

// Lets context menus follow dark mode. These uxtheme exports have no names, only numbers,
// but every dark-mode Win32 app uses them and they've been stable since Windows 10 1903.
void allowDarkMenus(bool dark) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));
    if (setMode) setMode(dark ? 2 : 3);  // ForceDark / ForceLight
    if (flush) flush();
}

enum class Hit { None, CatCheck, CatRow, Item, Clean, Rescan, Admin, BannerClose, ConfirmOk, ConfirmCancel, Thumb };

struct HitBox {
    D2D1_RECT_F r{};
    Hit kind = Hit::None;
    int cat = -1, item = -1;
    bool same(const HitBox& o) const { return kind == o.kind && cat == o.cat && item == o.item; }
};

struct Drive {
    wchar_t letter;
    uint64_t total, free;
};

enum class Font { UI, Display, Icon };

struct TextStyle {
    float size;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    bool pathTrim = false;  // ellipsis in the middle, keeping the last folders visible
    bool wrap = false;
    Font font = Font::UI;
};

constexpr auto SEMI = DWRITE_FONT_WEIGHT_SEMI_BOLD;
constexpr auto CENTER = DWRITE_TEXT_ALIGNMENT_CENTER;
constexpr auto RIGHT = DWRITE_TEXT_ALIGNMENT_TRAILING;

class App {
public:
    int run(HINSTANCE inst, int show, const std::vector<std::wstring>& args);

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);

    // drawing
    void createTarget();
    void applyTheme();
    IDWriteTextFormat* format(const TextStyle& s);
    float measure(const std::wstring& s, const TextStyle& st);
    void text(const std::wstring& s, D2D1_RECT_F r, const TextStyle& st, D2D1_COLOR_F c);
    void fill(D2D1_RECT_F r, D2D1_COLOR_F c);
    void fillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c);
    void strokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c);
    void checkbox(float x, float y, int state, bool hot);
    void button(D2D1_RECT_F r, const std::wstring& label, bool primary, bool enabled, Hit kind);
    void addHit(D2D1_RECT_F r, Hit kind, int cat = -1, int item = -1, bool clipToList = true);

    void paint();
    void saveScreenshot();
    std::wstring shown(const std::wstring& path) const;
    void drawHeader();
    void drawMeters(float x, float y, float w);
    void drawList();
    float drawBanner(float x, float y, float w);
    float drawCategory(int ci, float x, float y, float w);
    void drawItem(int ci, int ii, float x, float y, float w);
    float drawScanning(float x, float y, float w);
    float drawEmpty(float x, float y, float w);
    void drawScrollbar();
    void drawFooter();
    void drawConfirm();

    // behaviour
    void startScan();
    void startClean();
    void refreshDrives();
    void click(const HitBox& h);
    void itemMenu(int ci, int ii);
    void reveal(const Item& it);
    void relaunchAsAdmin();
    void ensureTimer();
    void totals(uint64_t& permanent, uint64_t& recycled, bool& bin) const;
    D2D1_COLOR_F tintFg(uint32_t hex) const;
    D2D1_COLOR_F tintBg(uint32_t hex) const;
    bool adminLink() const { return !env.admin && !env.sandbox && phase != Phase::Cleaning; }
    float headerH() const { return adminLink() ? 148.f : 122.f; }
    float maxScroll() const { return std::max(0.f, contentH - (listBottom - listTop)); }
    HitBox hitAt(float x, float y) const;

    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1HwndRenderTarget> hwndRt;
    ComPtr<ID2D1RenderTarget> rt;  // the window, or a bitmap for --screenshot
    ComPtr<ID2D1SolidColorBrush> brush;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats;
    std::wstring uiFont = L"Segoe UI", displayFont = L"Segoe UI", iconFont = L"Segoe MDL2 Assets";
    Theme th;
    float dpi = 96, W = 0, H = 0;

    Env env;
    Progress progress;
    std::thread worker;
    std::mutex pendingMx;
    std::vector<Category> pending;
    CleanResult pendingClean;
    std::vector<Category> cats;
    std::vector<std::wstring> emptyCats;
    enum class Phase { Scanning, Ready, Cleaning } phase = Phase::Scanning;
    ULONGLONG scanStart = 0, scanMs = 0;
    uint64_t scanFiles = 0;
    bool stopped = false;
    CleanResult lastClean;
    bool showBanner = false, confirmOpen = false;
    std::vector<Drive> drives;

    float scroll = 0, scrollTarget = 0, contentH = 0, listTop = 0, listBottom = 0;
    float thumbTravel = 1;  // scroll distance per pixel of thumb drag
    std::vector<HitBox> hits;
    HitBox hover, down;
    bool dragging = false, tracking = false, timerOn = false;
    float dragY = 0, dragScroll = 0;

    // --screenshot <png> [--theme light|dark] [--expand id,id] [--size WxH], for docs and checks
    std::wstring shotPath, shotTheme;
    std::vector<std::wstring> shotExpand;
};

// ------------------------------------------------------------------ resources

void App::createTarget() {
    RECT rc;
    GetClientRect(hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                              dpi, dpi);
    d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(UINT32(rc.right), UINT32(rc.bottom))),
                                &hwndRt);
    rt = hwndRt;
    if (rt) rt->CreateSolidColorBrush(th.text, &brush);
}

void App::applyTheme() {
    th = makeTheme(shotTheme.empty() ? systemDark() : shotTheme == L"dark");
    BOOL dark = th.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF caption = colorref(th.bgHex);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    allowDarkMenus(th.dark);
}

IDWriteTextFormat* App::format(const TextStyle& s) {
    uint64_t key = uint64_t(s.size * 4) | (uint64_t(s.weight / 100) << 12) | (uint64_t(s.align) << 16) |
                   (uint64_t(s.pathTrim) << 20) | (uint64_t(s.wrap) << 21) | (uint64_t(s.font) << 22);
    auto it = formats.find(key);
    if (it != formats.end()) return it->second.Get();
    const std::wstring& family = s.font == Font::Icon ? iconFont : s.font == Font::Display ? displayFont : uiFont;
    ComPtr<IDWriteTextFormat> f;
    dw->CreateTextFormat(family.c_str(), nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size,
                         L"en-us", &f);
    f->SetTextAlignment(s.align);
    f->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(s.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, s.pathTrim ? UINT32(L'\\') : 0u, s.pathTrim ? 2u : 0u};
    ComPtr<IDWriteInlineObject> ellipsis;
    dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
    f->SetTrimming(&trim, ellipsis.Get());
    formats[key] = f;
    return f.Get();
}

float App::measure(const std::wstring& s, const TextStyle& st) {
    ComPtr<IDWriteTextLayout> lay;
    dw->CreateTextLayout(s.c_str(), UINT32(s.size()), format(st), 4000, 100, &lay);
    DWRITE_TEXT_METRICS m{};
    if (lay) lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void App::text(const std::wstring& s, D2D1_RECT_F r, const TextStyle& st, D2D1_COLOR_F c) {
    if (s.empty() || r.right <= r.left) return;
    brush->SetColor(c);
    rt->DrawText(s.c_str(), UINT32(s.size()), format(st), r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::fill(D2D1_RECT_F r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    rt->FillRectangle(r, brush.Get());
}

void App::fillRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    brush->SetColor(c);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush.Get());
}

void App::strokeRound(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    brush->SetColor(c);
    D2D1_RECT_F in{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
    rt->DrawRoundedRectangle(D2D1::RoundedRect(in, rad, rad), brush.Get(), 1.f);
}

void App::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c) {
    brush->SetColor(c);
    rt->DrawLine({x1, y1}, {x2, y2}, brush.Get(), 1.f);
}

void App::checkbox(float x, float y, int state, bool hot) {
    D2D1_RECT_F r{x, y, x + 18, y + 18};
    if (state == 0) {
        fillRound(r, 4, hot ? th.cardHover : th.card);
        strokeRound(r, 4, hot ? th.text2 : th.checkBorder);
        return;
    }
    fillRound(r, 4, hot ? th.accentHover : th.accent);
    if (state == 2) text(L"", r, {11, DWRITE_FONT_WEIGHT_BOLD, CENTER, false, false, Font::Icon}, th.onAccent);
    else fillRound({x + 5, y + 8.2f, x + 13, y + 9.8f}, 0.8f, th.onAccent);
}

void App::button(D2D1_RECT_F r, const std::wstring& label, bool primary, bool enabled, Hit kind) {
    bool hot = enabled && hover.kind == kind;
    if (!enabled) {
        fillRound(r, 6, th.disabledBg);
        text(label, r, {14, SEMI, CENTER}, th.disabledText);
        return;
    }
    if (primary) {
        fillRound(r, 6, hot ? th.accentHover : th.accent);
        text(label, r, {14, SEMI, CENTER}, th.onAccent);
    } else {
        fillRound(r, 6, hot ? th.cardHover : th.card);
        strokeRound(r, 6, th.border);
        text(label, r, {14, SEMI, CENTER}, th.text);
    }
    addHit(r, kind, -1, -1, false);
}

void App::addHit(D2D1_RECT_F r, Hit kind, int cat, int item, bool clipToList) {
    if (clipToList) {
        r.top = std::max(r.top, listTop);
        r.bottom = std::min(r.bottom, listBottom);
        if (r.bottom <= r.top) return;
    }
    hits.push_back({r, kind, cat, item});
}

HitBox App::hitAt(float x, float y) const {
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return *it;
    return {};
}

D2D1_COLOR_F App::tintFg(uint32_t hex) const {
    return th.dark ? mix(rgb(hex), rgb(0xFFFFFF), 0.2f) : mix(rgb(hex), rgb(0x000000), 0.12f);
}

D2D1_COLOR_F App::tintBg(uint32_t hex) const { return rgb(hex, th.dark ? 0.18f : 0.13f); }

void App::totals(uint64_t& permanent, uint64_t& recycled, bool& bin) const {
    permanent = recycled = 0;
    bin = false;
    for (auto& c : cats)
        for (auto& it : c.items) {
            if (!it.selected) continue;
            if (it.action == Action::EmptyRecycleBin) bin = true;
            (it.recycle ? recycled : permanent) += it.bytes;
        }
}

// ------------------------------------------------------------------ painting

void App::paint() {
    if (!rt) createTarget();
    if (!rt) return;
    hits.clear();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(th.bg);
    drawList();
    drawHeader();
    drawFooter();
    if (confirmOpen) {
        hits.clear();  // the dialog is modal
        drawConfirm();
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt.Reset();
        hwndRt.Reset();
        brush.Reset();
    }
    // Keep the scroll position valid as the content grows and shrinks.
    float m = maxScroll();
    if (scrollTarget > m) scrollTarget = m;
    if (scroll > m) scroll = m;
}

// Renders the window at 2x into a PNG. Used for the README images and for checking the layout
// without driving the real window.
void App::saveScreenshot() {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    const float scale = 2;
    UINT pw = UINT(W * scale), ph = UINT(H * scale);
    ComPtr<IWICBitmap> bmp;
    wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ComPtr<ID2D1RenderTarget> target;
    d2d->CreateWicBitmapRenderTarget(
        bmp.Get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                     96 * scale, 96 * scale),
        &target);
    if (!target) return;
    auto saved = rt;
    auto savedBrush = brush;
    rt = target;
    brush.Reset();
    rt->CreateSolidColorBrush(th.text, &brush);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    paint();
    rt = saved;
    brush = savedBrush;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(shotPath.c_str(), GENERIC_WRITE))) return;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(pw, ph);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WriteSource(bmp.Get(), nullptr);
    frame->Commit();
    enc->Commit();
}

// In --home mode, show paths as if the sandbox were a normal profile.
std::wstring App::shown(const std::wstring& path) const {
    if (env.sandbox && fs::startsWith(path, env.home)) return L"C:\\Users\\you" + path.substr(env.home.size());
    return path;
}

void App::drawHeader() {
    fill({0, 0, W, headerH()}, th.bg);
    float x = 32;
    bool meters = W >= 760 && !drives.empty();
    float textRight = meters ? W - 32 - 270 - 24 : W - 32;

    uint64_t total = 0;
    for (auto& c : cats) total += c.bytes();
    std::wstring big, tail;
    if (phase == Phase::Cleaning) big = L"Cleaning up";
    else if (phase == Phase::Scanning) big = sizeText(total), tail = L"  found so far";
    else if (total == 0) big = L"All clean";
    else big = sizeText(total), tail = L"  can be freed";

    // One layout with two font sizes, so both parts sit on the same baseline.
    std::wstring s = big + tail;
    ComPtr<IDWriteTextLayout> lay;
    dw->CreateTextLayout(s.c_str(), UINT32(s.size()), format({40, SEMI, DWRITE_TEXT_ALIGNMENT_LEADING, false, true, Font::Display}),
                         textRight - x, 60, &lay);
    ComPtr<ID2D1SolidColorBrush> dim;
    if (lay && !tail.empty()) {
        DWRITE_TEXT_RANGE rg{UINT32(big.size()), UINT32(tail.size())};
        lay->SetFontSize(18, rg);
        lay->SetFontWeight(DWRITE_FONT_WEIGHT_NORMAL, rg);
        lay->SetFontFamilyName(uiFont.c_str(), rg);
        rt->CreateSolidColorBrush(th.text2, &dim);
        lay->SetDrawingEffect(dim.Get(), rg);
    }
    brush->SetColor(th.text);
    if (lay) rt->DrawTextLayout({x, 22}, lay.Get(), brush.Get());

    // The line under the big number.
    D2D1_RECT_F l2{x, 80, textRight, 100};
    if (phase == Phase::Scanning) {
        std::wstring stage = L"Checking " + progress.stageName();
        TextStyle st{13};
        float sw = measure(stage, st);
        text(stage, l2, st, th.text2);
        text(progress.path(), {x + sw + 10, 80, textRight, 100}, {12.5f, DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_LEADING, true}, th.text3);
    } else if (phase == Phase::Cleaning) {
        text(countText(progress.files) + L" files removed so far", l2, {13}, th.text2);
    } else {
        std::wstring t = stopped ? L"Scan stopped early. " : L"";
        wchar_t secs[32];
        swprintf(secs, 32, L"%.1f", double(scanMs) / 1000.0);
        t += L"Checked " + countText(scanFiles) + L" files in " + secs + L" seconds";
        if (total == 0 && !stopped) t = L"Nothing worth deleting turned up. " + t + L".";
        text(t, l2, {13}, th.text2);
    }

    // Offer to look at system folders too.
    if (adminLink()) {
        std::wstring label = L"Include Windows system files";
        TextStyle st{13, SEMI};
        float w = measure(label, st);
        D2D1_RECT_F r{x, 108, x + 22 + w, 130};
        D2D1_COLOR_F c = hover.kind == Hit::Admin ? th.accentHover : th.accentInk;
        text(L"", {x, 108, x + 16, 130}, {13, DWRITE_FONT_WEIGHT_NORMAL, CENTER, false, false, Font::Icon}, c);
        text(label, {x + 22, 108, r.right, 130}, st, c);
        hits.push_back({r, Hit::Admin});
    }

    if (meters) drawMeters(W - 32 - 270, 24, 270);

    // A thin moving bar while something is happening.
    if (phase != Phase::Ready) {
        float t = float(GetTickCount64() % 1600) / 1600.f;
        float seg = std::max(120.f, W * 0.18f);
        float sx = -seg + (W + seg) * t;
        fill({std::max(0.f, sx), headerH() - 2, std::min(W, sx + seg), headerH()}, th.accent);
    }
}

void App::drawMeters(float x, float y, float w) {
    // How much each drive would gain. Recycle Bin moves don't free anything yet.
    std::map<wchar_t, uint64_t> gain;
    wchar_t sys = env.windir.empty() ? L'C' : wchar_t(towupper(env.windir[0]));
    for (auto& c : cats)
        for (auto& it : c.items) {
            if (!it.selected || it.recycle) continue;
            wchar_t d = it.action == Action::EmptyRecycleBin || it.paths.empty() ? sys : wchar_t(towupper(it.paths[0][0]));
            gain[d] += it.bytes;
        }
    std::vector<Drive> shown = drives;
    std::stable_sort(shown.begin(), shown.end(), [&](const Drive& a, const Drive& b) {
        if ((a.letter == sys) != (b.letter == sys)) return a.letter == sys;
        return gain[a.letter] > gain[b.letter];
    });
    if (shown.size() > 2) shown.resize(2);
    for (auto& d : shown) {
        uint64_t g = gain[d.letter];
        text(std::wstring(1, d.letter) + L":", {x, y, x + 40, y + 18}, {13, SEMI}, th.text);
        text(sizeText(d.free) + L" free of " + sizeText(d.total), {x + 30, y, x + w, y + 18}, {12, DWRITE_FONT_WEIGHT_NORMAL, RIGHT},
             th.text2);
        D2D1_RECT_F track{x, y + 24, x + w, y + 30};
        fillRound(track, 3, th.track);
        float used = d.total ? w * float(double(d.total - d.free) / double(d.total)) : 0;
        fillRound({x, y + 24, x + used, y + 30}, 3, th.meterUsed);
        if (g) {
            float gw = std::max(4.f, w * float(double(g) / double(d.total)));
            fillRound({std::max(x, x + used - gw), y + 24, x + used, y + 30}, 3, th.accent);
            text(sizeText(d.free + g) + L" free after cleaning", {x, y + 32, x + w, y + 48},
                 {12, DWRITE_FONT_WEIGHT_NORMAL, RIGHT}, th.accentInk);
        }
        y += 54;
    }
}

void App::drawList() {
    listTop = headerH();
    listBottom = H - kFooterH;
    float w = std::min(W - 48, kMaxWidth), x = (W - w) / 2;
    rt->PushAxisAlignedClip({0, listTop, W, listBottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = listTop + 6 - scroll;
    if (showBanner) y = drawBanner(x, y, w) + 10;
    if (phase == Phase::Ready && cats.empty()) y = drawEmpty(x, y, w) + 10;
    for (int i = 0; i < int(cats.size()); ++i) y = drawCategory(i, x, y, w) + 8;
    if (phase == Phase::Scanning) y = drawScanning(x, y, w) + 8;
    if (phase == Phase::Ready && !emptyCats.empty()) {
        std::wstring s = L"Also checked, nothing to clean: ";
        for (size_t i = 0; i < emptyCats.size(); ++i) s += (i ? L", " : L"") + emptyCats[i];
        text(s, {x + 4, y + 4, x + w, y + 40}, {12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true},
             th.text3);
        y += 44;
    }
    y += 10;
    contentH = y + scroll - listTop;
    rt->PopAxisAlignedClip();
    drawScrollbar();
}

float App::drawBanner(float x, float y, float w) {
    float h = 62;
    D2D1_RECT_F r{x, y, x + w, y + h};
    fillRound(r, 8, alpha(th.accent, th.dark ? 0.14f : 0.10f));
    strokeRound(r, 8, alpha(th.accent, 0.35f));
    D2D1_RECT_F dot{x + 18, y + 18, x + 44, y + 44};
    fillRound(dot, 13, th.accent);
    text(L"", dot, {12, DWRITE_FONT_WEIGHT_BOLD, CENTER, false, false, Font::Icon}, th.onAccent);

    const CleanResult& c = lastClean;
    std::wstring title = c.freed ? L"Freed " + sizeText(c.freed)
                       : c.recycled ? L"Moved " + sizeText(c.recycled) + L" to the Recycle Bin"
                                    : L"Nothing could be removed";
    std::wstring detail;
    if (c.freed && c.recycled) detail = sizeText(c.recycled) + L" went to the Recycle Bin";
    if (c.skipped) {
        if (!detail.empty()) detail += L"  ·  ";
        detail += countText(c.skipped) + (c.skipped == 1 ? L" file was" : L" files were") +
                  L" in use by another app and got skipped";
    }
    if (detail.empty()) detail = L"Everything you picked is gone.";
    text(title, {x + 58, y + 11, x + w - 50, y + 33}, {15, SEMI}, th.text);
    text(detail, {x + 58, y + 33, x + w - 50, y + 51}, {12.5f}, th.text2);

    D2D1_RECT_F close{x + w - 44, y + 17, x + w - 16, y + 45};
    if (hover.kind == Hit::BannerClose) fillRound(close, 6, alpha(th.text, 0.08f));
    text(L"", close, {11, DWRITE_FONT_WEIGHT_NORMAL, CENTER, false, false, Font::Icon}, th.text2);
    addHit(close, Hit::BannerClose);
    return y + h;
}

float App::drawCategory(int ci, float x, float y, float w) {
    Category& c = cats[ci];
    float h = kRowH + (c.expanded ? 1 + float(c.items.size()) * kItemH + 8 : 0);
    if (y + h < listTop || y > listBottom) return y + h;

    D2D1_RECT_F card{x, y, x + w, y + h};
    fillRound(card, 8, th.card);
    bool rowHot = hover.cat == ci && (hover.kind == Hit::CatRow || hover.kind == Hit::CatCheck);
    if (rowHot) {
        fillRound({x, y, x + w, y + kRowH}, 8, th.cardHover);
        if (c.expanded) fill({x, y + kRowH - 8, x + w, y + kRowH}, th.cardHover);
    }
    strokeRound(card, 8, th.border);

    checkbox(x + 20, y + 30, c.selectionState(), hover.kind == Hit::CatCheck && hover.cat == ci);
    addHit({x, y, x + 54, y + kRowH}, Hit::CatCheck, ci);
    addHit({x + 54, y, x + w, y + kRowH}, Hit::CatRow, ci);

    D2D1_RECT_F icon{x + 58, y + 21, x + 94, y + 57};
    fillRound(icon, 8, tintBg(c.tint));
    text(std::wstring(1, c.glyph), icon, {17, DWRITE_FONT_WEIGHT_NORMAL, CENTER, false, false, Font::Icon}, tintFg(c.tint));

    // Right column: size, then how many items (or how much of it is ticked).
    float right = x + w - 46;
    std::wstring size = sizeText(c.bytes());
    int state = c.selectionState();
    std::wstring sub = state == 1 ? sizeText(c.selectedBytes()) + L" ticked"
                     : c.items.size() == 1 ? L"1 item" : countText(c.items.size()) + L" items";
    TextStyle sizeStyle{16, SEMI, RIGHT}, subStyle{12, DWRITE_FONT_WEIGHT_NORMAL, RIGHT};
    text(size, {right - 140, y + 17, right, y + 41}, sizeStyle, th.text);
    text(sub, {right - 160, y + 41, right, y + 59}, subStyle, th.text3);
    text(c.expanded ? L"" : L"", {x + w - 40, y, x + w - 14, y + kRowH},
         {11, DWRITE_FONT_WEIGHT_NORMAL, CENTER, false, false, Font::Icon}, th.text3);

    const wchar_t* label = c.safety == Safety::Safe ? L"Safe" : c.safety == Safety::Rebuilds ? L"Rebuilds itself" : L"Check first";
    D2D1_COLOR_F bc = c.safety == Safety::Safe ? th.safe : c.safety == Safety::Rebuilds ? th.rebuilds : th.checkFirst;
    TextStyle badge{11.5f, SEMI, CENTER};
    float bw = measure(label, badge) + 18;
    float colW = std::max({measure(size, sizeStyle), measure(sub, subStyle), 64.f});
    float bx = right - colW - 18 - bw;
    D2D1_RECT_F b{bx, y + 28, bx + bw, y + 50};
    fillRound(b, 11, alpha(bc, th.dark ? 0.16f : 0.11f));
    text(label, b, badge, bc);

    float tx = x + 110, tr = bx - 18;
    text(c.name, {tx, y + 13, tr, y + 36}, {15, SEMI}, th.text);
    text(c.blurb, {tx, y + 37, tr, y + 72}, {12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true}, th.text2);

    if (!c.expanded) return y + h;
    line(x + 1, y + kRowH + 0.5f, x + w - 1, y + kRowH + 0.5f, th.divider);
    float iy = y + kRowH + 5;
    for (int ii = 0; ii < int(c.items.size()); ++ii, iy += kItemH)
        if (iy + kItemH >= listTop && iy <= listBottom) drawItem(ci, ii, x, iy, w);
    return y + h;
}

void App::drawItem(int ci, int ii, float x, float y, float w) {
    Item& it = cats[ci].items[ii];
    D2D1_RECT_F row{x + 8, y, x + w - 8, y + kItemH};
    bool hot = hover.kind == Hit::Item && hover.cat == ci && hover.item == ii;
    if (hot) fillRound(row, 6, th.cardHover);
    addHit(row, Hit::Item, ci, ii);
    checkbox(x + 67, y + 15, it.selected ? 2 : 0, hot);

    float right = x + w - 46, colW = 130, tx = x + 110, tr = right - colW - 12;
    text(sizeText(it.bytes), {right - colW, y + 6, right, y + 26}, {13, SEMI, RIGHT}, th.text);
    if (it.showAge) text(ageText(it.newest), {right - colW, y + 26, right, y + 43}, {12, DWRITE_FONT_WEIGHT_NORMAL, RIGHT}, th.text3);

    TextStyle ls{13.5f};
    float lw = std::min(measure(it.label, ls), tr - tx);
    text(it.label, {tx, y + 6, tx + lw + 1, y + 26}, ls, th.text);
    if (!it.kind.empty() && tx + lw + 40 < tr) text(it.kind, {tx + lw + 8, y + 6, tr, y + 26}, {12.5f}, th.text3);
    std::wstring l2 = shown(!it.line2.empty() ? it.line2 : it.paths.empty() ? L"" : it.paths[0]);
    text(l2, {tx, y + 26, tr, y + 43}, {12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true}, th.text3);
}

float App::drawScanning(float x, float y, float w) {
    float h = 58;
    D2D1_RECT_F r{x, y, x + w, y + h};
    fillRound(r, 8, alpha(th.card, 0.6f));
    strokeRound(r, 8, th.border);
    // Three dots taking turns.
    float t = float(GetTickCount64() % 1200) / 1200.f;
    for (int i = 0; i < 3; ++i) {
        float ph = std::fmod(t + 1.f - i * 0.18f, 1.f);
        float a = 0.25f + 0.75f * std::max(0.f, 1.f - std::fabs(ph - 0.5f) * 3.f);
        float cx = x + 26 + i * 11.f, cy = y + h / 2;
        fillRound({cx - 3, cy - 3, cx + 3, cy + 3}, 3, alpha(th.accent, a));
    }
    text(L"Checking " + progress.stageName() + L"…", {x + 70, y, x + w - 20, y + h}, {13.5f}, th.text2);
    return y + h;
}

float App::drawEmpty(float x, float y, float w) {
    float h = 170;
    D2D1_RECT_F r{x, y, x + w, y + h};
    fillRound(r, 8, th.card);
    strokeRound(r, 8, th.border);
    float cx = x + w / 2;
    D2D1_RECT_F dot{cx - 22, y + 28, cx + 22, y + 72};
    fillRound(dot, 22, alpha(th.accent, th.dark ? 0.18f : 0.13f));
    text(L"", dot, {18, DWRITE_FONT_WEIGHT_NORMAL, CENTER, false, false, Font::Icon}, th.accentInk);
    text(stopped ? L"Scan stopped" : L"Nothing worth deleting", {x, y + 86, x + w, y + 110}, {17, SEMI, CENTER}, th.text);
    text(stopped ? L"Press Rescan to run it again." : L"Your PC is in good shape. Check back in a few weeks.",
         {x, y + 112, x + w, y + 134}, {13, DWRITE_FONT_WEIGHT_NORMAL, CENTER}, th.text2);
    return y + h;
}

void App::drawScrollbar() {
    float view = listBottom - listTop;
    if (contentH <= view + 1) return;
    float trackTop = listTop + 4, trackH = view - 8;
    float thumbH = std::max(36.f, trackH * view / contentH);
    float m = maxScroll();
    float ty = trackTop + (trackH - thumbH) * (m > 0 ? scroll / m : 0);
    thumbTravel = (trackH - thumbH) > 0 ? m / (trackH - thumbH) : 0;
    bool hot = hover.kind == Hit::Thumb || dragging;
    float tw = hot ? 6.f : 4.f, tx = W - 5 - tw;
    fillRound({tx, ty, tx + tw, ty + thumbH}, tw / 2, hot ? th.text3 : th.scroll);
    hits.push_back({{W - 16, ty, W, ty + thumbH}, Hit::Thumb});
}

void App::drawFooter() {
    float top = H - kFooterH;
    fill({0, top, W, H}, th.footer);
    line(0, top + 0.5f, W, top + 0.5f, th.divider);
    uint64_t perm, rec;
    bool bin;
    totals(perm, rec, bin);
    uint64_t sel = perm + rec;

    float bx, by = top + 19, bh = 38;
    std::wstring cl = phase == Phase::Cleaning ? L"Cleaning…" : L"Clean up";
    float bw = std::max(124.f, measure(cl, {14, SEMI}) + 44);
    bx = W - 32 - bw;
    button({bx, by, bx + bw, by + bh}, cl, true, phase == Phase::Ready && sel > 0, Hit::Clean);
    float rw = 100;
    button({bx - 10 - rw, by, bx - 10, by + bh}, phase == Phase::Scanning ? L"Stop" : L"Rescan", false,
           phase != Phase::Cleaning, Hit::Rescan);

    float tr = bx - 10 - rw - 20;
    text(sel ? sizeText(sel) + L" selected" : L"Nothing selected", {32, top + 15, tr, top + 39}, {15, SEMI}, th.text);
    std::wstring t2;
    if (phase == Phase::Cleaning) {
        t2 = L"Files that are open in other apps are skipped";
    } else if (sel) {
        if (perm) t2 = sizeText(perm) + L" deleted for good";
        if (rec) t2 += (t2.empty() ? L"" : L", ") + sizeText(rec) + L" moved to the Recycle Bin";
    } else {
        t2 = phase == Phase::Scanning ? L"Results show up as each check finishes" : L"Tick what you want gone";
    }
    text(t2, {32, top + 39, tr, top + 58}, {12.5f}, th.text2);
}

void App::drawConfirm() {
    fill({0, 0, W, H}, rgb(0x000000, th.dark ? 0.55f : 0.32f));
    uint64_t perm, rec;
    bool bin;
    totals(perm, rec, bin);
    std::vector<std::pair<std::wstring, bool>> lines;  // text, muted
    if (perm) lines.push_back({sizeText(perm) + L" will be deleted permanently.", false});
    if (rec) lines.push_back({sizeText(rec) + L" will be moved to the Recycle Bin.", false});
    if (bin) lines.push_back({L"The Recycle Bin will be emptied. That can't be undone.", false});
    lines.push_back({L"Files that are open in another app are skipped.", true});

    float cw = std::min(460.f, W - 48), ch = 24 + 34 + float(lines.size()) * 23 + 26 + 38 + 22;
    float x = (W - cw) / 2, y = (H - ch) / 2;
    D2D1_RECT_F r{x, y, x + cw, y + ch};
    fillRound(r, 10, th.dialog);
    strokeRound(r, 10, th.border);
    text(L"Clean up " + sizeText(perm + rec) + L"?", {x + 26, y + 22, x + cw - 26, y + 52}, {20, SEMI}, th.text);
    float ly = y + 62;
    for (auto& [s, muted] : lines) {
        text(s, {x + 26, ly, x + cw - 26, ly + 22}, {13.5f}, muted ? th.text3 : th.text2);
        ly += 23;
    }
    float by = y + ch - 22 - 38;
    float okW = 124, cancelW = 96;
    button({x + cw - 26 - okW, by, x + cw - 26, by + 38}, L"Clean up", true, true, Hit::ConfirmOk);
    button({x + cw - 26 - okW - 10 - cancelW, by, x + cw - 26 - okW - 10, by + 38}, L"Cancel", false, true,
           Hit::ConfirmCancel);
}

// ------------------------------------------------------------------ behaviour

void App::ensureTimer() {
    if (timerOn) return;
    SetTimer(hwnd, kTimer, 16, nullptr);
    timerOn = true;
}

void App::refreshDrives() {
    drives.clear();
    if (env.sandbox) {
        // Show the drive the sandbox lives on, so the meter still means something.
        wchar_t root[] = {env.home.empty() ? L'C' : env.home[0], L':', L'\\', 0};
        ULARGE_INTEGER freeBytes, total;
        if (GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr))
            drives.push_back({wchar_t(towupper(root[0])), total.QuadPart, freeBytes.QuadPart});
        return;
    }
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeBytes, total;
        if (GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr) && total.QuadPart)
            drives.push_back({root[0], total.QuadPart, freeBytes.QuadPart});
    }
}

void App::startScan() {
    if (worker.joinable()) worker.join();
    cats.clear();
    emptyCats.clear();
    {
        std::lock_guard<std::mutex> lock(pendingMx);
        pending.clear();
    }
    scroll = scrollTarget = 0;
    progress.reset();
    phase = Phase::Scanning;
    stopped = false;
    scanStart = GetTickCount64();
    refreshDrives();
    worker = std::thread([this] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        scanAll(env, progress, [this](Category&& c) {
            {
                std::lock_guard<std::mutex> lock(pendingMx);
                pending.push_back(std::move(c));
            }
            PostMessageW(hwnd, WM_APP_CATEGORY, 0, 0);
        });
        CoUninitialize();
        PostMessageW(hwnd, WM_APP_SCAN_DONE, 0, 0);
    });
    ensureTimer();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::startClean() {
    std::vector<Item> chosen;
    for (auto& c : cats)
        for (auto& it : c.items)
            if (it.selected) chosen.push_back(it);
    if (chosen.empty()) return;
    if (worker.joinable()) worker.join();
    progress.reset();
    phase = Phase::Cleaning;
    worker = std::thread([this, chosen] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CleanResult r = clean(chosen, env, progress);
        CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(pendingMx);
            pendingClean = r;
        }
        PostMessageW(hwnd, WM_APP_CLEAN_DONE, 0, 0);
    });
    ensureTimer();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::click(const HitBox& h) {
    switch (h.kind) {
        case Hit::CatCheck: {
            auto& c = cats[size_t(h.cat)];
            bool on = c.selectionState() != 2;
            for (auto& it : c.items) it.selected = on;
            break;
        }
        case Hit::CatRow: cats[size_t(h.cat)].expanded = !cats[size_t(h.cat)].expanded; break;
        case Hit::Item: {
            auto& it = cats[size_t(h.cat)].items[size_t(h.item)];
            it.selected = !it.selected;
            break;
        }
        case Hit::Clean: confirmOpen = true; break;
        case Hit::Rescan:
            if (phase == Phase::Scanning) progress.cancel = true;
            else if (phase == Phase::Ready) startScan();
            break;
        case Hit::Admin: relaunchAsAdmin(); break;
        case Hit::BannerClose: showBanner = false; break;
        case Hit::ConfirmOk:
            confirmOpen = false;
            startClean();
            break;
        case Hit::ConfirmCancel: confirmOpen = false; break;
        default: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void App::reveal(const Item& it) {
    if (it.action == Action::EmptyRecycleBin) {
        ShellExecuteW(hwnd, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (!it.paths.empty()) {
        if (it.action == Action::DeleteContents || it.paths.size() > 1) {
            std::wstring dir = it.paths.size() > 1 ? fs::commonFolder(it.paths) : it.paths[0];
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            std::wstring args = L"/select,\"" + it.paths[0] + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
}

void App::itemMenu(int ci, int ii) {
    const Item& it = cats[size_t(ci)].items[size_t(ii)];
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Show in File Explorer");
    if (!it.paths.empty()) AppendMenuW(m, MF_STRING, 2, L"Copy path");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) reveal(it);
    if (cmd == 2 && OpenClipboard(hwnd)) {
        std::wstring s = it.paths[0];
        EmptyClipboard();
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
            memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    }
}

void App::relaunchAsAdmin() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.hwnd = hwnd;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// ------------------------------------------------------------------ window plumbing

LRESULT CALLBACK App::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        app->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return app ? app->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::handle(UINT m, WPARAM w, LPARAM l) {
    auto dip = [&](LPARAM p, float& x, float& y) {
        x = float(GET_X_LPARAM(p)) * 96.f / dpi;
        y = float(GET_Y_LPARAM(p)) * 96.f / dpi;
    };
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: {
            W = float(LOWORD(l)) * 96.f / dpi;
            H = float(HIWORD(l)) * 96.f / dpi;
            if (hwndRt) hwndRt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DPICHANGED: {
            dpi = float(HIWORD(w));
            if (rt) rt->SetDpi(dpi, dpi);
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = {LONG(640 * dpi / 96), LONG(520 * dpi / 96)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (l && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0) {
                applyTheme();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            float x, y;
            dip(l, x, y);
            if (!tracking) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                tracking = true;
            }
            if (dragging) {
                scroll = scrollTarget = std::clamp(dragScroll + (y - dragY) * thumbTravel, 0.f, maxScroll());
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            HitBox h = hitAt(x, y);
            if (!h.same(hover)) {
                hover = h;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (!dragging) {
                hover = {};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            float x, y;
            dip(l, x, y);
            down = hitAt(x, y);
            SetCapture(hwnd);
            if (down.kind == Hit::Thumb) {
                dragging = true;
                dragY = y;
                dragScroll = scroll;
            }
            if (m == WM_LBUTTONDBLCLK && down.kind == Hit::Item) reveal(cats[size_t(down.cat)].items[size_t(down.item)]);
            return 0;
        }
        case WM_LBUTTONUP: {
            float x, y;
            dip(l, x, y);
            ReleaseCapture();
            if (dragging) {
                dragging = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            HitBox h = hitAt(x, y);
            if (h.kind != Hit::None && h.same(down)) click(h);
            down = {};
            return 0;
        }
        case WM_RBUTTONUP: {
            float x, y;
            dip(l, x, y);
            HitBox h = hitAt(x, y);
            if (h.kind == Hit::Item) itemMenu(h.cat, h.item);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (confirmOpen) return 0;
            float delta = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
            scrollTarget = std::clamp(scrollTarget - delta * 90.f, 0.f, maxScroll());
            ensureTimer();
            return 0;
        }
        case WM_KEYDOWN:
            if (confirmOpen) {
                if (w == VK_ESCAPE) click({{}, Hit::ConfirmCancel});
                if (w == VK_RETURN) click({{}, Hit::ConfirmOk});
            } else if (w == VK_F5 && phase == Phase::Ready) {
                startScan();
            } else if (w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
                float page = (listBottom - listTop) * 0.85f;
                scrollTarget = w == VK_HOME ? 0 : w == VK_END ? maxScroll() : scrollTarget + (w == VK_NEXT ? page : -page);
                scrollTarget = std::clamp(scrollTarget, 0.f, maxScroll());
                ensureTimer();
            }
            return 0;
        case WM_TIMER: {
            bool moving = std::fabs(scrollTarget - scroll) > 0.5f;
            scroll = moving ? scroll + (scrollTarget - scroll) * 0.28f : scrollTarget;
            if (!moving && phase == Phase::Ready) {
                KillTimer(hwnd, kTimer);
                timerOn = false;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP_CATEGORY: {
            std::vector<Category> got;
            {
                std::lock_guard<std::mutex> lock(pendingMx);
                got.swap(pending);
            }
            for (auto& c : got) {
                if (c.bytes() > 0) cats.push_back(std::move(c));
                else emptyCats.push_back(c.name);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP_SCAN_DONE:
            if (worker.joinable()) worker.join();
            scanMs = GetTickCount64() - scanStart;
            scanFiles = progress.files;
            stopped = progress.cancelled();
            phase = Phase::Ready;
            std::stable_sort(cats.begin(), cats.end(), [](const Category& a, const Category& b) { return a.bytes() > b.bytes(); });
            refreshDrives();
            if (!shotPath.empty()) {
                for (auto& c : cats)
                    for (auto& id : shotExpand)
                        if (wide(c.id) == id) c.expanded = true;
                saveScreenshot();
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_CLEAN_DONE:
            if (worker.joinable()) worker.join();
            {
                std::lock_guard<std::mutex> lock(pendingMx);
                lastClean = pendingClean;
            }
            showBanner = true;
            startScan();  // show what's left
            return 0;
        case WM_CLOSE:
            progress.cancel = true;
            if (worker.joinable()) worker.join();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

bool hasFamily(IDWriteFactory* dw, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
    UINT32 index;
    BOOL exists = FALSE;
    fonts->FindFamilyName(name, &index, &exists);
    return exists;
}

int App::run(HINSTANCE inst, int show, const std::vector<std::wstring>& args) {
    env = detectEnv();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--home" && i + 1 < args.size()) {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(args[i + 1].c_str(), MAX_PATH, full, nullptr);
            env = sandboxEnv(full);
        }
    }
    float shotW = 0, shotH = 0;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == L"--screenshot") shotPath = args[i + 1];
        if (args[i] == L"--theme") shotTheme = args[i + 1];
        if (args[i] == L"--size") swscanf_s(args[i + 1].c_str(), L"%fx%f", &shotW, &shotH);
        if (args[i] == L"--expand") {
            std::wstring list = args[i + 1];
            for (size_t a = 0, b; a <= list.size(); a = b + 1) {
                b = list.find(L',', a);
                if (b == std::wstring::npos) b = list.size();
                if (b > a) shotExpand.push_back(list.substr(a, b - a));
            }
        }
    }

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    if (!d2d || !dw) return 1;
    if (hasFamily(dw.Get(), L"Segoe UI Variable Text")) uiFont = L"Segoe UI Variable Text";
    if (hasFamily(dw.Get(), L"Segoe UI Variable Display")) displayFont = L"Segoe UI Variable Display";
    if (hasFamily(dw.Get(), L"Segoe Fluent Icons")) iconFont = L"Segoe Fluent Icons";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RoomyWindow";
    RegisterClassExW(&wc);

    // Size the window for the monitor the cursor is on.
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT mdpi = GetDpiForSystem();
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
        UINT dx, dy;
        if (fn && SUCCEEDED(fn(mon, 0, &dx, &dy))) mdpi = dx;
    }
    int ww = MulDiv(980, int(mdpi), 96), wh = MulDiv(760, int(mdpi), 96);
    RECT work = mi.rcWork;
    ww = std::min(ww, int(work.right - work.left) - 40);
    wh = std::min(wh, int(work.bottom - work.top) - 40);
    int wx = work.left + (work.right - work.left - ww) / 2, wy = work.top + (work.bottom - work.top - wh) / 2;

    hwnd = CreateWindowExW(0, wc.lpszClassName, env.sandbox ? L"Roomy (sandbox)" : L"Roomy", WS_OVERLAPPEDWINDOW, wx, wy,
                           ww, wh, nullptr, nullptr, inst, this);
    if (!hwnd) return 1;
    dpi = float(GetDpiForWindow(hwnd));
    applyTheme();
    RECT rc;
    GetClientRect(hwnd, &rc);
    W = float(rc.right) * 96.f / dpi;
    H = float(rc.bottom) * 96.f / dpi;
    if (!shotPath.empty()) {
        if (shotW > 0 && shotH > 0) W = shotW, H = shotH;  // drawn off screen, so any size works
    } else {
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
    }
    startScan();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

}  // namespace

int runUi(HINSTANCE inst, int show, const std::vector<std::wstring>& args) {
    App app;
    return app.run(inst, show, args);
}

}  // namespace roomy
