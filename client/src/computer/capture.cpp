#include "computer/capture.h"

#include "platform/com.h"

// MinGW-w64: windows.foundation.h specialises IReference<BYTE> and IReference<boolean>, both `unsigned char`; skipping
// the boolean one avoids the redefinition (as in host/toast.cpp).
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <inspectable.h>
#include <roapi.h>
#include <wincodec.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>

#include <chrono>
#include <cstring>
#include <thread>

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice* dxgi, IInspectable** device);

namespace wb::computer {

// IDirect3DDxgiInterfaceAccess {A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1} (windows.graphics.directx.direct3d11.interop.h,
// which MinGW does not ship). Outside the anonymous namespace on purpose: with internal linkage GCC sees no class that
// implements it, devirtualises GetInterface at -O2 to the pure declaration and calls an address that does not exist.
struct IDirect3DDxgiInterfaceAccess : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};

namespace {

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wd3d = ABI::Windows::Graphics::DirectX::Direct3D11;

constexpr IID kIID_DxgiAccess = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};

struct Mta {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Mta() {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
};

template <typename T>
HRESULT factory(const wchar_t* cls, const IID& iid, com_ptr<T>& out) {
    HSTRING_HEADER header;
    HSTRING name = nullptr;
    if (FAILED(::WindowsCreateStringReference(cls, static_cast<UINT32>(wcslen(cls)), &header, &name))) return E_FAIL;
    return ::RoGetActivationFactory(name, iid, out.put_void());
}

Box frame_of(HWND hwnd) {
    RECT r{};
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) ::GetWindowRect(hwnd, &r);
    return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

void close(IUnknown* o) {
    com_ptr<ABI::Windows::Foundation::IClosable> c;
    if (o && SUCCEEDED(o->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable), c.put_void())) && c) c->Close();
}

Result<Bitmap> graphics_capture(HWND hwnd) {
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID3D11DeviceContext> ctx;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put())) &&
        FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put())))
        return make_error("capture", "no Direct3D device");
    com_ptr<IDXGIDevice> dxgi;
    if (FAILED(d3d.as(dxgi, __uuidof(IDXGIDevice)))) return make_error("capture", "no DXGI device");
    com_ptr<IInspectable> insp;
    if (FAILED(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()))) return make_error("capture", "no WinRT Direct3D device");
    com_ptr<wd3d::IDirect3DDevice> device;
    if (FAILED(insp.as(device, __uuidof(wd3d::IDirect3DDevice)))) return make_error("capture", "no WinRT Direct3D device");

    com_ptr<IGraphicsCaptureItemInterop> interop;
    if (FAILED(factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", __uuidof(IGraphicsCaptureItemInterop), interop)))
        return make_error("capture", "Windows.Graphics.Capture is not available");
    com_ptr<wgc::IGraphicsCaptureItem> item;
    if (FAILED(interop->CreateForWindow(hwnd, __uuidof(wgc::IGraphicsCaptureItem), item.put_void())) || !item)
        return make_error("capture", "the window cannot be captured");
    ABI::Windows::Graphics::SizeInt32 size{};
    item->get_Size(&size);
    if (size.Width <= 0 || size.Height <= 0) return make_error("capture", "the window has no size");

    com_ptr<wgc::IDirect3D11CaptureFramePoolStatics2> statics;
    if (FAILED(factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", __uuidof(wgc::IDirect3D11CaptureFramePoolStatics2), statics)))
        return make_error("capture", "no capture frame pool");
    com_ptr<wgc::IDirect3D11CaptureFramePool> pool;
    if (FAILED(statics->CreateFreeThreaded(device.get(), ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 1, size,
                                           pool.put())))
        return make_error("capture", "no capture frame pool");
    com_ptr<wgc::IGraphicsCaptureSession> session;
    if (FAILED(pool->CreateCaptureSession(item.get(), session.put()))) {
        close(pool.get());
        return make_error("capture", "no capture session");
    }
    com_ptr<wgc::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(session.as(session2, __uuidof(wgc::IGraphicsCaptureSession2)))) session2->put_IsCursorCaptureEnabled(false);
    const auto done = [&] {
        close(session.get());
        close(pool.get());
    };
    if (FAILED(session->StartCapture())) {
        done();
        return make_error("capture", "the capture did not start");
    }
    com_ptr<wgc::IDirect3D11CaptureFrame> frame;
    for (int i = 0; i < 100 && !frame; ++i) {
        if (FAILED(pool->TryGetNextFrame(frame.put())) || !frame) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!frame) {
        done();
        return make_error("capture", "no frame within a second");
    }
    com_ptr<wd3d::IDirect3DSurface> surface;
    frame->get_Surface(surface.put());
    com_ptr<IDirect3DDxgiInterfaceAccess> access;
    com_ptr<ID3D11Texture2D> texture;
    if (!surface || FAILED(surface->QueryInterface(kIID_DxgiAccess, access.put_void())) ||
        FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()))) {
        close(frame.get());
        done();
        return make_error("capture", "the frame cannot be read");
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    com_ptr<ID3D11Texture2D> staging;
    if (FAILED(d3d->CreateTexture2D(&desc, nullptr, staging.put()))) {
        close(frame.get());
        done();
        return make_error("capture", "no staging texture");
    }
    ctx->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map))) {
        close(frame.get());
        done();
        return make_error("capture", "the frame cannot be mapped");
    }
    Bitmap b;
    b.width = static_cast<int>(desc.Width);
    b.height = static_cast<int>(desc.Height);
    b.bgra.resize(static_cast<size_t>(b.width) * b.height * 4);
    for (int y = 0; y < b.height; ++y)
        std::memcpy(b.bgra.data() + static_cast<size_t>(y) * b.width * 4, static_cast<const std::uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    static_cast<size_t>(b.width) * 4);
    ctx->Unmap(staging.get(), 0);
    close(frame.get());
    done();
    b.box = frame_of(hwnd);
    b.method = "graphics-capture";
    return b;
}

Result<Bitmap> from_dc(HDC src, int sx, int sy, int w, int h, bool print_window, HWND hwnd) {
    HDC mem = ::CreateCompatibleDC(src);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem || !dib) {
        if (dib) ::DeleteObject(dib);
        if (mem) ::DeleteDC(mem);
        return make_error("capture", "no bitmap");
    }
    HGDIOBJ old = ::SelectObject(mem, dib);
    const bool ok = print_window ? ::PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT) != FALSE
                                 : ::BitBlt(mem, 0, 0, w, h, src, sx, sy, SRCCOPY | CAPTUREBLT) != FALSE;
    Bitmap b;
    if (ok) {
        b.width = w;
        b.height = h;
        b.bgra.assign(static_cast<const std::uint8_t*>(bits), static_cast<const std::uint8_t*>(bits) + static_cast<size_t>(w) * h * 4);
        for (size_t i = 3; i < b.bgra.size(); i += 4) b.bgra[i] = 255;  // GDI leaves alpha undefined
    }
    ::SelectObject(mem, old);
    ::DeleteObject(dib);
    ::DeleteDC(mem);
    if (!ok) return make_error("capture", "the window could not be drawn");
    return b;
}

}  // namespace

Result<Bitmap> capture_window(HWND hwnd) {
    if (!::IsWindow(hwnd)) return make_error("window_gone", "the window no longer exists");
    Result<Bitmap> wgc_result = make_error("capture", "not tried");
    {
        Mta mta;
        wgc_result = graphics_capture(hwnd);
    }
    if (wgc_result) return wgc_result;
    // PrintWindow with full content draws DirectX content too; it covers the whole window rectangle.
    RECT r{};
    ::GetWindowRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return make_error("capture", "the window has no size");
    HDC screen = ::GetDC(nullptr);
    auto b = from_dc(screen, 0, 0, w, h, true, hwnd);
    ::ReleaseDC(nullptr, screen);
    if (!b) return make_error("capture", "the window could not be captured: " + wgc_result.error().message);
    b->box = {r.left, r.top, w, h};
    b->method = "print-window";
    return b;
}

Result<Bitmap> capture_region(const Box& box) {
    if (box.empty()) return make_error("capture", "the region is empty");
    HDC screen = ::GetDC(nullptr);
    auto b = from_dc(screen, box.x, box.y, box.w, box.h, false, nullptr);
    ::ReleaseDC(nullptr, screen);
    if (!b) return b;
    b->box = box;
    b->method = "screen";
    return b;
}

Result<std::vector<std::uint8_t>> encode_png(const Bitmap& b) {
    Mta mta;
    com_ptr<IWICImagingFactory> f;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, f.put_void())))
        return make_error("encode", "the imaging component is not available");
    com_ptr<IWICBitmap> bmp;
    if (FAILED(f->CreateBitmapFromMemory(static_cast<UINT>(b.width), static_cast<UINT>(b.height), GUID_WICPixelFormat32bppBGRA,
                                         static_cast<UINT>(b.width) * 4, static_cast<UINT>(b.bgra.size()), const_cast<BYTE*>(b.bgra.data()),
                                         bmp.put())))
        return make_error("encode", "no bitmap");
    IStream* raw = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &raw))) return make_error("encode", "no memory stream");
    com_ptr<IStream> stream(raw, false);
    com_ptr<IWICBitmapEncoder> enc;
    com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(b.width), static_cast<UINT>(b.height))))
        return make_error("encode", "no PNG encoder");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;  // screenshots need no alpha
    frame->SetPixelFormat(&fmt);
    com_ptr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(bmp.get(), fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(conv.get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return make_error("encode", "PNG encoding failed");
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    std::vector<std::uint8_t> out(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    stream->Read(out.data(), static_cast<ULONG>(out.size()), &got);
    out.resize(got);
    return out;
}

}  // namespace wb::computer
