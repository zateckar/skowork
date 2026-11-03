// Tests of the native UI device layer (add-native-ui task 2.1): software rendering (WARP), dirty-rectangle frames,
// device-loss recovery, DPI and theme.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "ui/bundles.h"
#include "ui/device.h"
#include "ui/theme.h"

using namespace wb;
using namespace wb::ui;

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kRed = 0xFFFF0000, kBlue = 0xFF0000FF;  // BGRA in memory = 0xAARRGGBB

void fill(Renderer& r, const Rect& dip, uint32_t rgb) {
    com_ptr<ID2D1SolidColorBrush> b;
    ASSERT_TRUE(SUCCEEDED(r.dc()->CreateSolidColorBrush(color(rgb), b.put())));
    r.dc()->FillRectangle(D2D1::RectF(dip.x, dip.y, dip.right(), dip.bottom()), b.get());
}

// Draws every region the renderer asks for with `draw` inside a clip, then ends the frame.
template <class F>
Renderer::FrameEnd frame(Renderer& r, const std::vector<Rect>& dirty, F draw, std::vector<Rect>* regions = nullptr) {
    const std::vector<Rect> todo = r.begin_frame(dirty);
    if (regions) *regions = todo;
    for (const Rect& g : todo) {
        r.dc()->PushAxisAlignedClip(D2D1::RectF(g.x, g.y, g.right(), g.bottom()), D2D1_ANTIALIAS_MODE_ALIASED);
        draw();
        r.dc()->PopAxisAlignedClip();
    }
    auto e = r.end_frame();
    EXPECT_TRUE(e.ok()) << (e.ok() ? "" : e.error().message);
    return e.ok() ? *e : Renderer::FrameEnd::presented;
}

uint32_t at(const std::vector<uint32_t>& px, UINT w, UINT x, UINT y) { return px[static_cast<size_t>(y) * w + x]; }

struct TestWindow {
    HWND hwnd = nullptr;
    explicit TestWindow(int w, int h) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = ::DefWindowProcW;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WbUiDeviceTest";
        ::RegisterClassExW(&wc);
        RECT rc{0, 0, w, h};
        ::AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
    }
    ~TestWindow() {
        if (hwnd) ::DestroyWindow(hwnd);
    }
};

}  // namespace

TEST(UiDevice, UsesTheSoftwareAdapter) {
    auto r = Renderer::offscreen(16, 16, 96);
    ASSERT_TRUE(r.ok()) << r.error().message;
    com_ptr<IDXGIDevice> dxgi;
    ASSERT_TRUE(SUCCEEDED((*r)->d3d()->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void())));
    com_ptr<IDXGIAdapter> adapter;
    ASSERT_TRUE(SUCCEEDED(dxgi->GetAdapter(adapter.put())));
    DXGI_ADAPTER_DESC d{};
    adapter->GetDesc(&d);
    EXPECT_EQ(d.VendorId, 0x1414u);  // Microsoft Basic Render Driver (WARP), not a GPU
    EXPECT_EQ((*r)->generation(), 1u);
}

TEST(UiDevice, OffscreenFullThenDirtyFrame) {
    auto rr = Renderer::offscreen(100, 80, 96);
    ASSERT_TRUE(rr.ok()) << rr.error().message;
    Renderer& r = **rr;
    std::vector<Rect> regions;
    frame(r, {}, [&] { r.dc()->Clear(color(0xFFFFFF)); }, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 100, 80}}));  // the first frame is full whatever is dirty

    EXPECT_TRUE(r.begin_frame({}).empty());  // nothing dirty, nothing to draw

    frame(r, {{10, 10, 20, 20}}, [&] { r.dc()->Clear(color(0x0000FF)); }, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{10, 10, 20, 20}}));
    auto px = r.read_pixels();
    ASSERT_TRUE(px.ok()) << px.error().message;
    EXPECT_EQ(at(*px, 100, 15, 15), kBlue);
    EXPECT_EQ(at(*px, 100, 29, 29), kBlue);
    EXPECT_EQ(at(*px, 100, 30, 30), kWhite);  // the clear stayed inside the region
    EXPECT_EQ(at(*px, 100, 5, 5), kWhite);
}

TEST(UiDevice, DpiScalesDipsToPixels) {
    auto rr = Renderer::offscreen(200, 200, 192);
    ASSERT_TRUE(rr.ok()) << rr.error().message;
    Renderer& r = **rr;
    EXPECT_FLOAT_EQ(r.size_dip().w, 100);
    frame(r, {}, [&] {
        r.dc()->Clear(color(0xFFFFFF));
        fill(r, {0, 0, 50, 50}, 0xFF0000);
    });
    auto px = r.read_pixels();
    ASSERT_TRUE(px.ok());
    EXPECT_EQ(at(*px, 200, 95, 95), kRed);  // 50 DIPs = 100 px at 200 %
    EXPECT_EQ(at(*px, 200, 105, 105), kWhite);

    ASSERT_TRUE(r.set_dpi(96).ok());
    EXPECT_FLOAT_EQ(r.size_dip().w, 200);
    std::vector<Rect> regions;
    frame(r, {{0, 0, 1, 1}}, [&] {
        r.dc()->Clear(color(0xFFFFFF));
        fill(r, {0, 0, 50, 50}, 0xFF0000);
    }, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 200, 200}}));  // a DPI change redraws everything
    px = r.read_pixels();
    ASSERT_TRUE(px.ok());
    EXPECT_EQ(at(*px, 200, 45, 45), kRed);
    EXPECT_EQ(at(*px, 200, 55, 55), kWhite);
}

TEST(UiDevice, DeviceLossRecreatesAndRedrawsInFull) {
    auto rr = Renderer::offscreen(64, 64, 96);
    ASSERT_TRUE(rr.ok());
    Renderer& r = **rr;
    frame(r, {}, [&] { r.dc()->Clear(color(0xFFFFFF)); });
    ID2D1DeviceContext* before = r.dc();
    r.simulate_device_loss_for_test();
    EXPECT_EQ(frame(r, {{0, 0, 8, 8}}, [&] { r.dc()->Clear(color(0x0000FF)); }), Renderer::FrameEnd::recreated);
    EXPECT_EQ(r.generation(), 2u);
    EXPECT_NE(r.dc(), nullptr);
    (void)before;
    std::vector<Rect> regions;
    EXPECT_EQ(frame(r, {{0, 0, 8, 8}}, [&] { r.dc()->Clear(color(0xFF0000)); }, &regions), Renderer::FrameEnd::presented);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 64, 64}}));
    auto px = r.read_pixels();
    ASSERT_TRUE(px.ok());
    EXPECT_EQ(at(*px, 64, 60, 60), kRed);
}

TEST(UiDevice, WindowFlipChainRedrawsPreviousDirtyAndPresentsOnlyCurrent) {
    TestWindow w(300, 200);
    ASSERT_NE(w.hwnd, nullptr);
    auto rr = Renderer::for_window(w.hwnd, 96);
    ASSERT_TRUE(rr.ok()) << rr.error().message;
    Renderer& r = **rr;
    EXPECT_EQ(r.width_px(), 300u);
    std::vector<Rect> regions;
    auto clear = [&] { r.dc()->Clear(color(0xFFFFFF)); };
    frame(r, {}, clear, &regions);
    EXPECT_EQ(regions.size(), 1u);
    EXPECT_TRUE(r.last_present_rects().empty());  // full present

    // Frame 2: the back buffer is two frames old, which was the full frame's predecessor: redraw everything.
    frame(r, {{10, 10, 10, 10}}, clear, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 300, 200}}));
    ASSERT_EQ(r.last_present_rects().size(), 1u);
    EXPECT_EQ(r.last_present_rects()[0].left, 10);
    EXPECT_EQ(r.last_present_rects()[0].right, 20);

    // Frame 3: its own dirty rectangle plus frame 2's.
    frame(r, {{100, 100, 10, 10}}, clear, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{100, 100, 10, 10}, {10, 10, 10, 10}}));
    ASSERT_EQ(r.last_present_rects().size(), 1u);
    EXPECT_EQ(r.last_present_rects()[0].left, 100);

    // Resize: the next frame is full again.
    ASSERT_TRUE(r.resize(400, 250).ok());
    EXPECT_FLOAT_EQ(r.size_dip().w, 400);
    frame(r, {{0, 0, 1, 1}}, clear, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 400, 250}}));
    EXPECT_TRUE(r.last_present_rects().empty());

    // Device loss on the window path.
    r.simulate_device_loss_for_test();
    EXPECT_EQ(frame(r, {{0, 0, 5, 5}}, clear), Renderer::FrameEnd::recreated);
    frame(r, {{0, 0, 5, 5}}, clear, &regions);
    EXPECT_EQ(regions, (std::vector<Rect>{{0, 0, 400, 250}}));
}

TEST(UiTheme, HighContrastUsesSystemColoursOnly) {
    const SystemColors sys{0x000000, 0xFFFF00, 0x1AEBFF, 0x000000, 0x8080FF, 0x3FF23F, 0x000000, 0xFFFFFF};
    const Theme t = make_theme(true, true, sys);
    EXPECT_TRUE(t.high_contrast);
    EXPECT_FALSE(t.shadows);
    EXPECT_EQ(t.background, 0x000000u);
    EXPECT_EQ(t.text, 0xFFFF00u);
    EXPECT_EQ(t.accent, 0x1AEBFFu);  // the accent colour is ignored
    EXPECT_EQ(t.link, 0x8080FFu);
    EXPECT_EQ(t.text_secondary, 0x3FF23Fu);
}

// improve-desktop-look D1: the brand colours are the accent whatever the Windows accent is.
TEST(UiTheme, BrandAccentInLightAndDark) {
    const SystemColors sys{};
    const Theme light = make_theme(false, false, sys);
    const Theme dark = make_theme(true, false, sys);
    EXPECT_GT(light.background, dark.background);
    EXPECT_EQ(light.accent, 0x0E3A2Fu);  // Emerald
    EXPECT_EQ(light.accent_text, 0xFFFFFFu);
    EXPECT_EQ(dark.accent, 0x78FAAEu);  // Electric Green
    EXPECT_EQ(dark.accent_text, 0x0E3A2Fu);
    EXPECT_TRUE(light.shadows);
    const Theme now = current_theme();  // reads the system; must not fail
    EXPECT_NE(now.text, now.background);
}

namespace {
// WCAG 2.1 relative luminance and contrast ratio.
double channel(uint32_t c) {
    const double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}
double rel_luminance(uint32_t rgb) {
    return 0.2126 * channel((rgb >> 16) & 0xFF) + 0.7152 * channel((rgb >> 8) & 0xFF) + 0.0722 * channel(rgb & 0xFF);
}
double contrast(uint32_t a, uint32_t b) {
    const double la = rel_luminance(a), lb = rel_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
}  // namespace

// client/work-inbox Colour Contrast (improve-desktop-look D2): text 4.5:1 and control boundaries and focus 3:1 against
// the colours they are drawn on, in both themes.
TEST(UiTheme, PalettesMeetWcagAaContrast) {
    for (const bool dark : {false, true}) {
        const Theme t = make_theme(dark, false, SystemColors{});
        const char* mode = dark ? "dark" : "light";
        const std::pair<const char*, uint32_t> grounds[] = {
            {"background", t.background}, {"surface", t.surface}, {"surface_alt", t.surface_alt}, {"code_background", t.code_background},
            {"accent_surface", t.accent_surface}};
        for (const auto& [gname, g] : grounds) {
            EXPECT_GE(contrast(t.text, g), 4.5) << mode << " text on " << gname;
            EXPECT_GE(contrast(t.text_secondary, g), 4.5) << mode << " text_secondary on " << gname;
            EXPECT_GE(contrast(t.link, g), 4.5) << mode << " link on " << gname;
            EXPECT_GE(contrast(t.error, g), 4.5) << mode << " error on " << gname;
            EXPECT_GE(contrast(t.warning, g), 4.5) << mode << " warning on " << gname;
            EXPECT_GE(contrast(t.success, g), 4.5) << mode << " success on " << gname;
            EXPECT_GE(contrast(t.info, g), 4.5) << mode << " info on " << gname;
            EXPECT_GE(contrast(t.control_border, g), 3.0) << mode << " control_border on " << gname;
            EXPECT_GE(contrast(t.focus, g), 3.0) << mode << " focus on " << gname;
        }
        EXPECT_GE(contrast(t.accent_text, t.accent), 4.5) << mode << " accent_text on accent";
        EXPECT_GE(contrast(t.selection_text, t.selection), 4.5) << mode << " selection_text on selection";
        EXPECT_GE(contrast(t.text_secondary, t.selection), 4.5) << mode << " text_secondary on selection";
        EXPECT_GE(contrast(t.accent, t.surface), 3.0) << mode << " accent (selection marker) on surface";
        EXPECT_GE(contrast(t.error, t.error_background), 4.5) << mode << " error on its banner";
        EXPECT_GE(contrast(t.warning, t.warning_background), 4.5) << mode << " warning on its banner";
        EXPECT_GE(contrast(t.info, t.info_background), 4.5) << mode << " info on its banner";
        EXPECT_GE(contrast(t.text, t.error_background), 4.5) << mode << " text on the error banner";
    }
}
TEST(UiStrings, SystemLanguageIsATag) {
    const std::string tag = system_language();
    EXPECT_GE(tag.size(), 2u);
    const std::string lang = resolve_language("system", tag);
    EXPECT_TRUE(lang == "cs" || lang == "de" || lang == "en");
}

TEST(UiTheme, RecognisesThemeChangeMessages) {
    EXPECT_TRUE(is_theme_change(WM_THEMECHANGED, 0, 0));
    EXPECT_TRUE(is_theme_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet")));
    EXPECT_FALSE(is_theme_change(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"intl")));
    EXPECT_FALSE(is_theme_change(WM_PAINT, 0, 0));
}
