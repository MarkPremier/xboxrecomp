#include "nv2a_gpu_native.h"
#include <vkd3d_shader.h>
#include <new>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class NativeShaderBlob final : public ID3DBlob {
    std::atomic<ULONG> references{1};
    std::vector<unsigned char> bytes;
public:
    explicit NativeShaderBlob(size_t size) : bytes(size) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3DBlob)) return E_NOINTERFACE;
        *out = static_cast<ID3DBlob *>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = --references; if (!n) delete this; return n; }
    void *STDMETHODCALLTYPE GetBufferPointer() override { return bytes.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes.size(); }
};

HRESULT nv2a_native_blob(size_t size, ID3DBlob **blob)
{
    if (!blob) return E_POINTER;
    *blob = nullptr;
    try { *blob = new NativeShaderBlob(size); return S_OK; }
    catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
}

/* Specialized pixel shaders (NV_COMPILED): the register combiners as
 * straight-line HLSL.
 *
 * The renderer's ps_main runs the combiners on `float4 registers[16]`,
 * passing the whole array by value into mapped()/products() for every
 * operand. FXC folds that away. vkd3d copies the array at every call: with
 * the stage loops unrolled it takes minutes per shader, and with them as
 * [loop] the result keeps indexed temporaries that RADV places in scratch
 * memory (~4k instructions, ~85 Mpix/s on a Vega 6).
 *
 * In a specialized shader every combiner word is a compile-time constant
 * (NV_STAGE_WORDS, NV_FINAL0/1, NV_COMBINER_CONTROL), so the combiners are
 * decoded here and emitted on sixteen named registers r0-r15, mirroring
 * mapped(), output_map(), products() and store() in nv2a_gpu_shader.h,
 * including their order (both products of a stage before either store).
 * The texture stages are left as they are. Any marker not found leaves the
 * source untouched. */
namespace {
bool macro_value(const D3D_SHADER_MACRO *defines, const char *name, uint32_t &out)
{
    for (const D3D_SHADER_MACRO *m = defines; m && m->Name; m++)
        if (!std::strcmp(m->Name, name) && m->Definition) {
            out = (uint32_t)std::strtoul(m->Definition, nullptr, 0);
            return true;
        }
    return false;
}

bool stage_words(const D3D_SHADER_MACRO *defines, uint32_t words[32])
{
    const char *text = nullptr;
    for (const D3D_SHADER_MACRO *m = defines; m && m->Name; m++)
        if (!std::strcmp(m->Name, "NV_STAGE_WORDS")) text = m->Definition;
    if (!text) return false;
    for (int stage = 0; stage < 8; stage++) {
        text = std::strstr(text, "uint4(");
        if (!text) return false;
        text += 6;
        for (int component = 0; component < 4; component++) {
            char *end = nullptr;
            words[stage * 4 + component] = (uint32_t)std::strtoul(text, &end, 0);
            if (end == text) return false;
            text = end;
            while (*text == 'u' || *text == 'U' || *text == ' ') text++;
            if (component < 3) {
                if (*text != ',') return false;
                text++;
            }
        }
    }
    return true;
}

std::string reg(uint32_t index) { return "r" + std::to_string(index & 15); }

/* mapped(): an input mapping of one register, as a float4 expression. */
std::string mapped(uint32_t encoded, bool alpha)
{
    encoded &= 255;
    std::string v = reg(encoded);
    if (encoded & 16) v += ".aaaa";
    else if (alpha) v += ".bbbb";
    switch ((encoded >> 5) & 7) {
    case 0: return "max(" + v + ",0)";
    case 1: return "(1-saturate(" + v + "))";
    case 2: return "(2*max(" + v + ",0)-1)";
    case 3: return "(1-2*max(" + v + ",0))";
    case 4: return "(max(" + v + ",0)-0.5)";
    case 5: return "(0.5-max(" + v + ",0))";
    case 6: return v;
    default: return "(-" + v + ")";
    }
}

/* output_map(): the stage's output scale/bias, then the clamp. */
std::string output_map(const std::string &v, uint32_t output)
{
    std::string scaled;
    switch ((output >> 15) & 7) {
    case 1: scaled = "(" + v + "-0.5)"; break;
    case 2: scaled = "(" + v + "*2)"; break;
    case 3: scaled = "((" + v + "-0.5)*2)"; break;
    case 4: scaled = "(" + v + "*4)"; break;
    case 6: scaled = "(" + v + "*0.5)"; break;
    default: scaled = v; break;
    }
    return "clamp(" + scaled + ",-1,1)";
}

/* products(): three temporaries <p>ab, <p>cd, <p>sum. */
void products(std::string &out, const std::string &p, uint32_t input, uint32_t output,
              bool alpha, const std::string &select)
{
    out += " float4 " + p + "ab=" + mapped(input >> 24, alpha) + "*" + mapped(input >> 16, alpha) + ";\n";
    out += " float4 " + p + "cd=" + mapped(input >> 8, alpha) + "*" + mapped(input, alpha) + ";\n";
    if (!alpha && (output & 0x2000)) out += " " + p + "ab=dot(" + p + "ab.rgb,float3(1,1,1)).xxxx;\n";
    if (!alpha && (output & 0x1000)) out += " " + p + "cd=dot(" + p + "cd.rgb,float3(1,1,1)).xxxx;\n";
    if (output & 0x4000) out += " float4 " + p + "sum=" + select + "?" + p + "cd:" + p + "ab;\n";
    else out += " float4 " + p + "sum=" + p + "ab+" + p + "cd;\n";
    for (const char *part : {"ab", "cd", "sum"})
        out += " " + p + part + "=" + output_map(p + part, output) + ";\n";
}

/* store(): destinations in product order, so a later product wins. */
void store(std::string &out, const std::string &p, uint32_t output, bool alpha)
{
    const uint32_t destinations[3] = {(output >> 4) & 15, output & 15, (output >> 8) & 15};
    const char *parts[3] = {"ab", "cd", "sum"};
    for (int product = 0; product < 3; product++) {
        uint32_t d = destinations[product];
        if (!d) continue;
        if (alpha) out += " " + reg(d) + ".a=" + p + parts[product] + ".x;\n";
        else {
            out += " " + reg(d) + ".rgb=" + p + parts[product] + ".rgb;\n";
            if (product < 2 && (output & (product == 0 ? 0x80000u : 0x40000u)))
                out += " " + reg(d) + ".a=" + p + parts[product] + ".b;\n";
        }
    }
}

void specialize_combiners(std::string &body, const D3D_SHADER_MACRO *defines)
{
    static const char zero_loop[] =
        " float4 registers[16]; [unroll]for(uint index=0;index<16;index++)registers[index]=0;";
    static const char combiner_start[] = " registers[12].a=registers[8].a;";
    uint32_t count, control, final0, final1, words[32];
    if (!macro_value(defines, "NV_STAGE_COUNT", count) || !macro_value(defines, "NV_COMBINER_CONTROL", control) ||
        !macro_value(defines, "NV_FINAL0", final0) || !macro_value(defines, "NV_FINAL1", final1) ||
        !stage_words(defines, words))
        return;
    const size_t main_at = body.find("PixelOutput ps_main(");
    if (main_at == std::string::npos) return;
    /* ps_main's closing brace, by matching braces from its opening one (other
     * functions, e.g. pixel_lighting, may follow it in the source). */
    size_t end = body.find('{', main_at);
    if (end == std::string::npos) return;
    for (int depth = 0; end < body.size(); end++) {
        if (body[end] == '{') depth++;
        else if (body[end] == '}' && --depth == 0) break;
    }
    if (end >= body.size()) return;
    const size_t zero_at = body.find(zero_loop, main_at);
    const size_t start = body.find(combiner_start, main_at);
    if (zero_at == std::string::npos || start == std::string::npos || zero_at > end || start > end || zero_at > start)
        return;
    if (count > 8) count = 8;

    std::string code = " float4 r0=registers[0],r1=registers[1],r2=registers[2],r3=registers[3],"
                       "r4=registers[4],r5=registers[5],r6=registers[6],r7=registers[7],"
                       "r8=registers[8],r9=registers[9],r10=registers[10],r11=registers[11],"
                       "r12=registers[12],r13=registers[13],r14=registers[14],r15=registers[15];\n";
    code += " r12.a=r8.a;\n";
    for (uint32_t stage = 0; stage < count; stage++) {
        const uint32_t *w = words + stage * 4;
        const std::string s = std::to_string(stage);
        code += " r1=factors[" + std::to_string(((control & 0x1000) ? stage : 0) * 2) + "];\n";
        code += " r2=factors[" + std::to_string(((control & 0x10000) ? stage : 0) * 2 + 1) + "];\n";
        const std::string select = "select" + s;
        if (control & 0x100) code += " bool " + select + "=r12.a>=0.5;\n";
        else code += " bool " + select + "=(((uint)(saturate(r12.a)*255))&1)!=0;\n";
        products(code, "c" + s, w[0], w[2], false, select);
        products(code, "a" + s, w[1], w[3], true, select);
        store(code, "c" + s, w[2], false);
        store(code, "a" + s, w[3], true);
    }
    code += " r1=factors[16];r2=factors[17];\n";
    code += " float4 specular=r5,temporary=r12;\n";
    if (final1 & 0x40) code += " specular=1-specular;\n";
    if (final1 & 0x20) code += " temporary=1-temporary;\n";
    code += " r14=specular+temporary;\n";
    if (final1 & 0x80) code += " r14=saturate(r14);\n";
    code += " r15=" + mapped(final1 >> 24, false) + "*" + mapped(final1 >> 16, false) + ";\n";
    code += " r14.a=0;r15.a=0;\n";
    if (final0 == 0 && final1 == 0) {
        code += std::string(" float4 result=") + (count ? "r12" : "r4") + ";result.a*=hazeCoverage;\n";
    } else {
        code += " float4 aa=" + mapped(final0 >> 24, false) + ";\n";
        code += " float4 result=aa*" + mapped(final0 >> 16, false) + "+(1-aa)*" + mapped(final0 >> 8, false) +
                "+" + mapped(final0, false) + ";\n";
        code += " result.a=" + mapped(final1 >> 8, true) + ".x; result=saturate(result);\n";
        code += " result.a*=hazeCoverage;\n";
    }
    code += " return finish_pixel(result,input,frontFacing);\n";

    std::string zeros = " float4 registers[16];";
    for (int index = 0; index < 16; index++) zeros += " registers[" + std::to_string(index) + "]=0;";
    body.replace(start, end - start, code);
    body.replace(zero_at, std::strlen(zero_loop), zeros);
}
} // namespace

HRESULT nv2a_native_compile(const void *source, size_t size, const char *name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, const char *entry,
    const char *profile, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors)
{
    if (code) *code = nullptr;
    if (errors) *errors = nullptr;
    if (!code || !source || !profile || include) return E_INVALIDARG;
    (void)flags1; (void)flags2;
    /* vkd3d 1.19 does not implement these SM4 intrinsics. Bit tests retain
     * NaN/Inf behavior even when the compiler folds floating comparisons. */
    std::string prefixed;
    for (int n = 1; n <= 4; n++) {
        std::string suffix = n == 1 ? "" : std::to_string(n);
        prefixed += "bool" + suffix + " nv_isnan(float" + suffix + " v){return (asuint(v)&0x7fffffffu)>0x7f800000u;}\n";
        prefixed += "bool" + suffix + " nv_isinf(float" + suffix + " v){return (asuint(v)&0x7fffffffu)==0x7f800000u;}\n";
        prefixed += "bool" + suffix + " nv_isfinite(float" + suffix + " v){return (asuint(v)&0x7f800000u)!=0x7f800000u;}\n";
    }
    // Pixel-only helpers share the renderer's source with VS/GS entry points.
    // Their sample-count expression is unreachable in those other stages.
    std::string body(static_cast<const char *>(source), size);
    for (const char *function : {"isnan(", "isinf(", "isfinite("}) {
        for (size_t at = 0; (at = body.find(function, at)) != std::string::npos; at += std::strlen(function) + 3)
            body.insert(at, "nv_");
    }
    // FXC can unroll these nested combiner/texture loops cheaply; vkd3d's
    // simplifier expands them for minutes. SM5 loop control has the same
    // results and keeps native shader compilation bounded.
    bool specialized = false;
    for (const D3D_SHADER_MACRO *macro = defines; macro && macro->Name; macro++)
        specialized |= !std::strcmp(macro->Name, "NV_COMPILED");
    if (specialized && profile[0] == 'p' && entry && !std::strcmp(entry, "ps_main"))
        specialize_combiners(body, defines);
    if (profile[0] == 'p') {
        for (const char *index : {"stage", "product", "rectangle", "index"}) {
            if (specialized && (!std::strcmp(index, "stage") || !std::strcmp(index, "product"))) continue;
            const std::string loop = std::string("[unroll]for(uint ") + index;
            for (size_t at = 0; (at = body.find(loop, at)) != std::string::npos; at += 6)
                body.replace(at, 8, "[loop]");
        }
    }
    if (profile[0] != 'p') {
        const std::string expression = "GetRenderTargetSampleCount()";
        for (size_t at = 0; (at = body.find(expression, at)) != std::string::npos; at++)
            body.replace(at, expression.size(), "1");
    }
    prefixed += body;
    /* RECOMP_NATIVE_HLSL_DUMP=<file>: append each pixel shader's final source
     * (diagnostic, for reviewing the specialized combiners). */
    if (profile[0] == 'p') {
        if (const char *dump = std::getenv("RECOMP_NATIVE_HLSL_DUMP"); dump && *dump) {
            if (FILE *f = std::fopen(dump, "ab")) {
                std::fprintf(f, "// ---- %s %s%s\n", name ? name : "?", entry ? entry : "?", specialized ? " (specialized)" : "");
                std::fwrite(prefixed.data(), 1, prefixed.size(), f);
                std::fclose(f);
            }
        }
    }
    std::vector<vkd3d_shader_macro> macros;
    for (; defines && defines->Name; defines++) {
        const char *value = defines->Definition;
        // vkd3d lacks conservative-depth semantics. Ordinary depth writes
        // preserve the result, with the early-Z optimization left disabled.
        if (!std::strcmp(defines->Name, "NV_DEPTH_SEMANTIC") && value &&
            (!std::strcmp(value, "SV_DepthGreaterEqual") || !std::strcmp(value, "SV_DepthLessEqual")))
            value = "SV_DEPTH";
        macros.push_back({defines->Name, value});
    }
    vkd3d_shader_preprocess_info preprocess = {};
    preprocess.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    preprocess.macros = macros.data(); preprocess.macro_count = macros.size();
    vkd3d_shader_hlsl_source_info hlsl = {};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.next = &preprocess; hlsl.entry_point = entry; hlsl.profile = profile;
    vkd3d_shader_compile_info info = {};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl; info.source = {prefixed.data(), prefixed.size()};
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level = VKD3D_SHADER_LOG_WARNING; info.source_name = name;
    const vkd3d_shader_compile_option options[] = {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_19},
    };
    info.options = options; info.option_count = sizeof options / sizeof options[0];
    vkd3d_shader_code result = {};
    char *messages = nullptr;
    int status = vkd3d_shader_compile(&info, &result, &messages);
    if (messages && errors && SUCCEEDED(nv2a_native_blob(std::strlen(messages) + 1, errors)))
        std::memcpy((*errors)->GetBufferPointer(), messages, std::strlen(messages) + 1);
    vkd3d_shader_free_messages(messages);
    HRESULT hr = status < 0 ? E_FAIL : nv2a_native_blob(result.size, code);
    if (SUCCEEDED(hr)) std::memcpy((*code)->GetBufferPointer(), result.code, result.size);
    vkd3d_shader_free_shader_code(&result);
    return hr;
}
