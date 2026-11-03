#include "ui/device.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace wb::ui {
namespace {

Error hr_error(const char* what, HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return make_error("ui_device", std::string(what) + " failed (" + buf + ")");
}

bool device_lost(HRESULT hr) {
    return hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

bool overlaps(const RECT& a, const RECT& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

// Adds r to the list, merging every rectangle it overlaps.
void add_merged(std::vector<RECT>& list, RECT r) {
    for (bool merged = true; merged;) {
        merged = false;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (overlaps(*it, r)) {
                r = {std::min(r.left, it->left), std::min(r.top, it->top), std::max(r.right, it->right), std::max(r.bottom, it->bottom)};
                list.erase(it);
                merged = true;
                break;
            }
        }
    }
    list.push_back(r);
}

}  // namespace

Renderer::~Renderer() { release_device(); }

Result<std::unique_ptr<Renderer>> Renderer::for_window(HWND hwnd, UINT dpi) {
    std::unique_ptr<Renderer> r(new Renderer());
    r->hwnd_ = hwnd;
    r->dpi_ = dpi ? dpi : 96;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    r->w_ = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    r->h_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));
    if (Status s = r->create_device(); !s) return s.error();
    return r;
}

Result<std::unique_ptr<Renderer>> Renderer::offscreen(UINT width_px, UINT height_px, UINT dpi) {
    std::unique_ptr<Renderer> r(new Renderer());
    r->dpi_ = dpi ? dpi : 96;
    r->w_ = std::max(1u, width_px);
    r->h_ = std::max(1u, height_px);
    if (Status s = r->create_device(); !s) return s.error();
    return r;
}

Status Renderer::create_device() {
    if (!factory_) {
        D2D1_FACTORY_OPTIONS fo{};
        HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, factory_.put_void());
        if (FAILED(hr)) return hr_error("D2D1CreateFactory", hr);
        hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
        if (FAILED(hr)) return hr_error("DWriteCreateFactory", hr);
    }
    // Software rendering only (design D1): WARP, no GPU driver is loaded.
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, d3d_.put(), nullptr, nullptr);
    if (FAILED(hr)) return hr_error("D3D11CreateDevice(WARP)", hr);
    com_ptr<IDXGIDevice1> dxgi;
    if (FAILED(hr = d3d_.as(dxgi, __uuidof(IDXGIDevice1)))) return hr_error("IDXGIDevice1", hr);
    dxgi->SetMaximumFrameLatency(1);
    if (hwnd_) {
        com_ptr<IDXGIAdapter> adapter;
        if (FAILED(hr = dxgi->GetAdapter(adapter.put()))) return hr_error("GetAdapter", hr);
        com_ptr<IDXGIFactory2> f;
        if (FAILED(hr = adapter->GetParent(__uuidof(IDXGIFactory2), f.put_void()))) return hr_error("IDXGIFactory2", hr);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w_;
        sd.Height = h_;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(hr = f->CreateSwapChainForHwnd(d3d_.get(), hwnd_, &sd, nullptr, nullptr, swap_.put())))
            return hr_error("CreateSwapChainForHwnd", hr);
        f->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    }
    if (FAILED(hr = factory_->CreateDevice(dxgi.get(), d2d_.put()))) return hr_error("ID2D1Factory1::CreateDevice", hr);
    if (FAILED(hr = d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.put())))
        return hr_error("CreateDeviceContext", hr);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    static std::atomic<uint64_t> next_device_id{0};
    device_id_ = ++next_device_id;
    ++generation_;
    return create_target();
}

Status Renderer::create_target() {
    dc_->SetTarget(nullptr);
    target_.reset();
    const float fdpi = static_cast<float>(dpi_);
    HRESULT hr;
    if (swap_) {
        com_ptr<IDXGISurface> surface;
        if (FAILED(hr = swap_->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()))) return hr_error("GetBuffer", hr);
        D2D1_BITMAP_PROPERTIES1 p{};
        p.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE};
        p.dpiX = p.dpiY = fdpi;
        p.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if (FAILED(hr = dc_->CreateBitmapFromDxgiSurface(surface.get(), &p, target_.put())))
            return hr_error("CreateBitmapFromDxgiSurface", hr);
    } else {
        D2D1_BITMAP_PROPERTIES1 p{};
        p.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        p.dpiX = p.dpiY = fdpi;
        p.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
        if (FAILED(hr = dc_->CreateBitmap(D2D1_SIZE_U{w_, h_}, nullptr, 0, &p, target_.put())))
            return hr_error("CreateBitmap(target)", hr);
    }
    dc_->SetTarget(target_.get());
    dc_->SetDpi(fdpi, fdpi);
    full_next_ = true;
    prev_px_.clear();
    return {};
}

void Renderer::release_device() {
    brushes_.clear();
    if (dc_) {
        dc_->SetTarget(nullptr);
        dc_->Flush();
    }
    target_.reset();
    dc_.reset();
    d2d_.reset();
    swap_.reset();
    if (d3d_) {
        com_ptr<ID3D11DeviceContext> ctx;
        d3d_->GetImmediateContext(ctx.put());
        if (ctx) {
            ctx->ClearState();
            ctx->Flush();
        }
    }
    d3d_.reset();
}

std::vector<RECT> Renderer::to_pixels(const std::vector<Rect>& dips) const {
    std::vector<RECT> out;
    const float s = scale();
    for (const Rect& d : dips) {
        if (d.empty()) continue;
        RECT r{static_cast<LONG>(std::floor(d.x * s)), static_cast<LONG>(std::floor(d.y * s)),
               static_cast<LONG>(std::ceil(d.right() * s)), static_cast<LONG>(std::ceil(d.bottom() * s))};
        r.left = std::clamp<LONG>(r.left, 0, static_cast<LONG>(w_));
        r.top = std::clamp<LONG>(r.top, 0, static_cast<LONG>(h_));
        r.right = std::clamp<LONG>(r.right, 0, static_cast<LONG>(w_));
        r.bottom = std::clamp<LONG>(r.bottom, 0, static_cast<LONG>(h_));
        if (r.right > r.left && r.bottom > r.top) add_merged(out, r);
    }
    return out;
}

std::vector<Rect> Renderer::begin_frame(const std::vector<Rect>& dirty) {
    frame_px_.clear();
    std::vector<RECT> draw;
    if (full_next_) {
        draw.push_back({0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_)});
    } else {
        frame_px_ = to_pixels(dirty);
        if (frame_px_.empty()) return {};
        draw = frame_px_;
        if (swap_)  // flip-sequential: the back buffer is two frames old
            for (const RECT& p : prev_px_) add_merged(draw, p);
    }
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::IdentityMatrix());
    in_frame_ = true;
    std::vector<Rect> out;
    const float s = scale();
    for (const RECT& p : draw)
        out.push_back({static_cast<float>(p.left) / s, static_cast<float>(p.top) / s, static_cast<float>(p.right - p.left) / s,
                       static_cast<float>(p.bottom - p.top) / s});
    return out;
}

Result<Renderer::FrameEnd> Renderer::end_frame() {
    if (!in_frame_) return FrameEnd::presented;
    in_frame_ = false;
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr) && simulate_loss_) {
        simulate_loss_ = false;
        hr = D2DERR_RECREATE_TARGET;
    }
    if (SUCCEEDED(hr) && swap_) {
        last_present_.clear();
        if (full_next_) {
            hr = swap_->Present(1, 0);
        } else {
            last_present_ = frame_px_;
            DXGI_PRESENT_PARAMETERS pp{};
            pp.DirtyRectsCount = static_cast<UINT>(last_present_.size());
            pp.pDirtyRects = last_present_.data();
            hr = swap_->Present1(1, 0, &pp);
        }
    }
    if (device_lost(hr)) {
        release_device();
        if (Status s = create_device(); !s) return s.error();
        return FrameEnd::recreated;
    }
    if (FAILED(hr)) return hr_error("EndDraw/Present", hr);
    prev_px_ = full_next_ ? std::vector<RECT>{{0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_)}} : frame_px_;
    full_next_ = false;
    return FrameEnd::presented;
}

Status Renderer::resize(UINT width_px, UINT height_px) {
    width_px = std::max(1u, width_px);
    height_px = std::max(1u, height_px);
    if (width_px == w_ && height_px == h_) return {};
    w_ = width_px;
    h_ = height_px;
    dc_->SetTarget(nullptr);
    target_.reset();
    if (swap_) {
        const HRESULT hr = swap_->ResizeBuffers(0, w_, h_, DXGI_FORMAT_UNKNOWN, 0);
        if (device_lost(hr)) {
            release_device();
            return create_device();
        }
        if (FAILED(hr)) return hr_error("ResizeBuffers", hr);
    }
    return create_target();
}

Status Renderer::set_dpi(UINT dpi) {
    if (!dpi || dpi == dpi_) return {};
    dpi_ = dpi;
    const float f = static_cast<float>(dpi_);
    if (target_) {
        // The target bitmap's DPI is fixed at creation; recreate it so DIPs map to the new scale.
        if (Status s = create_target(); !s) return s;
    }
    dc_->SetDpi(f, f);
    full_next_ = true;
    return {};
}

Result<std::vector<uint32_t>> Renderer::read_pixels() {
    if (swap_) return make_error("ui_device", "read_pixels is for off-screen renderers");
    D2D1_BITMAP_PROPERTIES1 p{};
    p.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    p.dpiX = p.dpiY = static_cast<float>(dpi_);
    p.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    com_ptr<ID2D1Bitmap1> staging;
    HRESULT hr = dc_->CreateBitmap(D2D1_SIZE_U{w_, h_}, nullptr, 0, &p, staging.put());
    if (FAILED(hr)) return hr_error("CreateBitmap(staging)", hr);
    if (FAILED(hr = staging->CopyFromBitmap(nullptr, target_.get(), nullptr))) return hr_error("CopyFromBitmap", hr);
    D2D1_MAPPED_RECT m{};
    if (FAILED(hr = staging->Map(D2D1_MAP_OPTIONS_READ, &m))) return hr_error("Map", hr);
    std::vector<uint32_t> px(static_cast<size_t>(w_) * h_);
    for (UINT y = 0; y < h_; ++y)
        std::memcpy(px.data() + static_cast<size_t>(y) * w_, m.bits + static_cast<size_t>(y) * m.pitch, static_cast<size_t>(w_) * 4);
    staging->Unmap();
    return px;
}

}  // namespace wb::ui
