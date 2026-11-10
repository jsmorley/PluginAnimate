#include "Animation.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <limits>
#include <mutex>
#include "third_party/libwebp-1.6.0/src/webp/demux.h"

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<BYTE>;
namespace
{
constexpr size_t MaxPixels = 16 * 1024 * 1024;
constexpr size_t MaxFrames = 10000;
constexpr uint64_t MaxOutputBytes = 1024ull * 1024 * 1024;
// Serialize shared Temp creation/removal across measures and background cleanup.
std::mutex temporaryDirectoryMutex;
constexpr wchar_t OwnerName[] = L"\\.animate-owner";
constexpr char OwnerSignature[] = "Rainmeter Animate extraction owner v1\r\n";
struct FileHandle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool PlainDirectory(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("Windows image codec operation failed"); }
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void EnsureDirectory(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr)) return;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "Cannot access Windows temporary directory; check write permissions");
}
std::wstring WindowsTempRoot()
{
    wchar_t path[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH + 1, path);
    if (!length || length > MAX_PATH - 70) return {};
    std::wstring root(path, length);
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    return root;
}
void Cancel(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) throw AnimationCancelled();
}
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
uint32_t Crc(const BYTE* p, size_t n, const std::function<bool()>& cancelled = {})
{
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
            values[i] = c;
        }
        return values;
    }();
    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < n; ++i)
    {
        if (!(i & 65535)) Cancel(cancelled);
        c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    }
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
AnimationFileLock SaveImage(IWICImagingFactory* factory, const std::wstring& path, const Bytes& canvas, UINT w, UINT h,
    const std::function<bool()>& cancelled, bool bmp = false)
{
    Cancel(cancelled);
    // Protect from the moment the file exists. Permit the WIC writer while it
    // encodes, then acquire a read-only lease before releasing this first one.
    AnimationFileLock creating(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(creating.value != INVALID_HANDLE_VALUE, "Cannot create protected frame file");
    {
        ComPtr<IWICStream> stream;
        Check(factory->CreateStream(&stream));
        Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;
        Check(factory->CreateEncoder(bmp ? GUID_ContainerFormatBmp : GUID_ContainerFormatPng, nullptr, &encoder));
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        Check(encoder->CreateNewFrame(&frame, &properties));
        if (bmp)
        {
            // The V5 header carries explicit alpha masks; legacy BMP headers do not.
            PROPBAG2 property{};
            property.pstrName = const_cast<wchar_t*>(L"EnableV5Header32bppBGRA");
            VARIANT value{};
            value.vt = VT_BOOL;
            value.boolVal = VARIANT_TRUE;
            Check(properties->Write(1, &property, &value));
        }
        Check(frame->Initialize(properties.Get()));
        Check(frame->SetSize(w, h));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        Check(frame->SetPixelFormat(&format));
        Require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA), "Image encoder does not support BGRA");
        // Both encoders store straight alpha; fully opaque pixels already match it.
        Bytes straight(canvas);
        for (size_t i = 0; i < straight.size(); i += 4)
        {
            if (!(i & 65535)) Cancel(cancelled);
            if (straight[i + 3] == 255) continue;
            for (size_t c = 0; c < 3; ++c)
                straight[i + c] = straight[i + 3] ? BYTE((std::min)(255u, (unsigned(straight[i + c]) * 255 + straight[i + 3] / 2) / straight[i + 3])) : 0;
        }
        for (UINT row = 0; row < h; row += 32)
        {
            Cancel(cancelled);
            const UINT rows = (std::min)(32u, h - row);
            Check(frame->WritePixels(rows, w * 4, rows * w * 4, straight.data() + size_t(row) * w * 4));
        }
        Cancel(cancelled);
        Check(frame->Commit());
        Check(encoder->Commit());
    } // Close all WIC write handles before denying further writes.
    AnimationFileLock protection(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(protection.value != INVALID_HANDLE_VALUE, "Cannot protect frame file");
    return protection;
}
void Save(IWICImagingFactory* factory, Animation& output, const Bytes& canvas, UINT w, UINT h, const std::function<bool()>& cancelled)
{
    Require(output.frames.size() < MaxFrames && uint64_t(canvas.size()) * (output.frames.size() + 1) <= MaxOutputBytes,
        "Animation exceeds extraction limits (10000 frames / 1 GiB decoded pixels)");
    std::wstring path = output.directory + L"\\frame-" + std::to_wstring(output.frames.size()) + L".bmp";
    output.frames.push_back(path); // Track partial writes for cleanup too.
    output.frameLocks.push_back(SaveImage(factory, path, canvas, w, h, cancelled, true));
}
struct TextMask
{
    HDC dc = nullptr;
    HFONT font = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldFont = nullptr, oldBitmap = nullptr;
    ~TextMask()
    {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (oldFont) SelectObject(dc, oldFont);
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
};
void MakePreview(IWICImagingFactory* factory, Animation& output, const Bytes& canvas, UINT w, UINT h,
    const std::function<bool()>& cancelled)
{
    Cancel(cancelled);
    Bytes pixels(canvas); // The real first frame and compositing canvas are never modified.
    if (w >= 32 && h >= 16)
    {
        TextMask mask;
        mask.dc = CreateCompatibleDC(nullptr);
        Require(mask.dc != nullptr, "Cannot create loading-preview text context");
        constexpr int fontHeight = 12;
        mask.font = CreateFontW(-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Require(mask.font != nullptr, "Cannot create loading-preview font");
        mask.oldFont = SelectObject(mask.dc, mask.font);
        constexpr wchar_t text[] = L"Loading...";
        SIZE extent{};
        Require(GetTextExtentPoint32W(mask.dc, text, 10, &extent) != FALSE, "Cannot measure loading text");
        constexpr UINT padding = 2;
        const UINT pw = (std::min)(w, UINT(extent.cx) + padding * 2);
        const UINT ph = (std::min)(h, UINT(extent.cy) + padding * 2);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(pw);
        info.bmiHeader.biHeight = -static_cast<LONG>(ph);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        mask.bitmap = CreateDIBSection(mask.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Require(mask.bitmap && bits, "Cannot create loading-preview text bitmap");
        mask.oldBitmap = SelectObject(mask.dc, mask.bitmap);
        std::memset(bits, 0, size_t(pw) * ph * 4);
        SetBkMode(mask.dc, TRANSPARENT);
        SetTextColor(mask.dc, RGB(255, 255, 255));
        RECT rect{0, 0, static_cast<LONG>(pw), static_cast<LONG>(ph)};
        Require(DrawTextW(mask.dc, text, 10, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX) != 0,
            "Cannot render loading-preview text");
        GdiFlush();
        const auto* coverage = static_cast<const BYTE*>(bits);
        const UINT left = (w - pw) / 2, top = (h - ph) / 2;
        for (UINT y = 0; y < ph; ++y)
        {
            Cancel(cancelled);
            for (UINT x = 0; x < pw; ++x)
            {
                BYTE* d = pixels.data() + (size_t(y + top) * w + x + left) * 4;
                // A translucent black panel keeps white text readable on any frame.
                for (int c = 0; c < 3; ++c) d[c] = BYTE((unsigned(d[c]) * 55 + 127) / 255);
                d[3] = BYTE(200 + (unsigned(d[3]) * 55 + 127) / 255);
                const unsigned alpha = coverage[(size_t(y) * pw + x) * 4];
                for (int c = 0; c < 4; ++c) d[c] = BYTE(alpha + (unsigned(d[c]) * (255 - alpha) + 127) / 255);
            }
        }
    }
    output.preview = output.directory + L"\\loading.png";
    output.previewLock = SaveImage(factory, output.preview, pixels, w, h, cancelled);
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
void Gif(IWICImagingFactory* factory, const std::wstring& path, Animation& output, const std::function<bool()>& cancelled,
    const std::function<void(const Animation&)>& firstFrame, bool firstFrameOnly)
{
    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
    Cancel(cancelled);
    ComPtr<IWICMetadataQueryReader> global;
    Check(decoder->GetMetadataQueryReader(&global));
    UINT w = Metadata(global.Get(), L"/logscrdesc/Width"), h = Metadata(global.Get(), L"/logscrdesc/Height");
    Bytes canvas(PixelBytes(w, h));
    UINT count;
    Check(decoder->GetFrameCount(&count));
    Require(count && count <= MaxFrames, "Invalid or excessive GIF frame count");
    Require(uint64_t(canvas.size()) * count <= MaxOutputBytes, "Animation exceeds extraction limits (1 GiB decoded pixels)");
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
        Cancel(cancelled);
        ComPtr<IWICBitmapFrameDecode> frame;
        Check(decoder->GetFrame(i, &frame));
        ComPtr<IWICMetadataQueryReader> metadata;
        Check(frame->GetMetadataQueryReader(&metadata));
        Rect r;
        Bytes pixels = Pixels(factory, frame.Get(), r.w, r.h);
        Cancel(cancelled);
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
        Save(factory, output, canvas, w, h, cancelled);
        // GIF delays are hundredths of a second. Missing metadata defaults to 100 ms.
        output.delays.push_back((std::max)(UINT(USER_TIMER_MINIMUM), Metadata(metadata.Get(), L"/grctlext/Delay", 10) * 10));
        if (firstFrameOnly) return;
        if (!firstFrameOnly && output.frames.size() == 1 && firstFrame)
        {
            MakePreview(factory, output, canvas, w, h, cancelled);
            firstFrame(output);
        }
        if (r.dispose == 2) Clear(canvas, w, r, clearColor);
        else if (r.dispose == 3) canvas.swap(previous);
    }
}
void Webp(IWICImagingFactory* factory, const Bytes& file, Animation& output, const std::function<bool()>& cancelled,
    const std::function<void(const Animation&)>& firstFrame, bool firstFrameOnly)
{
    const WebPData data{file.data(), file.size()};
    // Inspect dimensions/counts before AnimDecoder allocates its compositing canvases.
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data), WebPDemuxDelete);
    Require(bool(demux), "Invalid or truncated WebP container");
    Require((WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS) & ANIMATION_FLAG) != 0, "WebP is not animated");
    const UINT w = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    const UINT h = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    const size_t size = PixelBytes(w, h);
    const UINT count = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    Require(count && count <= MaxFrames && uint64_t(size) * count <= MaxOutputBytes,
        "Animation exceeds extraction limits (10000 frames / 1 GiB decoded pixels)");
    // libwebp's animation timestamps are signed int milliseconds. Check their
    // cumulative range before decoding, including animations with huge delays.
    {
        struct Iterator
        {
            WebPIterator frame{};
            ~Iterator() { WebPDemuxReleaseIterator(&frame); }
        } iterator;
        Require(WebPDemuxGetFrame(demux.get(), 1, &iterator.frame) != 0, "Missing WebP frame");
        uint64_t duration = 0;
        for (UINT i = 0; i < count; ++i)
        {
            Cancel(cancelled);
            duration += static_cast<UINT>(iterator.frame.duration);
            Require(iterator.frame.complete && duration <= uint64_t((std::numeric_limits<int>::max)()),
                "Incomplete WebP frame or excessive animation duration");
            if (i + 1 < count) Require(WebPDemuxNextFrame(&iterator.frame) != 0, "Missing WebP frame");
        }
    }
    demux.reset();
    Cancel(cancelled);
    WebPAnimDecoderOptions options{};
    Require(WebPAnimDecoderOptionsInit(&options) != 0, "Cannot initialize WebP decoder");
    options.color_mode = MODE_bgrA; // Premultiplied BGRA, matching the other decoders.
    options.use_threads = 0; // Already on our background loader; avoid extra codec threads.
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
        WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
    Require(bool(decoder), "Cannot decode WebP animation");
    Bytes canvas(size);
    int previous = 0;
    for (UINT i = 0; i < count; ++i)
    {
        Cancel(cancelled);
        uint8_t* pixels = nullptr;
        int timestamp = 0;
        Require(WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp) != 0 && timestamp >= previous,
            "Invalid WebP frame data");
        Cancel(cancelled);
        std::memcpy(canvas.data(), pixels, size);
        Save(factory, output, canvas, w, h, cancelled);
        output.delays.push_back((std::max)(UINT(USER_TIMER_MINIMUM), UINT(timestamp - previous)));
        previous = timestamp;
        if (firstFrameOnly) return;
        if (!i && firstFrame)
        {
            MakePreview(factory, output, canvas, w, h, cancelled);
            firstFrame(output);
        }
    }
}
void Apng(IWICImagingFactory* factory, const Bytes& file, Animation& output, const std::function<bool()>& cancelled,
    const std::function<void(const Animation&)>& firstFrame, bool firstFrameOnly)
{
    Bytes header, shared, compressed, canvas;
    UINT w = 0, h = 0, expected = 0;
    uint64_t sequence = 0;
    bool actl = false, controlled = false, idat = false, ended = false;
    Rect r;
    UINT delay = 100;
    auto finish = [&]()
    {
        Cancel(cancelled);
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
        Save(factory, output, canvas, w, h, cancelled);
        output.delays.push_back(delay);
        if (!firstFrameOnly && output.frames.size() == 1 && firstFrame)
        {
            MakePreview(factory, output, canvas, w, h, cancelled);
            firstFrame(output);
        }
        if (r.dispose == 1) Clear(canvas, w, r);
        else if (r.dispose == 2) canvas.swap(previous);
        compressed.clear();
    };
    for (size_t pos = 8; pos < file.size();)
    {
        Cancel(cancelled);
        Require(file.size() - pos >= 12, "Truncated PNG chunk");
        uint32_t n = Read32(file.data() + pos);
        Require(n <= file.size() - pos - 12, "Invalid PNG chunk size");
        const BYTE* type = file.data() + pos + 4;
        const BYTE* p = type + 4;
        Require(Crc(type, size_t(n) + 4, cancelled) == Read32(p + n), "PNG checksum mismatch");
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
            Require(uint64_t(canvas.size()) * expected <= MaxOutputBytes, "Animation exceeds extraction limits (1 GiB decoded pixels)");
        }
        else if (is("fcTL"))
        {
            Require(actl && n == 26 && Read32(p) == sequence++, "Invalid APNG frame control or sequence");
            if (controlled) { finish(); if (firstFrameOnly) return; }
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
            finish(); if (firstFrameOnly) return;
            ended = true;
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
    previewLock.Reset();
    frameLocks.clear();
    if (!preview.empty()) DeleteFileW(preview.c_str());
    for (const auto& path : frames) DeleteFileW(path.c_str());
    std::lock_guard<std::mutex> lock(temporaryDirectoryMutex);
    if (ownerFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(ownerFile);
        if (!directory.empty()) DeleteFileW((directory + OwnerName).c_str());
    }
    if (!directory.empty()) RemoveDirectoryW(directory.c_str());
    // Windows Temp belongs to Windows; remove only our unique extraction folder.
}

namespace
{
void CleanupOrphanRoot(const std::wstring& root, bool removeEmptyRoot)
{
    if (root.empty()) return;
    std::lock_guard<std::mutex> lock(temporaryDirectoryMutex);
    if (!PlainDirectory(root)) return;
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((root + L"\\Rainmeter-Animate-*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do
    {
        const std::wstring name = entry.cFileName;
        const std::wstring prefix = L"Rainmeter-Animate-";
        if (name.size() != prefix.size() + 38 || name.compare(0, prefix.size(), prefix) != 0 ||
            !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        GUID id{};
        if (FAILED(CLSIDFromString(name.c_str() + prefix.size(), &id))) continue;
        const std::wstring folder = root + L"\\" + name;
        const std::wstring owner = folder + OwnerName;
        const DWORD attributes = GetFileAttributesW(owner.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        // Exclusive open fails while any live extraction owns the marker,
        // including extractions in another Rainmeter process or DLL version.
        FileHandle lease;
        lease.value = CreateFileW(owner.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (lease.value == INVALID_HANDLE_VALUE) continue;
        char signature[sizeof(OwnerSignature)]{};
        DWORD bytes = 0;
        if (!ReadFile(lease.value, signature, sizeof(signature), &bytes, nullptr) ||
            bytes != sizeof(OwnerSignature) - 1 ||
            std::memcmp(signature, OwnerSignature, sizeof(OwnerSignature) - 1) != 0) continue;
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW child{};
        HANDLE contents = FindFirstFileW((folder + L"\\*").c_str(), &child);
        if (contents == INVALID_HANDLE_VALUE) continue;
        bool safe = true;
        do
        {
            const std::wstring file = child.cFileName;
            if (file == L"." || file == L".." || file == L".animate-owner") continue;
            const bool frame = file.size() > 10 && file.compare(0, 6, L"frame-") == 0 &&
                file.compare(file.size() - 4, 4, L".bmp") == 0 &&
                file.find_first_not_of(L"0123456789", 6) == file.size() - 4;
            if ((child.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                (!frame && file != L"loading.png")) { safe = false; break; }
            files.push_back(folder + L"\\" + file);
        } while (FindNextFileW(contents, &child));
        if (safe && GetLastError() != ERROR_NO_MORE_FILES) safe = false;
        FindClose(contents);
        if (!safe) continue;
        for (const auto& file : files) if (!DeleteFileW(file.c_str())) safe = false;
        // Keep the marker on partial failure so a later run can retry.
        if (!safe) continue;
        CloseHandle(lease.value);
        lease.value = INVALID_HANDLE_VALUE;
        if (DeleteFileW(owner.c_str())) RemoveDirectoryW(folder.c_str());
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    if (removeEmptyRoot) RemoveDirectoryW(root.c_str());
}
}

void CleanupOrphanAnimations(const std::wstring& resources)
{
    CleanupOrphanRoot(WindowsTempRoot(), false);
    // Also recover marked leftovers from versions 1.5 through 1.7 without
    // creating @Resources or touching active older plugin instances.
    if (resources.empty() || !PlainDirectory(resources)) return;
    std::wstring legacy = resources;
    if (legacy.back() != L'\\' && legacy.back() != L'/') legacy += L'\\';
    CleanupOrphanRoot(legacy + L"Temp", true);
}

void ExtractAnimation(const std::wstring& path, const std::wstring& resources, Animation& output, const std::function<bool()>& cancelled,
    const std::function<void(const Animation&)>& firstFrame, bool firstFrameOnly)
{
    Cancel(cancelled);
    output.firstFrameOnly = firstFrameOnly;
    CleanupOrphanAnimations(resources);
    Require(output.directory.empty() && output.temporaryRoot.empty() && output.frames.empty(), "Extraction destination is already in use");
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(bool(input), "Cannot open animation file");
    const auto length = input.tellg();
    Require(length >= 8 && length <= 256 * 1024 * 1024, "Invalid or excessive animation file size (maximum 256 MiB)");
    Bytes bytes(8);
    input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()), 8);
    Require(bool(input), "Cannot read animation file");
    const bool gif = std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0;
    if (!gif)
    {
        bytes.resize(static_cast<size_t>(length));
        for (size_t pos = 8; pos < bytes.size();)
        {
            Cancel(cancelled);
            const size_t count = (std::min)(size_t(1024 * 1024), bytes.size() - pos);
            input.read(reinterpret_cast<char*>(bytes.data() + pos), static_cast<std::streamsize>(count));
            Require(bool(input), "Cannot read animation file");
            pos += count;
        }
    }
    Cancel(cancelled);
    const std::wstring root = WindowsTempRoot();
    Require(!root.empty(), "Cannot locate Windows temporary directory");
    GUID id;
    Check(CoCreateGuid(&id));
    wchar_t guid[40]; StringFromGUID2(id, guid, 40);
    const std::wstring directory = root + L"\\Rainmeter-Animate-" + guid;
    {
        std::lock_guard<std::mutex> lock(temporaryDirectoryMutex);
        EnsureDirectory(root);
        output.temporaryRoot = root;
        Require(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "Cannot create temporary frame directory");
        output.directory = directory;
        output.ownerFile = CreateFileW((directory + OwnerName).c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(output.ownerFile != INVALID_HANDLE_VALUE, "Cannot create frame ownership marker");
        DWORD written = 0;
        Require(WriteFile(output.ownerFile, OwnerSignature, sizeof(OwnerSignature) - 1, &written, nullptr) &&
            written == sizeof(OwnerSignature) - 1 && FlushFileBuffers(output.ownerFile),
            "Cannot write frame ownership marker");
    }
    if (gif) Gif(factory.Get(), path, output, cancelled, firstFrame, firstFrameOnly);
    else if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
    {
        const uint32_t riffSize = uint32_t(bytes[4]) | uint32_t(bytes[5]) << 8 | uint32_t(bytes[6]) << 16 | uint32_t(bytes[7]) << 24;
        Require(uint64_t(riffSize) + 8 == bytes.size(), "Invalid WebP RIFF length");
        Webp(factory.Get(), bytes, output, cancelled, firstFrame, firstFrameOnly);
    }
    else
    {
        const BYTE signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
        Require(std::memcmp(bytes.data(), signature, 8) == 0, "Input must be GIF, animated PNG, or animated WebP");
        Apng(factory.Get(), bytes, output, cancelled, firstFrame, firstFrameOnly);
    }
    Cancel(cancelled);
}
