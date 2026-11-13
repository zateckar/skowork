// Golden images of the native window (add-native-ui design D10): references under tests/ui/golden, compared with a
// tolerance for anti-aliasing. Shared by the element tests and the screen tests.
#pragma once

#include <gtest/gtest.h>

#include <wincodec.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ui/device.h"
#include "ui/elements.h"

namespace {

using wb::com_ptr;
// --- golden images ----------------------------------------------------------------------------------------------

inline std::filesystem::path golden_dir() { return std::filesystem::path(SKOWORK_UI_GOLDEN_DIR); }

bool write_png(const std::filesystem::path& file, const std::vector<uint32_t>& px, UINT w, UINT h) {
    com_ptr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, wic.put_void()))) return false;
    com_ptr<IWICStream> s;
    wic->CreateStream(s.put());
    if (FAILED(s->InitializeFromFilename(file.wstring().c_str(), GENERIC_WRITE))) return false;
    com_ptr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put());
    enc->Initialize(s.get(), WICBitmapEncoderNoCache);
    com_ptr<IWICBitmapFrameEncode> f;
    enc->CreateNewFrame(f.put(), nullptr);
    f->Initialize(nullptr);
    f->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    f->SetPixelFormat(&fmt);
    f->WritePixels(h, w * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(const_cast<uint32_t*>(px.data())));
    f->Commit();
    return SUCCEEDED(enc->Commit());
}

std::vector<uint32_t> read_png(const std::filesystem::path& file, UINT& w, UINT& h) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto src = wb::ui::decode_image(bytes.data(), bytes.size());
    if (!src) return {};
    (*src)->GetSize(&w, &h);
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    (*src)->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data()));
    return px;
}

// Compares with the reference image: at most 0.5 % of pixels may differ by more than 64 per channel (anti-aliasing
// and ClearType vary a little between Windows builds). SKOWORK_UPDATE_GOLDEN=1 rewrites the reference instead.
void expect_golden(const std::string& name, const std::vector<uint32_t>& px, UINT w, UINT h) {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC; the thread may have COM already
    struct ComEnd {
        bool on;
        ~ComEnd() {
            if (on) ::CoUninitialize();
        }
    } com_end{SUCCEEDED(com)};
    const auto file = golden_dir() / (name + ".png");
    const char* update = std::getenv("SKOWORK_UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        std::filesystem::create_directories(golden_dir());
        ASSERT_TRUE(write_png(file, px, w, h)) << file;
        return;
    }
    UINT gw = 0, gh = 0;
    const auto ref = read_png(file, gw, gh);
    ASSERT_FALSE(ref.empty()) << "missing reference " << file << " (run with SKOWORK_UPDATE_GOLDEN=1)";
    ASSERT_EQ(gw, w);
    ASSERT_EQ(gh, h);
    size_t bad = 0;
    for (size_t i = 0; i < px.size(); ++i)
        for (int s = 0; s < 24; s += 8)
            if (std::abs(static_cast<int>((px[i] >> s) & 0xFF) - static_cast<int>((ref[i] >> s) & 0xFF)) > 64) {
                ++bad;
                break;
            }
    const double frac = static_cast<double>(bad) / static_cast<double>(px.size());
    if (frac > 0.005) {
        write_png(std::filesystem::temp_directory_path() / (name + ".actual.png"), px, w, h);
        FAIL() << name << ": " << bad << " pixels differ (" << frac * 100 << " %); actual image in %TEMP%";
    }
}

bool reference_fonts() {
    // The references were drawn with Segoe UI Variable (Windows 11); other font sets draw different glyphs.
    const bool com = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    BOOL a = FALSE, b = FALSE;
    {
        auto r = wb::ui::Renderer::offscreen(1, 1, 96);
        if (r) {
            com_ptr<IDWriteFontCollection> c;
            (*r)->dwrite()->GetSystemFontCollection(c.put());
            UINT32 i = 0;
            c->FindFamilyName(L"Segoe UI Variable Text", &i, &a);
            c->FindFamilyName(L"Segoe Fluent Icons", &i, &b);
        }
    }
    if (com) ::CoUninitialize();
    return a && b;
}

}  // namespace