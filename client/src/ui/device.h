// The drawing surface of the native window (add-native-ui design D1): Direct2D 1.1 on a Direct3D 11 WARP device
// (software rendering, no GPU driver loaded) with a DXGI flip-sequential swap chain, or an off-screen target for
// golden-image tests.
//
// Frames are drawn in dirty rectangles. With flip-sequential the back buffer holds the frame before the previous
// one, so each frame redraws the union of its own dirty rectangles and the previous frame's, then presents with the
// dirty rectangles as a hint to the compositor. After creation, a resize, a DPI change or device recreation the
// next frame is drawn and presented in full.
//
// Device loss (D2DERR_RECREATE_TARGET, DXGI_ERROR_DEVICE_REMOVED/RESET) recreates the device and increments
// generation(). The renderer's brushes go with the old device; elements drop their own device resources (bitmaps)
// when device_id() changes.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "base/result.h"
#include "platform/com.h"
#include "ui/layout.h"

namespace wb::ui {

class Renderer {
public:
    static Result<std::unique_ptr<Renderer>> for_window(HWND hwnd, UINT dpi);
    static Result<std::unique_ptr<Renderer>> offscreen(UINT width_px, UINT height_px, UINT dpi);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    ID2D1DeviceContext* dc() const { return dc_.get(); }
    ID2D1Factory1* factory() const { return factory_.get(); }
    IDWriteFactory* dwrite() const { return dwrite_.get(); }
    ID3D11Device* d3d() const { return d3d_.get(); }
    uint64_t generation() const { return generation_; }
    // Unique in the process for each device ever created; keys caches of device resources.
    uint64_t device_id() const { return device_id_; }
    UINT dpi() const { return dpi_; }
    float scale() const { return static_cast<float>(dpi_) / 96.f; }
    UINT width_px() const { return w_; }
    UINT height_px() const { return h_; }
    Size size_dip() const { return {static_cast<float>(w_) / scale(), static_cast<float>(h_) / scale()}; }

    // Starts a frame. `dirty` is in DIPs; the result is the list of regions (DIPs) the caller must redraw, each
    // inside a clip it pushes itself. An empty result means nothing to draw (end_frame is then not needed).
    std::vector<Rect> begin_frame(const std::vector<Rect>& dirty);
    enum class FrameEnd { presented, recreated };
    // Ends the frame and presents it. `recreated` means the device was lost and rebuilt: drop device resources and
    // draw the next frame in full.
    Result<FrameEnd> end_frame();

    Status resize(UINT width_px, UINT height_px);
    Status set_dpi(UINT dpi);  // the next frame is full; the caller re-lays out in DIPs

    // Off-screen only: the last frame as BGRA pixels (premultiplied), row by row.
    Result<std::vector<uint32_t>> read_pixels();

    // Makes the next end_frame() behave as if the device had been removed (tests of the recovery path).
    void simulate_device_loss_for_test() { simulate_loss_ = true; }
    // The pixel rectangles passed to the last Present1 (empty for a full present); for tests.
    const std::vector<RECT>& last_present_rects() const { return last_present_; }

    // Solid brushes of this device by colour key, dropped with the device. They live here, not in a thread-local
    // cache: a thread-local cache would release the device's last references while its thread exits, under the
    // loader lock, where the WARP device cannot shut down its worker threads.
    std::map<uint64_t, com_ptr<ID2D1SolidColorBrush>>& brushes() { return brushes_; }

private:
    Renderer() = default;
    Status create_device();
    Status create_target();
    void release_device();
    std::vector<RECT> to_pixels(const std::vector<Rect>& dips) const;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96, w_ = 0, h_ = 0;
    uint64_t generation_ = 0;
    uint64_t device_id_ = 0;
    bool full_next_ = true;
    bool in_frame_ = false;
    bool simulate_loss_ = false;
    std::vector<RECT> frame_px_, prev_px_, last_present_;

    com_ptr<ID2D1Factory1> factory_;
    com_ptr<IDWriteFactory> dwrite_;
    com_ptr<ID3D11Device> d3d_;
    com_ptr<IDXGISwapChain1> swap_;
    com_ptr<ID2D1Device> d2d_;
    com_ptr<ID2D1DeviceContext> dc_;
    com_ptr<ID2D1Bitmap1> target_;
    std::map<uint64_t, com_ptr<ID2D1SolidColorBrush>> brushes_;
};

// Converts 0xRRGGBB to a Direct2D colour.
inline D2D1_COLOR_F color(uint32_t rgb, float alpha = 1.f) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.f, static_cast<float>((rgb >> 8) & 0xFF) / 255.f,
            static_cast<float>(rgb & 0xFF) / 255.f, alpha};
}

}  // namespace wb::ui
