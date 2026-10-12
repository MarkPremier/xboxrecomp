#include <cstddef>

// Exercise private cache lookups and real texture uploads without a game or window.
#include "../../src/kernel/nv2a_gpu_d3d11.cpp"

extern "C" void xbox_Nv2aAcknowledgeHandshakes(void)
{
    std::fprintf(stderr, "Unexpected guest handshake in texture-only regression\n");
    std::abort();
}

extern "C" {
void (*g_xbox_path_hook)(const char *) = nullptr;
void xbox_CloakMarker(const char *)
{
    std::fprintf(stderr, "Unexpected cloak marker in texture-only regression\n");
    std::abort();
}
}

static int decode_pixel(void *source, uint32_t x, uint32_t y, uint32_t *pixel)
{
    if (x || y) return 0;
    *pixel = *static_cast<const uint32_t *>(source);
    return 1;
}

static ID3D11ShaderResourceView *bind_pixel(uint32_t &pixel)
{
    Nv2aGpuTexture binding = {};
    binding.source = reinterpret_cast<const uint8_t *>(&pixel);
    binding.source_bytes = sizeof(pixel);
    binding.width = binding.height = 1;
    binding.pitch = sizeof(pixel);
    binding.format = 0x12;
    binding.linear = 1;
    binding.decode_context = &pixel;
    binding.decode = decode_pixel;
    Surface destination = {};
    return get_texture(binding, destination);
}

static bool matches_pixel(ID3D11ShaderResourceView *view, uint32_t expected)
{
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> image;
    HRESULT result = resource.As(&image);
    if (FAILED(result)) {
        std::fprintf(stderr, "Texture query failed: 0x%08lX\n", (unsigned long)result);
        return false;
    }
    D3D11_TEXTURE2D_DESC description = {};
    image->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        std::fprintf(stderr, "Staging texture failed: 0x%08lX\n", (unsigned long)result);
        return false;
    }
    context->CopyResource(staging.Get(), image.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        std::fprintf(stderr, "Texture readback failed: 0x%08lX\n", (unsigned long)result);
        return false;
    }
    uint32_t actual = *static_cast<const uint32_t *>(mapped.pData);
    context->Unmap(staging.Get(), 0);
    if (actual != expected) {
        std::fprintf(stderr, "Texture readback mismatch: 0x%08X != 0x%08X\n", actual, expected);
        return false;
    }
    return true;
}

int main()
{
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                      nullptr, 0, D3D11_SDK_VERSION,
                                      &device, nullptr, &context);
    if (FAILED(result)) {
        std::fprintf(stderr, "WARP device creation failed: 0x%08lX\n", (unsigned long)result);
        return 1;
    }
    std::array<uint32_t, 1088> pixels = {};
    for (size_t i = 0; i < pixels.size(); i++)
        pixels[i] = 0xFF000000u | static_cast<uint32_t>(i);
    for (size_t i = 0; i < 1024; i++) {
        if (!bind_pixel(pixels[i])) {
            std::fprintf(stderr, "Initial texture creation failed at %zu\n", i);
            return 1;
        }
    }
    ComPtr<ID3D11ShaderResourceView> retained = bind_pixel(pixels[512]);
    if (!retained || gpu_timing.texture_created != 1024) {
        std::fprintf(stderr, "Cache warm-up created an unexpected texture\n");
        return 1;
    }
    for (size_t i = 1024; i < pixels.size(); i++) {
        if (!bind_pixel(pixels[i]) || textures.size() != 1024 ||
                gpu_timing.texture_cache_bytes > 256ull * 1024 * 1024) {
            std::fprintf(stderr, "Cache insertion/bounds failed at %zu\n", i);
            return 1;
        }
        uint64_t created = gpu_timing.texture_created;
        for (size_t resident : {size_t(256), size_t(512), size_t(1023), i}) {
            if (!bind_pixel(pixels[resident]) || gpu_timing.texture_created != created) {
                std::fprintf(stderr, "Resident texture %zu recreated after insertion %zu\n", resident, i);
                return 1;
            }
        }
        if (bind_pixel(pixels[512]) != retained.Get()) {
            std::fprintf(stderr, "Retained view changed after insertion %zu\n", i);
            return 1;
        }
    }
    pixels[512] = 0xFFABCDEF;
    ++s_texture_validate_serial;
    uint64_t updated = gpu_timing.texture_updated;
    if (bind_pixel(pixels[512]) != retained.Get() ||
            gpu_timing.texture_updated != updated + 1 ||
            !matches_pixel(retained.Get(), pixels[512])) {
        std::fprintf(stderr, "Guest texture modification was not refreshed correctly\n");
        return 1;
    }
    std::printf("texture cache: ALL PASS (64 full-cache evictions, resident views reused, changed pixels refreshed)\n");
    return 0;
}
