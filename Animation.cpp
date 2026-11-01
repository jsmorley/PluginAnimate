#include "Animation.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<BYTE>;
namespace
{
constexpr size_t MaxPixels = 16 * 1024 * 1024;
constexpr size_t MaxFrames = 10000;
constexpr uint64_t MaxOutputBytes = 1024ull * 1024 * 1024;
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Windows image codec operation failed"); }
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
size_t PixelBytes(UINT w, UINT h)
{
    Require(w && h && uint64_t(w) * h <= MaxPixels, "Invalid or excessive canvas dimensions");
    return size_t(w) * h * 4;
}
struct ComScope
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComScope() { if (hr != RPC_E_CHANGED_MODE) Check(hr); }
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
uint32_t Read32(const BYTE* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put32(Bytes& b, uint32_t n) { for (int s = 24; s >= 0; s -= 8) b.push_back(BYTE(n >> s)); }
uint32_t Crc(const BYTE* p, size_t n)
{
    uint32_t c = 0xffffffff;
    while (n--) { c ^= *p++; for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return c ^ 0xffffffff;
}
void Chunk(Bytes& out, const char* type, const Bytes& data)
{
    Put32(out, static_cast<uint32_t>(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Put32(out, Crc(out.data() + start, data.size() + 4));
}
Bytes Pixels(IWICImagingFactory* factory, IWICBitmapSource* source, UINT& w, UINT& h)
{
    Check(source->GetSize(&w, &h));
    Bytes pixels(PixelBytes(w, h));
    ComPtr<IWICFormatConverter> converter;
    Check(factory->CreateFormatConverter(&converter));
    Check(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    Check(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    return pixels;
}
struct Rect { UINT w = 0, h = 0, x = 0, y = 0; BYTE dispose = 0, blend = 0; };
void Bounds(const Rect& r, UINT w, UINT h)
{
    Require(r.w && r.h && r.x <= w && r.y <= h && r.w <= w - r.x && r.h <= h - r.y, "Frame lies outside animation canvas");
}
void Clear(Bytes& canvas, UINT width, const Rect& r, const std::array<BYTE, 4>& color = {})
{
    for (UINT y = 0; y < r.h; ++y)
        for (UINT x = 0; x < r.w; ++x)
            std::copy(color.begin(), color.end(), canvas.begin() + (size_t(y + r.y) * width + x + r.x) * 4);
}
void Composite(Bytes& canvas, UINT width, const Bytes& pixels, const Rect& r)
{
    for (UINT y = 0; y < r.h; ++y)
        for (UINT x = 0; x < r.w; ++x)
        {
            const BYTE* s = pixels.data() + (size_t(y) * r.w + x) * 4;
            BYTE* d = canvas.data() + (size_t(y + r.y) * width + x + r.x) * 4;
            for (int c = 0; c < 4; ++c)
                d[c] = r.blend ? BYTE(s[c] + (unsigned(d[c]) * (255 - s[3]) + 127) / 255) : s[c];
        }
}
void Save(IWICImagingFactory* factory, Animation& output, const Bytes& canvas, UINT w, UINT h)
{
    Require(output.frames.size() < MaxFrames && uint64_t(canvas.size()) * (output.frames.size() + 1) <= MaxOutputBytes,
        "Animation exceeds extraction limits (10000 frames / 1 GiB decoded pixels)");
    std::wstring path = output.directory + L"\\frame-" + std::to_wstring(output.frames.size()) + L".png";
    // Register before writing, so failed/partial writes are cleaned up too.
    output.frames.push_back(path);
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream));
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr));
    Check(frame->Initialize(nullptr));
    Check(frame->SetSize(w, h));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(frame->SetPixelFormat(&format));
    Require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "PNG encoder does not support BGRA");
    // PNG stores straight alpha; undo premultiplication used during compositing.
    Bytes straight(canvas);
    for (size_t i = 0; i < straight.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            straight[i + c] = straight[i + 3] ? BYTE((std::min)(255u, (unsigned(straight[i + c]) * 255 + straight[i + 3] / 2) / straight[i + 3])) : 0;
    Check(frame->WritePixels(h, w * 4, static_cast<UINT>(straight.size()), straight.data()));
    Check(frame->Commit());
    Check(encoder->Commit());
}
UINT Metadata(IWICMetadataQueryReader* reader, const wchar_t* name, UINT fallback = 0)
{
    PROPVARIANT value{};
    UINT result = fallback;
    if (SUCCEEDED(reader->GetMetadataByName(name, &value)))
    {
        if (value.vt == VT_UI1) result = value.bVal;
        else if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_BOOL) result = value.boolVal != VARIANT_FALSE;
    }
    PropVariantClear(&value);
    return result;
}
void Gif(IWICImagingFactory* factory, const std::wstring& path, Animation& output)
{
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICMetadataQueryReader> global;
    Check(decoder->GetMetadataQueryReader(&global));
    UINT w = Metadata(global.Get(), L"/logscrdesc/Width"), h = Metadata(global.Get(), L"/logscrdesc/Height");
    Bytes canvas(PixelBytes(w, h));
    UINT count;
    Check(decoder->GetFrameCount(&count));
    Require(count && count <= MaxFrames, "Invalid or excessive GIF frame count");
    std::array<BYTE, 4> background{};
    ComPtr<IWICPalette> palette;
    Check(factory->CreatePalette(&palette));
    if (SUCCEEDED(decoder->CopyPalette(palette.Get())))
    {
        WICColor colors[256]{}; UINT actual = 0;
        if (SUCCEEDED(palette->GetColors(256, colors, &actual)))
        {
            UINT index = Metadata(global.Get(), L"/logscrdesc/BackgroundColorIndex");
            if (index < actual) background = { BYTE(colors[index]), BYTE(colors[index] >> 8), BYTE(colors[index] >> 16), 255 };
        }
    }
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(i, &frame));
        ComPtr<IWICMetadataQueryReader> metadata;
        Check(frame->GetMetadataQueryReader(&metadata));
        Rect r;
        Bytes pixels = Pixels(factory, frame.Get(), r.w, r.h);
        r.x = Metadata(metadata.Get(), L"/imgdesc/Left");
        r.y = Metadata(metadata.Get(), L"/imgdesc/Top");
        r.dispose = BYTE(Metadata(metadata.Get(), L"/grctlext/Disposal"));
        r.blend = 1;
        Bounds(r, w, h);
        auto clearColor = Metadata(metadata.Get(), L"/grctlext/TransparencyFlag") ? std::array<BYTE, 4>{} : background;
        if (!i) Clear(canvas, w, Rect{w, h}, clearColor);
        Bytes previous;
        if (r.dispose == 3) previous = canvas;
        Composite(canvas, w, pixels, r);
        Save(factory, output, canvas, w, h);
        // GIF delays are hundredths of a second. Missing metadata defaults to 100 ms.
        output.delays.push_back((std::max)(UINT(USER_TIMER_MINIMUM), Metadata(metadata.Get(), L"/grctlext/Delay", 10) * 10));
        if (r.dispose == 2) Clear(canvas, w, r, clearColor);
        else if (r.dispose == 3) canvas.swap(previous);
    }
}
void Apng(IWICImagingFactory* factory, const Bytes& file, Animation& output)
{
    Bytes header, shared, compressed, canvas;
    UINT w = 0, h = 0, expected = 0;
    uint64_t sequence = 0;
    bool actl = false, controlled = false, idat = false, ended = false;
    Rect r;
    UINT delay = 100;
    auto finish = [&]()
    {
        Require(controlled && !compressed.empty(), "APNG frame has no image data");
        Bytes png(file.begin(), file.begin() + 8), ihdr(header);
        for (int i = 0; i < 4; ++i) { ihdr[i] = BYTE(r.w >> (24 - i * 8)); ihdr[i + 4] = BYTE(r.h >> (24 - i * 8)); }
        Chunk(png, "IHDR", ihdr);
        png.insert(png.end(), shared.begin(), shared.end());
        Chunk(png, "IDAT", compressed);
        Chunk(png, "IEND", {});
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream));
        Check(stream->InitializeFromMemory(png.data(), static_cast<DWORD>(png.size())));
        ComPtr<IWICBitmapDecoder> decoder;
        Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(0, &frame));
        UINT fw, fh;
        Bytes pixels = Pixels(factory, frame.Get(), fw, fh);
        Require(fw == r.w && fh == r.h, "APNG frame size mismatch");
        Bytes previous;
        if (r.dispose == 2) previous = canvas;
        Composite(canvas, w, pixels, r);
        Save(factory, output, canvas, w, h);
        output.delays.push_back(delay);
        if (r.dispose == 1) Clear(canvas, w, r);
        else if (r.dispose == 2) canvas.swap(previous);
        compressed.clear();
    };
    for (size_t pos = 8; pos < file.size();)
    {
        Require(file.size() - pos >= 12, "Truncated PNG chunk");
        uint32_t n = Read32(file.data() + pos);
        Require(n <= file.size() - pos - 12, "Invalid PNG chunk size");
        const BYTE* type = file.data() + pos + 4;
        const BYTE* p = type + 4;
        Require(Crc(type, size_t(n) + 4) == Read32(p + n), "PNG checksum mismatch");
        auto is = [&](const char* s) { return std::memcmp(type, s, 4) == 0; };
        if (pos == 8) Require(is("IHDR"), "PNG missing IHDR");
        if (is("IHDR"))
        {
            Require(header.empty() && n == 13, "Invalid PNG header");
            header.assign(p, p + n); w = Read32(p); h = Read32(p + 4); canvas.resize(PixelBytes(w, h));
        }
        else if (is("acTL"))
        {
            Require(!actl && !idat && n == 8, "Invalid APNG animation control");
            expected = Read32(p); Require(expected && expected <= MaxFrames, "Invalid APNG frame count"); actl = true;
        }
        else if (is("fcTL"))
        {
            Require(actl && n == 26 && Read32(p) == sequence++, "Invalid APNG frame control or sequence");
            if (controlled) finish();
            r = { Read32(p + 4), Read32(p + 8), Read32(p + 12), Read32(p + 16), p[24], p[25] };
            const UINT numerator = (UINT(p[20]) << 8) | p[21];
            UINT denominator = (UINT(p[22]) << 8) | p[23];
            if (!denominator) denominator = 100;
            // Round fractional milliseconds to nearest, with a 10 ms timer minimum.
            delay = (std::max)(UINT(USER_TIMER_MINIMUM), (1000 * numerator + denominator / 2) / denominator);
            Bounds(r, w, h);
            Require(r.dispose <= 2 && r.blend <= 1, "Invalid APNG disposal/blend operation");
            if (!idat) Require(r.w == w && r.h == h && !r.x && !r.y, "Invalid APNG default frame");
            controlled = true;
        }
        else if (is("IDAT"))
        {
            Require(actl, "PNG is not animated (missing acTL)");
            Require(sequence <= 1, "Unexpected APNG IDAT");
            idat = true;
            if (controlled) compressed.insert(compressed.end(), p, p + n); // Otherwise the default image is a poster.
        }
        else if (is("fdAT"))
        {
            Require(idat && controlled && n >= 4 && Read32(p) == sequence++, "Invalid APNG frame data or sequence");
            compressed.insert(compressed.end(), p + 4, p + n);
        }
        else if (is("IEND"))
        {
            Require(n == 0 && idat && controlled, "Incomplete APNG");
            finish(); ended = true;
            Require(pos + 12 == file.size(), "Data after PNG end");
            break;
        }
        else if (!idat)
        {
            // All frame PNGs inherit palette, transparency, and color information.
            if (is("PLTE") || is("tRNS") || is("gAMA") || is("cHRM") || is("sRGB") || is("iCCP") || is("sBIT"))
                shared.insert(shared.end(), file.begin() + pos, file.begin() + pos + n + 12);
            else Require((type[0] & 32) != 0, "Unknown critical PNG chunk");
        }
        pos += size_t(n) + 12;
    }
    Require(ended && output.frames.size() == expected, "APNG frame count mismatch or missing IEND");
}
}

Animation::~Animation()
{
    for (const auto& path : frames) DeleteFileW(path.c_str());
    if (!directory.empty()) RemoveDirectoryW(directory.c_str());
}

void ExtractAnimation(const std::wstring& path, Animation& output)
{
    Require(output.directory.empty() && output.frames.empty(), "Extraction destination is already in use");
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(bool(input), "Cannot open animation file");
    const auto length = input.tellg();
    Require(length >= 8 && length <= 256 * 1024 * 1024, "Invalid or excessive animation file size (maximum 256 MiB)");
    Bytes bytes(static_cast<size_t>(length));
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(bool(input), "Cannot read animation file");
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH + 1, temp);
    Require(tempLength && tempLength < MAX_PATH - 50, "Cannot locate temporary directory");
    GUID id;
    Check(CoCreateGuid(&id));
    wchar_t guid[40]; StringFromGUID2(id, guid, 40);
    const std::wstring directory = std::wstring(temp) + L"Rainmeter-Animate-" + guid;
    Require(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "Cannot create frame directory");
    output.directory = directory;
    if (std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0) Gif(factory.Get(), path, output);
    else
    {
        const BYTE signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
        Require(std::memcmp(bytes.data(), signature, 8) == 0, "Input must be GIF or animated PNG");
        Apng(factory.Get(), bytes, output);
    }
}
