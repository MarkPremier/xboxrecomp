#include "nv2a_gpu_native.h"
#include "nv2a_gpu_shader.h"

using Microsoft::WRL::ComPtr;
static int failures;
static void check(bool ok, const char *name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3DBlob> blob;
    check(SUCCEEDED(nv2a_native_blob(16, &blob)) && blob->GetBufferSize() == 16,
          "native COM blob allocates bytes");
    ComPtr<IUnknown> unknown;
    check(SUCCEEDED(blob.As(&unknown)), "blob QueryInterface uses the native COM ABI");
    {
        ComPtr<ID3DBlob> copy = blob;
        blob.Reset();
        check(copy->GetBufferSize() == 16, "copied COM pointer survives original reset");
    }
    check(unknown->Release() == 0, "last COM reference releases the blob");
    // The previous call consumed unknown's reference; do not release twice.
    *unknown.GetAddressOf() = nullptr;

    const struct { const char *entry, *profile; } programs[] = {
        {"vs_main", "vs_5_0"}, {"ps_uncombined", "ps_5_0"}, {"ps_main", "ps_5_0"},
        {"gs_main", "gs_5_0"}, {"gs_lines_main", "gs_5_0"}, {"gs_points_main", "gs_5_0"},
    };
    for (const auto &program : programs) {
        ComPtr<ID3DBlob> code, errors;
        auto started = std::chrono::steady_clock::now();
        HRESULT result = nv2a_native_compile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1,
            "NV2A native regression", nullptr, nullptr, program.entry, program.profile,
            D3DCOMPILE_IEEE_STRICTNESS, 0, &code, &errors);
        check(SUCCEEDED(result) && code && code->GetBufferSize() > 4 &&
              !std::memcmp(code->GetBufferPointer(), "DXBC", 4), program.entry);
        if (FAILED(result) && errors) std::fprintf(stderr, "%s\n", (char *)errors->GetBufferPointer());
        std::printf("  %.3f seconds\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    }
    std::string stages = "{uint4(0x08200000,0x14200000,0xc00,0xc00)";
    for (int i = 1; i < 8; i++) stages += ",uint4(0,0,0,0)";
    stages += "}";
    const D3D_SHADER_MACRO specialized[] = {
        {"NV_COMPILED", "1"}, {"NV_STAGE_COUNT", "1"}, {"NV_COMBINER_CONTROL", "1"},
        {"NV_STAGE_PROGRAM", "1"}, {"NV_FINAL0", "0x0000000c"}, {"NV_FINAL1", "0x00001c80"},
        {"NV_STAGE_WORDS", stages.c_str()}, {"NV_W_DEPTH", "0"},
        {"NV_TEXTURE_SIGNS", "uint4(0,0,0,0)"}, {"NV_WINDOW_CLIP_MODE", "0"},
        {"NV_DEPTH_ENABLED", "1"}, {"NV_DEPTH_SEMANTIC", "SV_DepthGreaterEqual"}, {nullptr, nullptr},
    };
    {
        ComPtr<ID3DBlob> code, errors;
        auto started = std::chrono::steady_clock::now();
        HRESULT result = nv2a_native_compile(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1,
            "specialized", specialized, nullptr, "ps_main", "ps_5_0", 0, 0, &code, &errors);
        check(SUCCEEDED(result) && code, "specialized texture/combiner pixel shader");
        if (FAILED(result) && errors) std::fprintf(stderr, "%s\n", (char *)errors->GetBufferPointer());
        std::printf("  %.3f seconds, %zu bytes\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), code ? code->GetBufferSize() : 0);
    }
    const char source[] = "float4 main():SV_Target{return float4(VALUE,0,0,1);}";
    const D3D_SHADER_MACRO defines[] = {{"VALUE", "0.5"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code, errors;
    check(SUCCEEDED(nv2a_native_compile(source, sizeof source - 1, "macros", defines, nullptr,
          "main", "ps_5_0", 0, 0, &code, &errors)), "preprocessor macros reach vkd3d");
    const char depth[] = "struct O{float4 color:SV_Target;float depth:NV_DEPTH_SEMANTIC;};"
                         "O main(){O o;o.color=1;o.depth=0.5;return o;}";
    const D3D_SHADER_MACRO depth_defines[] = {{"NV_DEPTH_SEMANTIC", "SV_DepthGreaterEqual"}, {nullptr, nullptr}};
    check(SUCCEEDED(nv2a_native_compile(depth, sizeof depth - 1, "depth", depth_defines, nullptr,
          "main", "ps_5_0", 0, 0, &code, &errors)), "conservative depth falls back to ordinary depth writes");
    const char invalid[] = "float4 main():SV_Target{return missing;}";
    check(FAILED(nv2a_native_compile(invalid, sizeof invalid - 1, "invalid", nullptr, nullptr,
          "main", "ps_5_0", 0, 0, &code, &errors)) && !code && errors &&
          std::strstr((char *)errors->GetBufferPointer(), "missing"), "compile failure returns diagnostics and no bytecode");

    char directory[] = "/tmp/nv2a-native-files.XXXXXX";
    if (char *created = mkdtemp(directory)) {
        std::wstring path = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(created) + L"/shader-\u00e9.dxbc";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD count = 0;
        check(file != INVALID_HANDLE_VALUE && WriteFile(file, "DXBC", 4, &count, nullptr) && count == 4,
              "Unicode shader cache path writes");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        std::wstring renamed = path + L".new";
        check(MoveFileExW(path.c_str(), renamed.c_str(), MOVEFILE_REPLACE_EXISTING), "cache file is atomically renamed");
        file = CreateFileW(renamed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size = {};
        char contents[4] = {};
        check(file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart == 4 &&
              ReadFile(file, contents, 4, &count, nullptr) && count == 4 && !std::memcmp(contents, "DXBC", 4),
              "cache file reads its complete bytecode");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(renamed.c_str()); rmdir(created);
    } else check(false, "temporary cache directory created");
    return failures ? 1 : 0;
}
