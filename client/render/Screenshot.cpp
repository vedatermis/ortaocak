#include "client/render/Screenshot.h"

#include "client/platform/Win32Util.h"

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace vo::client::render {

bool writePng(const std::filesystem::path& path, const CapturedImage& image, std::string& error)
{
    using vo::client::platform::hresultToString;

    if (image.width == 0 || image.height == 0 || image.bgra.size() < std::size_t{image.rowPitch} * image.height)
    {
        error = "empty or inconsistent image";
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        error = "WIC factory: " + hresultToString(hr);
        return false;
    }

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;

    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr))
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr))
        hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr))
        hr = frame->SetSize(image.width, image.height);

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr))
        hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA)
        hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;

    if (SUCCEEDED(hr))
    {
        // WritePixels takes a non-const buffer pointer but does not modify it.
        auto* pixels = const_cast<BYTE*>(image.bgra.data());
        hr = frame->WritePixels(image.height, image.rowPitch, static_cast<UINT>(image.bgra.size()), pixels);
    }
    if (SUCCEEDED(hr))
        hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();

    if (FAILED(hr))
    {
        error = "PNG encode failed: " + hresultToString(hr);
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        std::filesystem::remove(path, ec); // don't leave a truncated file behind
        return false;
    }
    return true;
}

} // namespace vo::client::render
