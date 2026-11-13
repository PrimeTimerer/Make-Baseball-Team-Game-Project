// PNG loading for texture replacement with the Windows Imaging Component instead of libpng (included by the generated
// compat/GSTextureReplacementLoaders.cpp in place of PCSX2's libpng handlers; tools/make_loaders.py).
// Same output as PCSX2's PNGLoader: 32-bit RGBA rows; images without alpha get A = 0x80 (opaque in GS terms), images
// with alpha keep it as stored. PCSX2 is GPL-3; so is this file.

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    // One factory per thread (texture loads run on the GS thread and on PCSX2's replacement worker thread).
    IWICImagingFactory *WicFactory()
    {
        thread_local Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        thread_local bool tried = false;
        if (!tried)
        {
            tried = true;
            const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            (void)init; // S_FALSE / RPC_E_CHANGED_MODE: COM is already usable on this thread
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        }
        return factory.Get();
    }

    bool WicHasAlpha(IWICImagingFactory *factory, const WICPixelFormatGUID &format)
    {
        Microsoft::WRL::ComPtr<IWICComponentInfo> info;
        Microsoft::WRL::ComPtr<IWICPixelFormatInfo2> pixelInfo;
        BOOL transparency = FALSE;
        if (FAILED(factory->CreateComponentInfo(format, info.GetAddressOf())) || FAILED(info.As(&pixelInfo)) ||
            FAILED(pixelInfo->SupportsTransparency(&transparency)))
            return true;
        return transparency != FALSE;
    }
}

bool PNGLoader(GSTextureReplacements::File& file, const char* filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
    IWICImagingFactory *factory = WicFactory();
    const s64 size = file.Size();
    if (!factory || size <= 0 || size > 0x7FFFFFFF || !file.Seek(0))
        return false;
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!file.Read(bytes.data(), bytes.size()))
        return false;

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    WICPixelFormatGUID sourceFormat{};
    UINT width = 0, height = 0;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(frame->GetSize(&width, &height)) || width == 0 ||
        height == 0 || FAILED(frame->GetPixelFormat(&sourceFormat)) ||
        FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
    {
        Console.Error("Failed to decode replacement texture %s", filename);
        return false;
    }

    const u32 pitch = width * sizeof(u32);
    tex->width = width;
    tex->height = height;
    tex->format = GSTexture::Format::Color;
    tex->pitch = pitch;
    tex->data.resize(static_cast<size_t>(pitch) * height);
    if (FAILED(converter->CopyPixels(nullptr, pitch, static_cast<UINT>(tex->data.size()), tex->data.data())))
        return false;

    if (!WicHasAlpha(factory, sourceFormat))
    {
        u8 *pixel = tex->data.data();
        for (size_t i = 0, count = static_cast<size_t>(width) * height; i < count; ++i, pixel += 4)
            pixel[3] = 0x80;
    }
    return true;
}

// Texture dumping (texture_dump=1): 32-bit RGBA rows as the GS reads them (A = 0x80 is opaque), written as a PNG with
// alpha, so an edited copy loads back through PNGLoader above the same way.
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
    IWICImagingFactory *factory = WicFactory();
    if (!factory || width == 0 || height == 0)
        return false;
    std::vector<u8> bgra(static_cast<size_t>(width) * height * 4);
    for (u32 y = 0; y < height; ++y)
    {
        const u8 *src = buffer + static_cast<size_t>(y) * pitch;
        u8 *dst = bgra.data() + static_cast<size_t>(y) * width * 4;
        for (u32 x = 0; x < width; ++x, src += 4, dst += 4)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, nullptr, 0);
    if (chars <= 0)
        return false;
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), -1, wide.data(), chars))
        return false;

    using Microsoft::WRL::ComPtr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(factory->CreateStream(stream.GetAddressOf())) &&
           SUCCEEDED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

