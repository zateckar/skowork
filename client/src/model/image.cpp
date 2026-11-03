#include "model/image.h"

#include "platform/com.h"

#include <windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <cmath>

namespace wb::model {
namespace {

// COM for the calling thread (multi-threaded apartment). S_FALSE means "already initialised", which is fine; a thread
// that already runs in another apartment keeps it.
struct ComScope {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
};

struct Frame {
    com_ptr<IWICImagingFactory> factory;
    com_ptr<IWICBitmapSource> source;  // 24bpp BGR at the target size
    UINT width = 0, height = 0;
};

Result<std::vector<std::uint8_t>> encode(const Frame& f, const GUID& container, double jpeg_quality) {
    com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(f.factory->CreateEncoder(container, nullptr, encoder.put()))) return make_error("encode", "no encoder");
    IStream* raw = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &raw))) return make_error("encode", "no memory stream");
    com_ptr<IStream> stream(raw, false);
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return make_error("encode", "encoder init");
    com_ptr<IWICBitmapFrameEncode> frame;
    com_ptr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.put(), props.put()))) return make_error("encode", "no frame");
    if (container == GUID_ContainerFormatJpeg && props) {
        PROPBAG2 option{};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT value;
        ::VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(jpeg_quality);
        props->Write(1, &option, &value);
    }
    if (FAILED(frame->Initialize(props.get()))) return make_error("encode", "frame init");
    if (FAILED(frame->SetSize(f.width, f.height))) return make_error("encode", "size");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) return make_error("encode", "pixel format");
    if (FAILED(frame->WriteSource(f.source.get(), nullptr))) return make_error("encode", "write");
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return make_error("encode", "commit");

    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return make_error("encode", "stat");
    std::vector<std::uint8_t> out(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    if (FAILED(stream->Read(out.data(), static_cast<ULONG>(out.size()), &got))) return make_error("encode", "read back");
    out.resize(got);
    return out;
}

Result<Frame> scaled_frame(const com_ptr<IWICImagingFactory>& factory, IWICBitmapSource* decoded, UINT w, UINT h) {
    Frame f;
    f.factory = factory;
    f.width = w;
    f.height = h;
    com_ptr<IWICBitmapSource> current(decoded);
    UINT cw = 0, ch = 0;
    decoded->GetSize(&cw, &ch);
    if (cw != w || ch != h) {
        com_ptr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(scaler.put())) ||
            FAILED(scaler->Initialize(decoded, w, h, WICBitmapInterpolationModeFant)))
            return make_error("encode", "scaler");
        current = com_ptr<IWICBitmapSource>(scaler.get());
    }
    com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(current.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return make_error("encode", "format converter");
    f.source = com_ptr<IWICBitmapSource>(converter.get());
    return f;
}

}  // namespace

std::pair<int, int> fit_within(int width, int height, int max_side) {
    if (width <= 0 || height <= 0) return {std::max(width, 1), std::max(height, 1)};
    const int longest = std::max(width, height);
    if (longest <= max_side) return {width, height};
    const double scale = static_cast<double>(max_side) / longest;
    return {std::max(1, static_cast<int>(std::lround(width * scale))), std::max(1, static_cast<int>(std::lround(height * scale)))};
}

Result<PreparedImage> prepare_image(const std::vector<std::uint8_t>& data, const ImageLimits& limits) {
    ComScope com;
    if (data.empty()) return make_error("decode", "empty image");
    com_ptr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                  factory.put_void())))
        return make_error("decode", "the imaging component is not available");
    com_ptr<IWICStream> in;
    if (FAILED(factory->CreateStream(in.put())) ||
        FAILED(in->InitializeFromMemory(const_cast<BYTE*>(data.data()), static_cast<DWORD>(data.size()))))
        return make_error("decode", "cannot read the image");
    com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(in.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())))
        return make_error("decode", "unsupported image format");
    com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return make_error("decode", "no image frame");
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return make_error("decode", "empty image");

    GUID container{};
    decoder->GetContainerFormat(&container);
    const bool is_png = container == GUID_ContainerFormatPng;
    const bool is_jpeg = container == GUID_ContainerFormatJpeg;
    const auto [tw0, th0] = fit_within(static_cast<int>(w), static_cast<int>(h), limits.max_side);

    PreparedImage out;
    if ((is_png || is_jpeg) && tw0 == static_cast<int>(w) && th0 == static_cast<int>(h) && data.size() <= limits.max_bytes) {
        out.mime = is_png ? "image/png" : "image/jpeg";
        out.bytes = data;
        out.width = static_cast<int>(w);
        out.height = static_cast<int>(h);
        return out;
    }

    int tw = tw0, th = th0;
    for (int shrink = 0; shrink < 6; ++shrink) {
        auto f = scaled_frame(factory, frame.get(), static_cast<UINT>(tw), static_cast<UINT>(th));
        if (!f) return f.error();
        auto png = encode(*f, GUID_ContainerFormatPng, 0);
        if (!png) return png.error();
        if (png->size() <= limits.max_bytes) {
            out = {"image/png", std::move(*png), tw, th, true};
            return out;
        }
        for (const double quality : {0.85, 0.7, 0.55}) {
            auto jpeg = encode(*f, GUID_ContainerFormatJpeg, quality);
            if (!jpeg) return jpeg.error();
            if (jpeg->size() <= limits.max_bytes) {
                out = {"image/jpeg", std::move(*jpeg), tw, th, true};
                return out;
            }
        }
        tw = std::max(1, tw * 3 / 4);  // still too big: scale further down and try again
        th = std::max(1, th * 3 / 4);
    }
    return make_error("too_large", "the image cannot be reduced to the size limit");
}

}  // namespace wb::model
