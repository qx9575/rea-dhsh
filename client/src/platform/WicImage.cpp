#include "platform/WicImage.h"

#include "resource/ResourceError.h"

#ifndef _WIN32
#error "WicImage requires Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace dhsh {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throw_hresult(const char* operation, HRESULT result) {
    std::ostringstream message;
    message << "WIC " << operation << " failed (HRESULT 0x"
        << std::hex << std::uppercase << static_cast<std::uint32_t>(result) << ')';
    throw FormatError(message.str());
}

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw_hresult(operation, result);
    }
}

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            return;
        }
        check_hresult(result, "COM initialization");
        initialized_ = true;
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

private:
    bool initialized_ = false;
};

} // namespace

Image WicImage::decode(std::span<const std::byte> data) {
    if (data.empty()) {
        throw FormatError("WIC cannot decode an empty image");
    }
    if (data.size() > std::numeric_limits<DWORD>::max()) {
        throw FormatError("WIC input is too large");
    }

    const ComApartment apartment;
    ComPtr<IWICImagingFactory> factory;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "factory creation");
    ComPtr<IWICStream> stream;
    check_hresult(factory->CreateStream(&stream), "stream creation");
    check_hresult(stream->InitializeFromMemory(
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data.data())),
        static_cast<DWORD>(data.size())), "stream initialization");
    ComPtr<IWICBitmapDecoder> decoder;
    check_hresult(factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder), "decoder creation");
    ComPtr<IWICBitmapFrameDecode> frame;
    check_hresult(decoder->GetFrame(0, &frame), "frame acquisition");

    UINT width = 0;
    UINT height = 0;
    check_hresult(frame->GetSize(&width, &height), "dimension query");
    if (width == 0 || height == 0) {
        throw FormatError("WIC decoded invalid zero dimensions");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > std::numeric_limits<UINT>::max() / 4ULL
        || pixels > std::numeric_limits<std::size_t>::max() / 4ULL
        || static_cast<std::uint64_t>(width) * 4 > std::numeric_limits<UINT>::max()) {
        throw FormatError("WIC dimensions overflow the RGBA buffer");
    }
    const UINT stride = width * 4;
    const UINT buffer_size = static_cast<UINT>(pixels * 4);

    ComPtr<IWICFormatConverter> converter;
    check_hresult(factory->CreateFormatConverter(&converter), "format-converter creation");
    check_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
        "RGBA conversion");

    Image image{width, height, std::vector<std::byte>(buffer_size)};
    check_hresult(converter->CopyPixels(nullptr, stride, buffer_size,
        reinterpret_cast<BYTE*>(image.rgba.data())), "pixel copy");
    return image;
}

} // namespace dhsh
