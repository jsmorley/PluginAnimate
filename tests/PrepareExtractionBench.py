"""Build a temporary PNG/BMP profiling harness from the current decoder.

Generated files stay in ignored tests/x64/Bench. Production source is unchanged.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
out = root / "tests/x64/Bench"
out.mkdir(parents=True, exist_ok=True)
source = (root / "Animation.cpp").read_text(encoding="utf-8")
source = source.replace('#include "Animation.h"', '#include "../../../Animation.h"')
source = source.replace('#include "third_party/', '#include "../../../third_party/')


def replace(old, new):
    global source
    assert source.count(old) == 1, old
    source = source.replace(old, new)


replace('    Check(source->GetSize(&w, &h));', '    BenchTime timer(decodeMs);\n    Check(source->GetSize(&w, &h));')
replace('    for (UINT y = 0; y < r.h; ++y)\n        for (UINT x = 0; x < r.w; ++x)\n        {',
        '    BenchTime timer(compositeMs);\n    for (UINT y = 0; y < r.h; ++y)\n        for (UINT x = 0; x < r.w; ++x)\n        {')
replace('    ComPtr<IWICStream> stream;\n    Check(factory->CreateStream(&stream));',
        '    BenchTime timer(saveMs);\n    ComPtr<IWICStream> stream;\n    Check(factory->CreateStream(&stream));')
replace('const std::function<bool()>& cancelled, bool bmp = false)',
        'const std::function<bool()>& cancelled)')
replace('SaveImage(factory, path, canvas, w, h, cancelled, true);',
        'SaveImage(factory, path, canvas, w, h, cancelled);')
replace('    Bytes straight(canvas);\n    for (size_t i',
        '    Bytes straight(canvas);\n    { BenchTime converting(convertMs);\n    for (size_t i')
replace('        if (straight[i + 3] == 255) continue;',
        '        if (fast && straight[i + 3] == 255) continue;')
replace('    for (UINT row = 0; row < h; row += 32)',
        '    }\n    BenchTime writing(writeMs);\n    for (UINT row = 0; row < h; row += 32)')
replace(' + L".bmp";', ' + (bmp ? L".bmp" : L".png");')

prefix = '''#include <chrono>
#include <filesystem>
#include <iostream>
bool bmp = false;
bool fast = false;
double decodeMs = 0, compositeMs = 0, saveMs = 0, convertMs = 0, writeMs = 0;
struct BenchTime {
    double& elapsed;
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    explicit BenchTime(double& target) : elapsed(target) {}
    ~BenchTime() { elapsed += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(); }
};
'''
main = r'''
int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 3) return 2;
        bmp = std::wstring(argv[1]).rfind(L"bmp", 0) == 0;
        fast = std::wstring(argv[1]).find(L"fast") != std::wstring::npos;
        const auto begin = std::chrono::steady_clock::now();
        Animation animation;
        ExtractAnimation(argv[2], std::filesystem::absolute("tests/x64/Bench/@Resources").wstring(), animation);
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        const auto extractDecodeMs = decodeMs;
        uint64_t bytes = 0;
        for (const auto& frame : animation.frames) bytes += std::filesystem::file_size(frame);
        // Decode every output, ensuring both formats preserve the same pixels.
        ComScope com;
        ComPtr<IWICImagingFactory> factory;
        Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        uint64_t hash = 14695981039346656037ull;
        for (const auto& frame : animation.frames) {
            ComPtr<IWICBitmapDecoder> decoder;
            Check(factory->CreateDecoderFromFilename(frame.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> image;
            Check(decoder->GetFrame(0, &image));
            UINT w, h;
            auto pixels = Pixels(factory.Get(), image.Get(), w, h);
            for (BYTE value : pixels) { hash ^= value; hash *= 1099511628211ull; }
        }
        std::cout << "{\"format\":\"" << (bmp ? "BMP-V5" : "PNG") << (fast ? "-fast-alpha" : "") << "\",\"frames\":" << animation.frames.size()
            << ",\"totalMs\":" << elapsed << ",\"saveMs\":" << saveMs << ",\"convertMs\":" << convertMs
            << ",\"writeMs\":" << writeMs << ",\"decodeMs\":" << extractDecodeMs << ",\"compositeMs\":" << compositeMs << ",\"bytes\":" << bytes
            << ",\"pixelHash\":\"" << hash << "\"}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''
(out / "Bench.cpp").write_text(prefix + source + main, encoding="utf-8")
project = (root / "tests/Tests.vcxproj").read_text(encoding="utf-8-sig")
project = project.replace('<ClCompile Include="Tests.cpp" /><ClCompile Include="..\\Animation.cpp" /><ClCompile Include="..\\AsyncLoader.cpp" />', '<ClCompile Include="Bench.cpp" />')
project = project.replace('<TargetName>Tests</TargetName>', '<TargetName>Bench</TargetName>')
project = project.replace('..\\third_party\\WebP.props', '..\\..\\..\\third_party\\WebP.props')
project = project.replace('{BB7D8B43-68CE-493D-B516-0497F95C42F6}', '{40D0C1CF-E2F8-42EB-A35F-7E499523BEEF}')
(out / "Bench.vcxproj").write_text(project, encoding="utf-8")
print(out)
