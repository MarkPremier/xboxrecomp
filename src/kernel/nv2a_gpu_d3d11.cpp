#include "nv2a_gpu.h"
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <d3dcompiler.h>
#include "nv2a_gpu_shader.h"
#include "../d3d/d3d8_swizzle.h"
#include <vector>
#include <array>
#include <cstring>
#include <string>
#include <cstddef>
#include <chrono>
#include <algorithm>
#include <set>
#include <atomic>
#include <unordered_map>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

using Microsoft::WRL::ComPtr;

/* Clear the NV2A WC-flush/idle handshake bits the guest spins on (KickOff).
 * Called from the GPU-wait spins so a long wait inside one packet does not
 * hold up the guest's next kick. Register write only, so any thread may call. */
extern "C" void xbox_Nv2aAcknowledgeHandshakes(void);

/* Cloak ability marker. The cloak vertex/pixel shaders are only ever loaded
 * when the effect engages, so their load is a definitive "the cloak is on".
 * kernel_path.c invokes g_xbox_path_hook for every opened Xbox path; install a
 * hook that edge-latches a "[CLOAK] active" log line a test can wait for. */
extern "C" void xbox_CloakMarker(const char *what);
extern "C" void (*g_xbox_path_hook)(const char *xbox_path);

static void nv2a_cloak_path_hook(const char *xbox_path)
{
    if (!xbox_path) return;
    if (!std::strstr(xbox_path, "cloak") && !std::strstr(xbox_path, "Cloak")) return;
    if (!std::strstr(xbox_path, ".xvu") && !std::strstr(xbox_path, ".xpu")) return;
    xbox_CloakMarker(xbox_path);
}

static struct {
    double draw, texture, hash, upload, sync, state;
    double sync_copy, sync_wait, sync_publish, sync_unmap;
    double color_publish, depth_publish;
    double sync_event_wait;
    double constants;
    double draw_setup, draw_textures, draw_streams, draw_shaders, draw_submit, draw_bookkeeping;
    double vertex_compile, pixel_compile;
    uint64_t vertex_shader_hits, vertex_shader_misses, pixel_shader_hits, pixel_shader_misses, shader_evictions;
    uint64_t vertex_shader_failures, pixel_shader_failures, shader_disk_hits, pixel_async_compiles, pixel_generic_draws, vertex_async_compiles, vertex_skipped_draws;
    uint64_t hash_bytes;
    uint64_t blend_created, depth_created, sampler_created;
    uint64_t color_created, depth_surface_created, color_refreshed, depth_refreshed;
    uint64_t color_refresh_reused, depth_refresh_reused;
    uint64_t color_readbacks, depth_readbacks, color_masked_draws, completion_waits;
    uint64_t constant_uploads, constant_reuses;
    uint64_t color_copy_bytes, color_publish_bytes, depth_publish_bytes, depth_convert_bytes;
    uint64_t coverage_mask_updates, coverage_mask_rows;
    uint64_t color_refresh_bytes, color_refresh_full_bytes;
    uint64_t color_bulk_readbacks;
    uint64_t color_clear_publications, depth_clear_publications, clear_readback_bytes_avoided;
    uint64_t texture_created, texture_updated, texture_update_bytes;
    uint64_t texture_evicted, texture_cache_entries, texture_cache_bytes;
    uint64_t texture_decode_failures, texture_create_failures;
    uint64_t fixed_draws, lit_draws, skinned_draws, texgen_draws, texture_matrix_draws;
    uint64_t signed_texture_stages, packed_texture_stages;
    uint64_t window_clip_draws, window_scissor_draws, window_shader_draws, window_exclusive_draws;
    uint64_t line_primitives, point_primitives;
    uint64_t sync_calls, target_evictions;
    uint64_t native_clears, clear_fallbacks;
    uint64_t stream_discards, stream_appends;
    uint64_t alias_copies, alias_copy_bytes, alias_copy_reuses, texture_alias_syncs;
    uint64_t compatible_target_views, partial_target_clears, target_layout_syncs;
    uint64_t memcmp_bytes, scan_fallbacks, scan_steps;
    uint64_t texture_calls, surface_scan_iters, surface_count, depth_surface_count;
    uint64_t async_resolved_flip, async_resolved_publish, async_resolved_sync, async_flip_syncs;
} gpu_timing;

/* The phase timings feed the hitch log (RECOMP_HITCH_MS) and the periodic
 * report (RECOMP_PB_REPORT_MS), or RECOMP_GPU_TIMING asks for them alone.
 * Otherwise no clock is read: a draw passes a dozen timers, and the reads
 * were a few percent of the GPU thread for numbers nobody printed. */
static bool gpu_timing_wanted()
{
    static int wanted = -1;
    if (wanted < 0)
        wanted = std::getenv("RECOMP_HITCH_MS") || std::getenv("RECOMP_PB_REPORT_MS") ||
                 std::getenv("RECOMP_GPU_TIMING");
    return wanted != 0;
}

struct GpuTimer {
    double *total;
    std::chrono::steady_clock::time_point start;
    explicit GpuTimer(double &target) : total(gpu_timing_wanted() ? &target : nullptr) {
        if (total) start = std::chrono::steady_clock::now();
    }
    void next_phase(double &target) {
        if (!total) return;
        auto now = std::chrono::steady_clock::now();
        *total += std::chrono::duration<double>(now - start).count();
        total = &target;
        start = now;
    }
    ~GpuTimer() {
        if (total) *total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
};

static void report_gpu_samples();

/* RECOMP_HITCH_MS: what the backend did since the previous flip -- the time
 * in the costly one-off kinds of work -- for the hitch log. Called every
 * flip; writes only when asked. */
extern "C" void nv2a_gpu_frame_summary(char *text, size_t size, int write)
{
    static decltype(gpu_timing) previous;
    const auto &now = gpu_timing;
    if (write && text && size)
        std::snprintf(text, size,
                      "gpu: draw %.1f ms (shaders %.1f, textures %.1f, submit %.1f), compile vs %llu/%.1f ms ps %llu/%.1f ms, "
                      "textures %llu new %llu updated %.2f MiB, hash %.1f ms, sync %.1f ms (%llu), targets %llu+%llu new, generic draws %llu, skipped draws %llu",
                      (now.draw - previous.draw) * 1e3, (now.draw_shaders - previous.draw_shaders) * 1e3,
                      (now.draw_textures - previous.draw_textures) * 1e3, (now.draw_submit - previous.draw_submit) * 1e3,
                      (unsigned long long)(now.vertex_shader_misses - previous.vertex_shader_misses),
                      (now.vertex_compile - previous.vertex_compile) * 1e3,
                      (unsigned long long)(now.pixel_shader_misses - previous.pixel_shader_misses),
                      (now.pixel_compile - previous.pixel_compile) * 1e3,
                      (unsigned long long)(now.texture_created - previous.texture_created),
                      (unsigned long long)(now.texture_updated - previous.texture_updated),
                      (double)(now.texture_update_bytes - previous.texture_update_bytes) / 1048576.0,
                      (now.hash - previous.hash) * 1e3, (now.sync - previous.sync) * 1e3,
                      (unsigned long long)(now.sync_calls - previous.sync_calls),
                      (unsigned long long)(now.color_created - previous.color_created),
                      (unsigned long long)(now.depth_surface_created - previous.depth_surface_created),
                      (unsigned long long)(now.pixel_generic_draws - previous.pixel_generic_draws),
                      (unsigned long long)(now.vertex_skipped_draws - previous.vertex_skipped_draws));
    previous = now;
}

extern "C" void nv2a_gpu_report(void)
{
    report_gpu_samples();
    std::fprintf(stderr, "[GPU-D3D11] time: draw %.3fs texture %.3fs hash %.3fs upload %.3fs sync %.3fs state %.3fs; hashed %.3f GiB\n",
                 gpu_timing.draw, gpu_timing.texture, gpu_timing.hash, gpu_timing.upload, gpu_timing.sync, gpu_timing.state,
                 (double)gpu_timing.hash_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] draw phases: setup %.3fs textures %.3fs streams %.3fs shaders %.3fs submit %.3fs bookkeeping %.3fs\n",
                 gpu_timing.draw_setup, gpu_timing.draw_textures, gpu_timing.draw_streams,
                 gpu_timing.draw_shaders, gpu_timing.draw_submit, gpu_timing.draw_bookkeeping);
    std::fprintf(stderr, "[GPU-D3D11] get_texture: %llu calls, %llu surface-scan iters; %llu surfaces, %llu depth surfaces\n",
                 (unsigned long long)gpu_timing.texture_calls, (unsigned long long)gpu_timing.surface_scan_iters,
                 (unsigned long long)gpu_timing.surface_count, (unsigned long long)gpu_timing.depth_surface_count);
    std::fprintf(stderr, "[GPU-D3D11] states created: %llu blend, %llu depth, %llu sampler\n",
                 (unsigned long long)gpu_timing.blend_created, (unsigned long long)gpu_timing.depth_created,
                 (unsigned long long)gpu_timing.sampler_created);
    std::fprintf(stderr, "[GPU-D3D11] texture uploads: %llu created, %llu updated; %.3f GiB updated; %llu decode failures, %llu create failures\n",
                 (unsigned long long)gpu_timing.texture_created, (unsigned long long)gpu_timing.texture_updated,
                 (double)gpu_timing.texture_update_bytes / 1073741824.0,
                 (unsigned long long)gpu_timing.texture_decode_failures,
                 (unsigned long long)gpu_timing.texture_create_failures);
    std::fprintf(stderr, "[GPU-D3D11] texture cache: %llu entries, %.1f MiB, %llu evictions\n",
                 (unsigned long long)gpu_timing.texture_cache_entries,
                 (double)gpu_timing.texture_cache_bytes / 1048576.0,
                 (unsigned long long)gpu_timing.texture_evicted);
    std::fprintf(stderr, "[GPU-D3D11] surfaces: %llu color created, %llu depth created; %llu color refreshed, %llu depth refreshed\n",
                 (unsigned long long)gpu_timing.color_created, (unsigned long long)gpu_timing.depth_surface_created,
                 (unsigned long long)gpu_timing.color_refreshed, (unsigned long long)gpu_timing.depth_refreshed);
    std::fprintf(stderr, "[GPU-D3D11] target cache: %llu evictions; %llu synchronization calls\n",
                 (unsigned long long)gpu_timing.target_evictions, (unsigned long long)gpu_timing.sync_calls);
    std::fprintf(stderr, "[GPU-D3D11] clears: %llu native, %llu CPU-path requests\n",
                 (unsigned long long)gpu_timing.native_clears, (unsigned long long)gpu_timing.clear_fallbacks);
    std::fprintf(stderr, "[GPU-D3D11] streams: %llu discard maps, %llu append maps\n",
                 (unsigned long long)gpu_timing.stream_discards, (unsigned long long)gpu_timing.stream_appends);
    std::fprintf(stderr, "[GPU-D3D11] texture validate: %.3f GiB memcmp, %llu scan fallbacks, %llu scan steps\n",
                 (double)gpu_timing.memcmp_bytes / 1073741824.0,
                 (unsigned long long)gpu_timing.scan_fallbacks, (unsigned long long)gpu_timing.scan_steps);
    std::fprintf(stderr, "[GPU-D3D11] source aliases: %llu GPU copies, %.3f GiB, %llu reused; %llu CPU synchronization fallbacks\n",
                 (unsigned long long)gpu_timing.alias_copies, (double)gpu_timing.alias_copy_bytes / 1073741824.0,
                 (unsigned long long)gpu_timing.alias_copy_reuses, (unsigned long long)gpu_timing.texture_alias_syncs);
    std::fprintf(stderr, "[GPU-D3D11] compatible targets: %llu reused views, %llu rectangle clears, %llu layout synchronizations\n",
                 (unsigned long long)gpu_timing.compatible_target_views,
                 (unsigned long long)gpu_timing.partial_target_clears,
                 (unsigned long long)gpu_timing.target_layout_syncs);
    std::fprintf(stderr, "[GPU-D3D11] unchanged refreshes: %llu color, %llu depth\n",
                 (unsigned long long)gpu_timing.color_refresh_reused, (unsigned long long)gpu_timing.depth_refresh_reused);
    std::fprintf(stderr, "[GPU-D3D11] readback: copy %.3fs wait %.3fs publish %.3fs unmap %.3fs\n",
                 gpu_timing.sync_copy, gpu_timing.sync_wait, gpu_timing.sync_publish, gpu_timing.sync_unmap);
    std::fprintf(stderr, "[GPU-D3D11] publication: color %.3fs depth %.3fs\n",
                 gpu_timing.color_publish, gpu_timing.depth_publish);
    std::fprintf(stderr, "[GPU-D3D11] bulk color readbacks: %llu\n",
                 (unsigned long long)gpu_timing.color_bulk_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] known clears: %llu color, %llu depth; %.3f GiB readback avoided\n",
                 (unsigned long long)gpu_timing.color_clear_publications,
                 (unsigned long long)gpu_timing.depth_clear_publications,
                 (double)gpu_timing.clear_readback_bytes_avoided / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] completion: %.3fs in %llu event waits; %llu color-masked draws; readbacks %llu color, %llu depth\n",
                 gpu_timing.sync_event_wait, (unsigned long long)gpu_timing.completion_waits,
                 (unsigned long long)gpu_timing.color_masked_draws, (unsigned long long)gpu_timing.color_readbacks,
                 (unsigned long long)gpu_timing.depth_readbacks);
    std::fprintf(stderr, "[GPU-D3D11] async flip: staged frame resolved by flip %llu, targeted publish %llu, full sync %llu; synchronous flips %llu\n",
                 (unsigned long long)gpu_timing.async_resolved_flip,
                 (unsigned long long)gpu_timing.async_resolved_publish,
                 (unsigned long long)gpu_timing.async_resolved_sync,
                 (unsigned long long)gpu_timing.async_flip_syncs);
    std::fprintf(stderr, "[GPU-D3D11] constants: %llu uploads, %llu unchanged reuses, %.3fs\n",
                 (unsigned long long)gpu_timing.constant_uploads,
                 (unsigned long long)gpu_timing.constant_reuses, gpu_timing.constants);
    std::fprintf(stderr, "[GPU-D3D11] shaders: vertex %llu hits/%llu misses/%llu failures, %.3fs compile; pixel %llu hits/%llu misses/%llu failures, %.3fs compile; %llu evictions\n",
                 (unsigned long long)gpu_timing.vertex_shader_hits, (unsigned long long)gpu_timing.vertex_shader_misses,
                 (unsigned long long)gpu_timing.vertex_shader_failures,
                 gpu_timing.vertex_compile, (unsigned long long)gpu_timing.pixel_shader_hits,
                 (unsigned long long)gpu_timing.pixel_shader_misses,
                 (unsigned long long)gpu_timing.pixel_shader_failures, gpu_timing.pixel_compile,
                 (unsigned long long)gpu_timing.shader_evictions);
    std::fprintf(stderr, "[GPU-D3D11] shader disk cache: %llu loaded; background compiles: %llu pixel (%llu draws with the generic shader meanwhile), %llu vertex (%llu draws skipped meanwhile)\n",
                 (unsigned long long)gpu_timing.shader_disk_hits, (unsigned long long)gpu_timing.pixel_async_compiles,
                 (unsigned long long)gpu_timing.pixel_generic_draws, (unsigned long long)gpu_timing.vertex_async_compiles,
                 (unsigned long long)gpu_timing.vertex_skipped_draws);
    std::fprintf(stderr, "[GPU-D3D11] fixed function: %llu draws, %llu lit, %llu skinned, %llu texgen, %llu texture-matrix\n",
                 (unsigned long long)gpu_timing.fixed_draws, (unsigned long long)gpu_timing.lit_draws,
                 (unsigned long long)gpu_timing.skinned_draws, (unsigned long long)gpu_timing.texgen_draws,
                 (unsigned long long)gpu_timing.texture_matrix_draws);
    std::fprintf(stderr, "[GPU-D3D11] texture state: %llu signed-channel stages, %llu expanded-format stages\n",
                 (unsigned long long)gpu_timing.signed_texture_stages, (unsigned long long)gpu_timing.packed_texture_stages);
    std::fprintf(stderr, "[GPU-D3D11] native primitives: %llu lines, %llu points\n",
                 (unsigned long long)gpu_timing.line_primitives, (unsigned long long)gpu_timing.point_primitives);
    std::fprintf(stderr, "[GPU-D3D11] window clip: %llu configured draws, %llu scissor-only, %llu pixel-union, %llu exclusive\n",
                 (unsigned long long)gpu_timing.window_clip_draws, (unsigned long long)gpu_timing.window_scissor_draws,
                 (unsigned long long)gpu_timing.window_shader_draws, (unsigned long long)gpu_timing.window_exclusive_draws);
    std::fprintf(stderr, "[GPU-D3D11] surface bytes: color copied %.3f GiB published %.3f GiB; depth published %.3f GiB converted %.3f GiB\n",
                 (double)gpu_timing.color_copy_bytes / 1073741824.0, (double)gpu_timing.color_publish_bytes / 1073741824.0,
                 (double)gpu_timing.depth_publish_bytes / 1073741824.0, (double)gpu_timing.depth_convert_bytes / 1073741824.0);
    std::fprintf(stderr, "[GPU-D3D11] coverage: %llu bitmap updates, %llu bitmap publication rows\n",
                 (unsigned long long)gpu_timing.coverage_mask_updates, (unsigned long long)gpu_timing.coverage_mask_rows);
    std::fprintf(stderr, "[GPU-D3D11] color refresh: %.3f GiB submitted, %.3f GiB full-surface equivalent\n",
                 (double)gpu_timing.color_refresh_bytes / 1073741824.0,
                 (double)gpu_timing.color_refresh_full_bytes / 1073741824.0);
}

static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> context;
/* D3D11.1 context, if the device supports constant-buffer offsetting. Lets the
 * whole constant buffer live in one dynamic ring bound by offset, instead of a
 * Map(DISCARD)/Unmap rename per draw. Null => fall back to the single buffer. */
static ComPtr<ID3D11DeviceContext1> context1;
static ComPtr<ID3D11Query> completion_event;
static bool pending_draws;

struct GpuDrawSample {
    ComPtr<ID3D11Query> disjoint, begin, end, statistics;
    uint32_t kind = 0;
    bool pending = false;
};

static std::array<GpuDrawSample,16> gpu_draw_samples;
struct GpuSampleBucket {
    uint64_t samples = 0, vertex_invocations = 0, geometry_invocations = 0, pixel_invocations = 0;
    double seconds = 0;
};
static struct {
    std::array<GpuSampleBucket,8> buckets;
    uint64_t draws = 0, busy = 0, disjoint = 0;
    uint32_t next = 0;
} gpu_samples;

static bool gpu_sampling_enabled()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_NV2A_GPU_TIMING");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static void gpu_sample_failed(const char *operation, HRESULT result)
{
    std::fprintf(stderr, "[GPU-D3D11] GPU profiling %s failed: 0x%08X; device 0x%08X\n",
                 operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

static bool collect_gpu_sample(GpuDrawSample &sample)
{
    if (!sample.pending) return true;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency = {};
    UINT64 begin = 0, end = 0;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS statistics = {};
    auto read = [&](ID3D11Query *query, void *data, UINT bytes) {
        HRESULT result = context->GetData(query, data, bytes, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(result)) gpu_sample_failed("read", result);
        return result == S_OK;
    };
    if (!read(sample.disjoint.Get(), &frequency, sizeof frequency) ||
        !read(sample.begin.Get(), &begin, sizeof begin) ||
        !read(sample.end.Get(), &end, sizeof end) ||
        !read(sample.statistics.Get(), &statistics, sizeof statistics)) return false;
    sample.pending = false;
    if (frequency.Disjoint || !frequency.Frequency || end < begin) {
        gpu_samples.disjoint++;
        return true;
    }
    auto &bucket = gpu_samples.buckets[sample.kind];
    bucket.samples++;
    bucket.seconds += (double)(end - begin) / frequency.Frequency;
    bucket.vertex_invocations += statistics.VSInvocations;
    bucket.geometry_invocations += statistics.GSInvocations;
    bucket.pixel_invocations += statistics.PSInvocations;
    return true;
}

static GpuDrawSample *begin_gpu_sample(const Nv2aGpuDraw &state)
{
    if (!gpu_sampling_enabled() || (++gpu_samples.draws % 128) != 0) return nullptr;
    auto &sample = gpu_draw_samples[gpu_samples.next++ % gpu_draw_samples.size()];
    if (!collect_gpu_sample(sample)) {
        gpu_samples.busy++;
        return nullptr;
    }
    if (!sample.disjoint) {
        auto create = [&](D3D11_QUERY type, ComPtr<ID3D11Query> &query) {
            D3D11_QUERY_DESC description = {type, 0};
            HRESULT result = device->CreateQuery(&description, &query);
            if (FAILED(result)) gpu_sample_failed("create", result);
        };
        create(D3D11_QUERY_TIMESTAMP_DISJOINT, sample.disjoint);
        create(D3D11_QUERY_TIMESTAMP, sample.begin);
        create(D3D11_QUERY_TIMESTAMP, sample.end);
        create(D3D11_QUERY_PIPELINE_STATISTICS, sample.statistics);
    }
    sample.kind = (state.vertex_program ? 1u : 0u) |
                  (state.fixed_transform && state.lighting_enable ? 2u : 0u) |
                  (state.control0 & 0x10000u ? 4u : 0u);
    context->Begin(sample.disjoint.Get());
    context->Begin(sample.statistics.Get());
    context->End(sample.begin.Get());
    return &sample;
}

static void end_gpu_sample(GpuDrawSample *sample)
{
    if (!sample) return;
    context->End(sample->end.Get());
    context->End(sample->statistics.Get());
    context->End(sample->disjoint.Get());
    sample->pending = true;
}

static void report_gpu_samples()
{
    if (!gpu_sampling_enabled() || !context) return;
    for (auto &sample : gpu_draw_samples) collect_gpu_sample(sample);
    static std::array<GpuSampleBucket,8> previous;
    for (uint32_t kind = 0; kind < gpu_samples.buckets.size(); kind++) {
        const auto &bucket = gpu_samples.buckets[kind];
        uint64_t samples = bucket.samples - previous[kind].samples;
        if (!samples) continue;
        std::fprintf(stderr, "[GPU-D3D11] GPU sample: program=%u lit=%u wdepth=%u interval samples=%llu draw=%.3fus mean; VS=%llu GS=%llu PS=%llu invocations\n",
                     kind & 1u, (kind >> 1) & 1u, (kind >> 2) & 1u,
                     (unsigned long long)samples,
                     (bucket.seconds - previous[kind].seconds) * 1000000 / samples,
                     (unsigned long long)(bucket.vertex_invocations - previous[kind].vertex_invocations),
                     (unsigned long long)(bucket.geometry_invocations - previous[kind].geometry_invocations),
                     (unsigned long long)(bucket.pixel_invocations - previous[kind].pixel_invocations));
        previous[kind] = bucket;
    }
    std::fprintf(stderr, "[GPU-D3D11] GPU sampling: every 128th draw; %llu busy skips, %llu disjoint samples discarded\n",
                 (unsigned long long)gpu_samples.busy, (unsigned long long)gpu_samples.disjoint);
}
static ComPtr<ID3D11VertexShader> vertex_shader;
static ComPtr<ID3D11GeometryShader> geometry_shader;
static ComPtr<ID3D11GeometryShader> line_geometry_shader, point_geometry_shader;
static ComPtr<ID3D11PixelShader> uncombined_shader;
static ComPtr<ID3DBlob> vertex_code;

struct TargetExtent {
    uint32_t width = 0, height = 0;
};

/* A target's width and height are the title's (guest RAM's); texture_width
 * and texture_height are its texture's, larger by RECOMP_RENDER_SCALE. A scaled
 * target reaches guest RAM through `resolve` (its guest-size copy, drawn down
 * from the texture before a publish) and takes guest RAM through `upload`
 * (drawn up into the texture by a refresh). */
struct Surface {
    uint8_t *memory;
    uint32_t width, height, pitch;
    uint32_t texture_width, texture_height;
    ComPtr<ID3D11Texture2D> resolve, upload;
    ComPtr<ID3D11RenderTargetView> resolve_target;
    ComPtr<ID3D11ShaderResourceView> upload_view;
    bool dirty, needs_refresh, snapshot_valid;
    bool clear_value_valid;
    uint32_t clear_value;
    uint64_t last_used, content_serial, alias_copy_serial;
    D3D11_BOX written_region;
    std::vector<uint64_t> written_mask;
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11ShaderResourceView> view;
    /* MSAA (nv2a_gpu_set_msaa): draws go to the multisampled twin; `texture`
     * is resolved from it before anything reads it (ms_newer), and drawn back
     * into it after anything else writes `texture` (ms_stale). */
    ComPtr<ID3D11Texture2D> ms_texture;
    ComPtr<ID3D11RenderTargetView> ms_target;
    bool ms_newer, ms_stale;
};

struct CachedTexture {
    const uint8_t *source;
    uint32_t width, height, format, cube, pitch, linear, face_stride, depth;
    uint32_t mip_levels;
    uint64_t bytes;
    uint64_t validated_serial;   /* frame serial this entry's memcmp last matched */
    bool force_validate;         /* a GPU blit overwrote this image; re-check now */
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11ShaderResourceView> view;
};

struct DepthSurface {
    uint8_t *memory;
    uint32_t width, height, pitch, format;
    uint32_t texture_width, texture_height;   /* as Surface */
    ComPtr<ID3D11Texture2D> resolve, upload;
    ComPtr<ID3D11DepthStencilView> resolve_view;
    ComPtr<ID3D11ShaderResourceView> upload_view;
    bool dirty, needs_refresh, snapshot_valid;
    bool clear_value_valid;
    uint32_t clear_value;
    uint64_t last_used, content_serial;
    D3D11_BOX written_region;
    std::vector<uint64_t> written_mask;
    std::vector<uint8_t> source_snapshot;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID3D11DepthStencilView> view;
    ComPtr<ID3D11ShaderResourceView> sample_view;   /* depth as a texture: shadow-map stages */
    ComPtr<ID3D11Texture2D> ms_texture;             /* as Surface */
    ComPtr<ID3D11DepthStencilView> ms_view;
    ComPtr<ID3D11ShaderResourceView> ms_sample_view;
    bool ms_newer, ms_stale;
};

/* RECOMP_RENDER_SCALE=<n> (1 to 8, may be fractional): colour targets, and
 * the depth targets drawn with them, are n times the title's size on each
 * axis. Vertex positions arrive in the title's pixels and are mapped through
 * its viewport size, so geometry fills the larger target by itself; scissors,
 * window clips and target-to-target copies are scaled where they are set. A
 * depth target only ever drawn without colour (a shadow map) keeps its size.
 *
 * Unset or "auto": the whole multiple of 480 lines that covers the primary
 * monitor's height (3 on 1440p), so the frame is rendered at least at the
 * screen's own resolution; above 1920 lines, the monitor's own (4.5 on 4K).
 *
 * nv2a_gpu_set_render_scale changes it while the title runs; the change waits
 * for the next flip, where every target is redrawn at the new size. */
static float automatic_render_scale()
{
    DEVMODEA mode = {};
    mode.dmSize = sizeof mode;
    if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) || !mode.dmPelsHeight) return 1.0f;
    float automatic = mode.dmPelsHeight > 1920 ? std::min(mode.dmPelsHeight / 480.0f, 8.0f)
                                                : (float)((mode.dmPelsHeight + 479) / 480);
    std::fprintf(stderr, "[GPU-D3D11] render scale %gx (monitor %lux%lu)\n", automatic,
                 mode.dmPelsWidth, mode.dmPelsHeight);
    return automatic;
}

static float requested_render_scale(float requested)
{
    if (requested == 0.0f) return automatic_render_scale();
    return requested >= 1.0f && requested <= 8.0f ? requested : 1.0f;
}

static float current_render_scale;
/* Set by any thread, taken at a flip: the scale asked for (0 automatic), or -1
 * for no change. */
static std::atomic<float> pending_render_scale{-1.0f};

static float render_scale()
{
    if (current_render_scale == 0.0f) {
        const char *value = std::getenv("RECOMP_RENDER_SCALE");
        current_render_scale = requested_render_scale(
            !value || !*value || !_stricmp(value, "auto") ? 0.0f : (float)std::atof(value));
    }
    return current_render_scale;
}

/* Widescreen (nv2a_gpu_set_wide_factor): screen-shaped targets -- the
 * display mode's size, or twice it on an axis for an anti-aliased surface --
 * are wider than the title made them by `wide_factor` (16:9 is 4/3 of 4:3).
 * Positions still arrive in the title's 4:3 pixels, so the picture is
 * stretched across the target; the draw undoes the stretch:
 *
 *   - a 3D draw has the x row of its projection divided by the factor, which
 *     widens the view instead (Hor+). Turok's rigid programs project through
 *     c[96], its skinned ones through c[187] (their bone palette is at
 *     c[96] + a0), and the fixed-function transform through c[0];
 *   - a 2D draw (a program that projects through neither) is squeezed back to
 *     the middle 4:3 by the geometry shader, triangle by triangle, unless the
 *     triangle reaches the left or right edge: backdrops, fades and edge
 *     effects span the screen, the HUD and text keep their shape. A draw
 *     that reaches both edges is left whole (nv2a_pb_exec_batch_spans), so
 *     a full-screen effect drawn in strips stays in one piece.
 *
 * A frame with no 3D draw on the screen (a loading screen, a menu) is a 2D
 * picture made for 4:3: two in a row and the targets go back to 4:3 until a
 * 3D draw comes back. */
/* MSAA: samples per pixel of the screen-shaped targets, 1 for none. */
static UINT msaa_samples = 1;
static std::atomic<int> pending_msaa{-1};

static float wide_factor = 1.0f;
static std::atomic<float> pending_wide_factor{-1.0f};
static bool wide_frames = true;             /* this frame's targets are wide */
static unsigned frames_without_3d, screen_3d_draws;

extern "C" void xbox_FramebufferSize(uint32_t *width, uint32_t *height);

static bool screen_shaped(uint32_t width, uint32_t height)
{
    uint32_t screen_width = 640, screen_height = 480;
    xbox_FramebufferSize(&screen_width, &screen_height);
    return screen_height && (height == screen_height || height == 2 * screen_height) &&
           (width == screen_width || width == 2 * screen_width);
}

static UINT extent_at(uint32_t size, float factor)
{
    if (factor == 1.0f) return size;
    return std::min<UINT>((UINT)((float)size * factor + 0.5f), D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
}

/* A target's texture size: `scaled` false for a depth-only target. */
static void texture_size(uint32_t width, uint32_t height, bool scaled, UINT &texture_width, UINT &texture_height)
{
    float scale = scaled ? render_scale() : 1.0f;
    float wide = scaled && wide_frames && screen_shaped(width, height) ? wide_factor : 1.0f;
    texture_width = extent_at(width, scale * wide);
    texture_height = extent_at(height, scale);
}

template<typename Target>
static bool is_scaled(const Target &target)
{
    return target.texture_width != target.width || target.texture_height != target.height;
}

/* Depth targets seen in a depth-only draw: those stay at the title's size. */
static std::set<const uint8_t *> depth_only_targets;

static bool depth_scaled(const uint8_t *memory)
{
    return !depth_only_targets.count(memory);
}

/* One edge of a rectangle, from one target's pixels to another's. */
static LONG scale_edge(LONG value, uint32_t from, uint32_t to)
{
    return from == to ? value : (LONG)(((int64_t)value * to + from / 2) / from);
}

struct Constants {
    uint32_t control[4], misc[4], stages[8][4], final_input[4];
    uint32_t texture_control[4];
    float factors[18][4], fog[4], viewport[4], texture_info[4][4];
    float bump_matrix[4][4], bump_luminance[4][4];
    float fog_parameters[4];
    uint32_t fog_control[4];
    float fog_plane[4];
    uint32_t texture_key[4][4];
    float depth_range[4];
    float depth_offset[4];
    float shader_eye_vector[4];
    float vertex_constants[192][4];
    /* Fixed-function lighting. */
    uint32_t light_state[4];     /* x=lighting_enable, y=specular_enable, z=light_enable_mask, w=color_material */
    uint32_t light_control[4];   /* flags, normalization, skin mode, texgen viewer */
    float material[4];           /* xyz=emission, w=alpha */
    float scene_ambient[4];      /* xyz */
    float specular_power[4];     /* x=specular exponent */
    float light_ambient[8][4];
    float light_diffuse[8][4];
    float light_specular[8][4];
    float light_local_position[8][4];    /* xyz=position, w=range */
    float light_local_attenuation[8][4]; /* xyz=constant/linear/quadratic */
    float light_infinite_direction[8][4];
    float light_infinite_half_vector[8][4];
    float light_spot_direction[8][4];    /* xyz=direction, w=spot parameter */
    uint32_t texgen[4][4], texture_matrix_enable[4];
    uint32_t window_clip_control[4], window_clip_rects[8][4];
};

static_assert(sizeof(Constants) % 16 == 0 && sizeof(Constants) <= 65536, "D3D11 constant-buffer extent");

static std::vector<Surface> surfaces;
static std::vector<DepthSurface> depth_surfaces;
static uint64_t surface_use_serial;
static uint64_t target_content_serial;
static std::vector<CachedTexture> textures;
/* Bind-time lookup is limited to entries with the same guest source address.
 * Rebuild after insertion, even when eviction keeps the cache size unchanged. */
static std::unordered_multimap<const void *, size_t> texture_index_by_source;
static size_t texture_index_count = (size_t)-1;
static void rebuild_texture_index()
{
    texture_index_by_source.clear();
    for (size_t i = 0; i < textures.size(); i++)
        texture_index_by_source.emplace(textures[i].source, i);
    texture_index_count = textures.size();
}
static ComPtr<ID3D11InputLayout> input_layout;
static ComPtr<ID3D11Buffer> constant_buffer;
static Constants uploaded_constants;
static bool uploaded_constants_valid;
/* Offset within constant_stream.buffer of the currently uploaded_constants. */
static UINT constant_upload_offset;
struct DynamicStream {
    ComPtr<ID3D11Buffer> buffer;
    UINT capacity = 0, used = 0;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    bool active = false;   /* mapped right now; always unmapped before a draw */
    uint64_t generation = 0;   /* bumped by every DISCARD: older offsets are gone */
};
/* D3D11.1 driver capability: NO_OVERWRITE appends to a dynamic constant buffer.
 * Without it every constant upload maps with DISCARD at offset 0. */
static bool constant_no_overwrite;
/* constant_stream.generation when uploaded_constants was written. */
static uint64_t constant_upload_generation;
static std::array<DynamicStream,2> vertex_streams;
static DynamicStream constant_stream;
static DynamicStream index_stream;
/* Ring slot for one Constants upload, 256-byte aligned as VSSetConstantBuffers1
 * requires. NumConstants must cover the whole HLSL cbuffer. */
static constexpr UINT kConstantSlot = (UINT)((sizeof(Constants) + 255u) & ~255u);
static constexpr UINT kConstantCount = kConstantSlot / 16u;
static void release_stream(DynamicStream &stream);
static ComPtr<ID3D11RasterizerState> rasterizer;
static std::array<ComPtr<ID3D11RasterizerState>,12> raster_states;

struct BlendVariant {
    std::array<uint32_t,7> key;
    ComPtr<ID3D11BlendState> state;
};
static std::vector<BlendVariant> blend_variants;
static std::array<ComPtr<ID3D11DepthStencilState>,32> depth_states;
struct StencilVariant {
    std::array<uint32_t,7> key;
    ComPtr<ID3D11DepthStencilState> state;
};
static std::vector<StencilVariant> stencil_states;
struct SamplerVariant {
    std::array<uint32_t,9> key;
    ComPtr<ID3D11SamplerState> state;
};
static std::vector<SamplerVariant> sampler_cache;

struct ShaderVariant {
    std::array<uint32_t,43> key;
    ComPtr<ID3D11PixelShader> shader;
    uint64_t last_used, bytecode_bytes;
};
static std::vector<ShaderVariant> shader_variants;

struct VertexVariant {
    std::vector<uint32_t> key;
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
    uint32_t input_mask = 0;   /* the packed attributes its layout reads */
    uint64_t last_used, bytecode_bytes;
};
static std::vector<VertexVariant> vertex_variants;

static uint64_t shader_use_serial;

template<typename Variant>
static Variant *cache_shader_variant(std::vector<Variant> &cache, Variant &&variant)
{
    constexpr uint64_t budget = 16 * 1024 * 1024;
    constexpr size_t maximum_entries = 512;
    if (variant.bytecode_bytes > budget) {
        std::fprintf(stderr, "[GPU-D3D11] shader exceeds bytecode cache budget: %llu bytes\n",
                     (unsigned long long)variant.bytecode_bytes);
        return nullptr;
    }
    uint64_t bytes = 0;
    for (const auto &entry : cache) bytes += entry.bytecode_bytes;
    while (!cache.empty() && (cache.size() >= maximum_entries || bytes + variant.bytecode_bytes > budget)) {
        auto oldest = std::min_element(cache.begin(), cache.end(),
            [](const Variant &left, const Variant &right) { return left.last_used < right.last_used; });
        bytes -= oldest->bytecode_bytes;
        cache.erase(oldest);
        gpu_timing.shader_evictions++;
    }
    variant.last_used = ++shader_use_serial;
    cache.push_back(std::move(variant));
    return &cache.back();
}

static std::string vertex_mask(uint32_t mask)
{
    std::string result;
    const char components[] = "xyzw";
    for (uint32_t component = 0; component < 4; component++)
        if (mask & (8u >> component)) result += components[component];
    return result;
}

static std::string vertex_source(uint32_t mux, uint32_t temporary, uint32_t swizzle,
                                 uint32_t negate, uint32_t input, uint32_t constant, bool relative)
{
    std::string result;
    switch (mux) {
    case 1:
        if (temporary > 12) return {};
        result = temporary == 12 ? "outputs[0]" : "temporary[" + std::to_string(temporary) + "]";
        break;
    case 2: result = "input.attribute" + std::to_string(input); break;
    case 3:
        if (!relative && constant >= 192) return {};
        result = "vertexConstants[" + std::to_string(constant) + (relative ? "+address]" : "]");
        break;
    default: return {};
    }
    result += ".";
    const char components[] = "xyzw";
    for (uint32_t component = 0; component < 4; component++) result += components[(swizzle >> (6 - component * 2)) & 3];
    return negate ? "-(" + result + ")" : result;
}

static uint32_t vertex_program_word_count(const Nv2aGpuDraw &state)
{
    if (!state.vertex_program || !state.vertex_valid || !state.vertex_constants || state.vertex_start >= 136) return 0;
    for (uint32_t slot = state.vertex_start; slot < 136; slot++) {
        if (state.vertex_valid[slot] != 15) return 0;
        if (state.vertex_program[slot][3] & 1u) return (slot - state.vertex_start + 1) * 4;
    }
    return 0;
}

static bool vertex_program_key(const Nv2aGpuDraw &state, std::vector<uint32_t> &key)
{
    uint32_t words = vertex_program_word_count(state);
    if (!words) return false;
    key.resize(words);
    std::memcpy(key.data(), state.vertex_program + state.vertex_start, words * sizeof(uint32_t));
    return true;
}

static std::string vertex_program_source(const std::vector<uint32_t> &key, bool state_program)
{
    std::string source = state_program ? "" : "#define NV_FIXED_TRANSFORM 0\n";
    source += nv2a_gpu_shader_source;
    source += "\nstruct GuestVertex {\n";
    for (uint32_t attribute = 0; attribute < 16; attribute++)
        source += "float4 attribute" + std::to_string(attribute) + ":TEXCOORD" + std::to_string(attribute) + ";\n";
    source += "};\n";
    if (state_program) {
        source += "RWStructuredBuffer<float4> stateConstants:register(u0); cbuffer Attributes:register(b1) {float4 stateAttributes[16];};\n#define vertexConstants stateConstants\n[numthreads(1,1,1)]void cs_guest(uint3 dispatch:SV_DispatchThreadID) { GuestVertex input;\n";
        for (uint32_t attribute = 0; attribute < 16; attribute++)
            source += "input.attribute" + std::to_string(attribute) + "=stateAttributes[" + std::to_string(attribute) + "];\n";
    } else source += "Pixel vs_guest(GuestVertex input) {\n";
    source += "float4 temporary[13]; float4 outputs[16]; int address=0;\n";
    source += "[unroll]for(uint index=0;index<13;index++)temporary[index]=0; [unroll]for(uint index=0;index<16;index++)outputs[index]=float4(0,0,0,1);\n";
    for (size_t instruction = 0; instruction < key.size(); instruction += 4) {
        const uint32_t *words = key.data() + instruction;
        uint32_t mac = (words[1] >> 21) & 15, ilu = (words[1] >> 25) & 7;
        uint32_t input = (words[1] >> 9) & 15, constant = (words[1] >> 13) & 255;
        uint32_t destination = (words[3] >> 20) & 15, output = (words[3] >> 3) & 255;
        uint32_t mac_mask = (words[3] >> 24) & 15, ilu_mask = (words[3] >> 16) & 15, output_mask = (words[3] >> 12) & 15;
        bool relative = (words[3] & 2) != 0;
        if (mac > 13) return {};
        uint32_t usage = nv_cpu_vp_operand_usage(mac, ilu);
        std::string operand_a = usage & 1u ? vertex_source((words[2] >> 26) & 3, words[2] >> 28, words[1] & 255, (words[1] >> 8) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_b = usage & 2u ? vertex_source((words[2] >> 11) & 3, (words[2] >> 13) & 15, (words[2] >> 17) & 255, (words[2] >> 25) & 1, input, constant, relative) : "float4(0,0,0,0)";
        std::string operand_c = usage & 4u ? vertex_source((words[3] >> 28) & 3, ((words[2] & 3) << 2) | (words[3] >> 30), (words[2] >> 2) & 255, (words[2] >> 10) & 1, input, constant, relative) : "float4(0,0,0,0)";
        if (operand_a.empty() || operand_b.empty() || operand_c.empty()) return {};
        source += "{ float4 aa=" + operand_a + ",bb=" + operand_b + ",cc=" + operand_c + "; precise float4 macResult=0; float4 iluResult=0;\n";
        switch (mac) {
        case 0: case 13: break;
        case 1: source += "macResult=aa;\n"; break;
        case 2: source += "macResult=guest_multiply(aa,bb);\n"; break;
        case 3: source += "macResult=aa+cc;\n"; break;
        case 4: source += "macResult=guest_multiply(aa,bb)+cc;\n"; break;
        case 5: case 6: case 7:
            source += "float4 product=guest_multiply(aa,bb); macResult=product.x+product.y+product.z";
            source += mac == 6 ? "+bb.w;\n" : mac == 7 ? "+product.w;\n" : ";\n";
            break;
        case 8: source += "macResult=float4(1,guest_multiply(aa,bb).y,aa.z,bb.w);\n"; break;
        case 9: source += "macResult=min(aa,bb);\n"; break;
        case 10: source += "macResult=max(aa,bb);\n"; break;
        case 11: source += "macResult=float4(aa<bb);\n"; break;
        case 12: source += "macResult=float4(aa>=bb);\n"; break;
        }
        switch (ilu) {
        case 0: break;
        case 1: source += "iluResult=cc;\n"; break;
        case 2: source += "iluResult=1/cc.x;\n"; break;
        case 3: source += "float reciprocal=1/cc.x; iluResult=(reciprocal<0?-1:1)*clamp(abs(reciprocal),5.421010862427522e-20,1.8446744073709552e19);\n"; break;
        case 4: source += "iluResult=1/sqrt(abs(cc.x));\n"; break;
        case 5: source += "iluResult=float4(exp2(floor(cc.x)),cc.x-floor(cc.x),exp2(cc.x),1);\n"; break;
        case 6: source += "float scalar=abs(cc.x); float exponent=log2(scalar); iluResult=float4(floor(exponent),scalar==0?1:scalar/exp2(floor(exponent)),exponent,1);\n"; break;
        case 7: source += "iluResult=float4(1,max(cc.x,0),cc.x>0?pow(max(cc.y,0),clamp(cc.w,-127.99609375,127.99609375)):0,1);\n"; break;
        }
        auto write = [&](const std::string &target, const std::string &result, uint32_t mask) {
            std::string components = vertex_mask(mask);
            source += target + "." + components + "=" + result + "." + components + ";\n";
        };
        bool selected_ilu = (words[3] & 4) != 0;
        if (output_mask && (selected_ilu ? ilu : mac)) {
            std::string result = selected_ilu ? "iluResult" : "macResult";
            if (!(words[3] & 0x800)) {
                if (!state_program || output >= 192) return {};
                write("vertexConstants[" + std::to_string(output) + "]", result, output_mask);
            }
            else if ((output & 15) == 15) source += "address=(int)floor(" + result + ".x);\n";
            else if ((output & 15) == 5) source += "outputs[5].x=" + result + "." + vertex_mask(output_mask).substr(0,1) + ";\n";
            else write("outputs[" + std::to_string(output & 15) + "]", result, output_mask);
        }
        if (mac == 13) source += "address=(int)floor(aa.x);\n";
        else if (mac && mac_mask && !(ilu && destination == 1)) {
            if (destination > 12) return {};
            write(destination == 12 ? "outputs[0]" : "temporary[" + std::to_string(destination) + "]", "macResult", mac_mask);
        }
        if (ilu && ilu_mask) {
            uint32_t ilu_destination = mac ? 1 : destination;
            if (ilu_destination > 12) return {};
            write(ilu_destination == 12 ? "outputs[0]" : "temporary[" + std::to_string(ilu_destination) + "]", "iluResult", ilu_mask);
        }
        source += "}\n";
    }
    if (state_program) source += "}\n";
    else source += "Vertex bridge=(Vertex)0; bridge.position=outputs[0]; bridge.diffuse=saturate(outputs[3]); bridge.specular=saturate(outputs[4]); bridge.tex0=outputs[9]; bridge.tex1=outputs[10]; bridge.tex2=outputs[11]; bridge.tex3=outputs[12]; bridge.fogCoordinate=outputs[5].x; Pixel result=vs_main(bridge); result.fog=fog_factor(outputs[5].x); return result; }\n";
    return source;
}

/* The title's projection registers, most specific first
 * (nv2a_gpu_set_projection_registers): D3D's c0 by default. */
static int projection_registers[4] = {96};
static int projection_register_count = 1;

extern "C" void nv2a_gpu_set_projection_registers(const int *registers, int count)
{
    projection_register_count = 0;
    for (int index = 0; index < count && index < 4; index++)
        if (registers[index] >= 0 && registers[index] < 192)
            projection_registers[projection_register_count++] = registers[index];
}

/* The register a draw projects x through, read directly (not through a0): -1
 * for a draw in screen space. The fixed-function transform's is c[0]. */
static int projection_register(const Nv2aGpuDraw &state)
{
    if (!state.vertex_program) return state.fixed_transform ? 0 : -1;
    uint32_t words = vertex_program_word_count(state), found = 0;
    for (uint32_t slot = state.vertex_start; slot < state.vertex_start + words / 4; slot++) {
        const uint32_t *word = state.vertex_program[slot];
        if (word[3] & 2) continue;
        uint32_t usage = nv_cpu_vp_operand_usage((word[1] >> 21) & 15, (word[1] >> 25) & 7);
        bool reads = (usage & 1 && ((word[2] >> 26) & 3) == 3) || (usage & 2 && ((word[2] >> 11) & 3) == 3) ||
                     (usage & 4 && ((word[3] >> 28) & 3) == 3);
        uint32_t constant = (word[1] >> 13) & 255;
        for (int index = 0; reads && index < projection_register_count; index++)
            if ((int)constant == projection_registers[index]) found |= 1u << index;
    }
    for (int index = 0; index < projection_register_count; index++)
        if (found & (1u << index)) return projection_registers[index];
    return -1;
}

/* Compiled shaders persist on disk: each guest shader variant costs fxc
 * 50-200 ms, paid on the frame that first draws with it -- the stutter the
 * first time a scene shows a new material. With the cache that happens once
 * per machine, not once per session. The key covers the source, the
 * definitions, entry point, profile and flags, so a changed shader simply
 * misses. Directory: RECOMP_SHADER_CACHE, default
 * %LOCALAPPDATA%\xboxrecomp\shadercache; RECOMP_SHADER_CACHE=0 turns it off. */
static const std::wstring &shader_cache_directory()
{
    static const std::wstring directory = [] {
        std::wstring path;
        const char *setting = std::getenv("RECOMP_SHADER_CACHE");
        if (setting && !std::strcmp(setting, "0")) return path;
        if (setting && *setting) {
            int length = MultiByteToWideChar(CP_UTF8, 0, setting, -1, nullptr, 0);
            path.resize(length > 0 ? length - 1 : 0);
            if (length > 1) MultiByteToWideChar(CP_UTF8, 0, setting, -1, &path[0], length);
        } else {
            wchar_t base[MAX_PATH];
            DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
            if (!length || length >= MAX_PATH) return path;
            path = std::wstring(base) + L"\\xboxrecomp";
            CreateDirectoryW(path.c_str(), nullptr);
            path += L"\\shadercache";
        }
        CreateDirectoryW(path.c_str(), nullptr);
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) path.clear();
        return path;
    }();
    return directory;
}

static void shader_key_mix(uint64_t key[2], const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t index = 0; index < size; index++) {
        key[0] = (key[0] ^ bytes[index]) * 0x100000001B3ull;
        key[1] = (key[1] ^ bytes[index]) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    }
    key[0] = (key[0] ^ size) * 0x100000001B3ull;   /* separates the fields */
}

static HRESULT compile_shader_cached(const char *source, size_t size, const char *name, const D3D_SHADER_MACRO *defines,
                                     const char *entry, const char *profile, UINT flags,
                                     ComPtr<ID3DBlob> &code, ComPtr<ID3DBlob> &errors, bool compile = true)
{
    const std::wstring &directory = shader_cache_directory();
    std::wstring path;
    if (!directory.empty()) {
        uint64_t key[2] = {0xCBF29CE484222325ull, 0x84222325CBF29CE4ull};
        shader_key_mix(key, source, size);
        shader_key_mix(key, entry, std::strlen(entry));
        shader_key_mix(key, profile, std::strlen(profile));
        shader_key_mix(key, &flags, sizeof flags);
        for (const D3D_SHADER_MACRO *macro = defines; macro && macro->Name; macro++) {
            shader_key_mix(key, macro->Name, std::strlen(macro->Name));
            shader_key_mix(key, macro->Definition, std::strlen(macro->Definition));
        }
        wchar_t file[40];
        swprintf(file, 40, L"\\%016llx%016llx.dxbc", (unsigned long long)key[0], (unsigned long long)key[1]);
        path = directory + file;
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER length = {};
            DWORD read = 0;
            bool loaded = GetFileSizeEx(handle, &length) && length.QuadPart > 0 && length.QuadPart < (64 << 20) &&
                          SUCCEEDED(D3DCreateBlob((SIZE_T)length.QuadPart, &code)) &&
                          ReadFile(handle, code->GetBufferPointer(), (DWORD)length.QuadPart, &read, nullptr) &&
                          read == (DWORD)length.QuadPart && read >= 4 && !std::memcmp(code->GetBufferPointer(), "DXBC", 4);
            CloseHandle(handle);
            if (loaded) { gpu_timing.shader_disk_hits++; return S_OK; }
            code.Reset();
        }
    }
    if (!compile) return E_PENDING;
    HRESULT result = D3DCompile(source, size, name, defines, nullptr, entry, profile, flags, 0, &code, &errors);
    if (SUCCEEDED(result) && !path.empty()) {
        /* Written under a unique name and renamed into place, so a reader
         * (or a second compile of the same shader) never sees half a file. */
        wchar_t suffix[32];
        swprintf(suffix, 32, L".%lu.%lu.tmp", GetCurrentProcessId(), GetCurrentThreadId());
        std::wstring temporary = path + suffix;
        HANDLE handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            bool complete = WriteFile(handle, code->GetBufferPointer(), (DWORD)code->GetBufferSize(), &written, nullptr) &&
                            written == code->GetBufferSize();
            CloseHandle(handle);
            if (!complete || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                DeleteFileW(temporary.c_str());
        }
    }
    return result;
}

extern "C" int nv2a_gpu_execute_state(const uint32_t program[136][4], const uint32_t valid[136], uint32_t start,
                                      const float attributes[16][4], float constants[192][4])
{
    GpuTimer state_timer(gpu_timing.state);
    struct StateVariant {
        std::vector<uint32_t> key;
        ComPtr<ID3D11ComputeShader> shader;
    };
    static std::vector<StateVariant> variants;
    if (!program || !valid || !attributes || !constants || !nv2a_gpu_available()) return 0;
    Nv2aGpuDraw state = {};
    state.vertex_program = program; state.vertex_valid = valid; state.vertex_start = start; state.vertex_constants = constants;
    StateVariant variant;
    if (!vertex_program_key(state, variant.key)) return 0;
    ComPtr<ID3D11ComputeShader> shader;
    for (auto &cached : variants) if (cached.key == variant.key) { shader = cached.shader; break; }
    if (!shader) {
        std::string source = vertex_program_source(variant.key, true);
        if (source.empty()) return 0;
        ComPtr<ID3DBlob> code, errors;
        HRESULT result = compile_shader_cached(source.data(), source.size(), "NV2A-state", nullptr, "cs_guest", "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, code, errors);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] state compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
            return 0;
        }
        if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader))) return 0;
        shader = variant.shader;
        if (variants.size() >= 128) variants.erase(variants.begin());
        variants.push_back(std::move(variant));
    }
    ComPtr<ID3D11Buffer> storage, staging, attribute_buffer;
    ComPtr<ID3D11UnorderedAccessView> writable;
    D3D11_BUFFER_DESC description = {};
    description.ByteWidth = 192 * 4 * sizeof(float); description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS; description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = 4 * sizeof(float);
    D3D11_SUBRESOURCE_DATA data = {constants, 0, 0};
    if (FAILED(device->CreateBuffer(&description, &data, &storage))) return 0;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; view.Buffer.NumElements = 192;
    if (FAILED(device->CreateUnorderedAccessView(storage.Get(), &view, &writable))) return 0;
    description.BindFlags = 0; description.MiscFlags = 0; description.StructureByteStride = 0;
    description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&description, nullptr, &staging))) return 0;
    description.ByteWidth = 16 * 4 * sizeof(float); description.Usage = D3D11_USAGE_IMMUTABLE;
    description.CPUAccessFlags = 0; description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    data.pSysMem = attributes;
    if (FAILED(device->CreateBuffer(&description, &data, &attribute_buffer))) return 0;
    ID3D11UnorderedAccessView *views[] = {writable.Get()}, *empty_views[] = {nullptr};
    ID3D11Buffer *buffers[] = {attribute_buffer.Get()}, *empty_buffers[] = {nullptr};
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(1, 1, buffers); context->CSSetUnorderedAccessViews(0, 1, views, nullptr);
    context->Dispatch(1, 1, 1);
    context->CSSetUnorderedAccessViews(0, 1, empty_views, nullptr);
    context->CSSetConstantBuffers(1, 1, empty_buffers); context->CSSetShader(nullptr, nullptr, 0);
    context->CopyResource(staging.Get(), storage.Get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 0;
    std::memcpy(constants, mapped.pData, 192 * 4 * sizeof(float)); context->Unmap(staging.Get(), 0);
    return 1;
}

/* A pixel shader variant neither in memory nor on disk compiles on worker
 * threads; meanwhile its draws use the generic shader -- the same source
 * without NV_COMPILED, reading the combiner setup from the constant buffer
 * every draw already fills. Slower per pixel, so it only bridges the few
 * frames a compile takes, but it never stalls the frame. RECOMP_SHADER_ASYNC=0
 * compiles in the draw instead, as before. */
struct PixelCompileJob {
    std::array<uint32_t,43> key;
    std::array<std::string,8> values;
    ComPtr<ID3DBlob> code, errors;
    ComPtr<ID3D11PixelShader> shader;
    bool generic = false;
    /* A vertex program instead: its words, HLSL and the objects made from it. */
    bool vertex = false;
    std::vector<uint32_t> vertex_key;
    std::string vertex_source;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11InputLayout> vertex_layout;
    uint32_t vertex_mask = 0;
};
static std::mutex pixel_jobs_lock;
static std::condition_variable pixel_jobs_signal;
static std::deque<std::shared_ptr<PixelCompileJob>> pixel_jobs_queued;
static std::vector<std::shared_ptr<PixelCompileJob>> pixel_jobs_done;
static std::vector<std::array<uint32_t,43>> pixel_jobs_pending;
static std::vector<std::vector<uint32_t>> vertex_jobs_pending;
static std::atomic<int> pixel_jobs_finished{0};
static ComPtr<ID3D11PixelShader> generic_pixel_shader;
static bool generic_pixel_requested;

static bool pixel_compiles_async()
{
    static const bool enabled = [] {
        const char *setting = std::getenv("RECOMP_SHADER_ASYNC");
        return !(setting && *setting == '0');
    }();
    return enabled;
}

static HRESULT compile_pixel_variant(const PixelCompileJob &job, ComPtr<ID3DBlob> &code, ComPtr<ID3DBlob> &errors,
                                     bool compile = true)
{
    if (job.generic)
        return compile_shader_cached(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A-generic", nullptr,
                                     "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                     code, errors, compile);
    const auto &values = job.values;
    const D3D_SHADER_MACRO definitions[] = {
        {"NV_COMPILED","1"}, {"NV_STAGE_COUNT",values[0].c_str()}, {"NV_COMBINER_CONTROL",values[1].c_str()},
        {"NV_STAGE_PROGRAM",values[2].c_str()}, {"NV_FINAL0",values[3].c_str()}, {"NV_FINAL1",values[4].c_str()},
        {"NV_STAGE_WORDS",values[5].c_str()}, {"NV_W_DEPTH",job.key[36] ? "1" : "0"},
        {"NV_TEXTURE_SIGNS",values[6].c_str()},
        {"NV_WINDOW_CLIP_MODE",values[7].c_str()},
        {"NV_DEPTH_ENABLED",job.key[37] ? "1" : "0"},
        {"NV_DEPTH_SEMANTIC",job.key[36] ? "SV_DepthGreaterEqual" : "SV_DEPTH"}, {nullptr,nullptr}
    };
    return compile_shader_cached(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A-guest", definitions,
                                 "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS,
                                 code, errors, compile);
}

static HRESULT compile_vertex_variant(const PixelCompileJob &job, ComPtr<ID3DBlob> &code, ComPtr<ID3DBlob> &errors,
                                      bool compile = true)
{
    return compile_shader_cached(job.vertex_source.data(), job.vertex_source.size(), "NV2A-vertex", nullptr, "vs_guest",
                                 "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, code, errors, compile);
}

/* The layout reads the attributes in `mask` packed in order (nv2a_gpu_draw
 * uploads only those); the rest -- inputs the program never reads -- alias
 * the first, as elements may overlap. */
static bool create_vertex_objects(ID3DBlob *code, ComPtr<ID3D11VertexShader> &shader, ComPtr<ID3D11InputLayout> &layout,
                                  uint32_t mask)
{
    D3D11_INPUT_ELEMENT_DESC elements[16] = {};
    uint32_t packed = 0;
    for (uint32_t attribute = 0; attribute < 16; attribute++) {
        elements[attribute].SemanticName = "TEXCOORD"; elements[attribute].SemanticIndex = attribute;
        elements[attribute].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elements[attribute].AlignedByteOffset = (mask & (1u << attribute)) ? 16 * packed++ : 0;
        elements[attribute].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    }
    if (FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)) ||
        FAILED(device->CreateInputLayout(elements, 16, code->GetBufferPointer(), code->GetBufferSize(), &layout))) {
        shader.Reset(); layout.Reset();
        return false;
    }
    return true;
}

static void pixel_compile_worker()
{
    for (;;) {
        std::shared_ptr<PixelCompileJob> job;
        {
            std::unique_lock<std::mutex> hold(pixel_jobs_lock);
            pixel_jobs_signal.wait(hold, [] { return !pixel_jobs_queued.empty(); });
            job = pixel_jobs_queued.front();
            pixel_jobs_queued.pop_front();
        }
        /* The device is free-threaded: the driver's own compile of the
         * bytecode (long for the generic shader) happens here too. */
        if (job->vertex) {
            if (SUCCEEDED(compile_vertex_variant(*job, job->code, job->errors)))
                create_vertex_objects(job->code.Get(), job->vertex_shader, job->vertex_layout, job->vertex_mask);
        } else if (SUCCEEDED(compile_pixel_variant(*job, job->code, job->errors)) &&
            FAILED(device->CreatePixelShader(job->code->GetBufferPointer(), job->code->GetBufferSize(), nullptr, &job->shader)))
            job->shader.Reset();
        std::lock_guard<std::mutex> hold(pixel_jobs_lock);
        pixel_jobs_done.push_back(job);
        pixel_jobs_finished++;
    }
}

static void queue_pixel_compile(std::shared_ptr<PixelCompileJob> job)
{
    static bool started;
    if (!started) {
        started = true;
        unsigned threads = std::max(1u, std::min(3u, std::thread::hardware_concurrency() / 4));
        for (unsigned index = 0; index < threads; index++) std::thread(pixel_compile_worker).detach();
    }
    std::lock_guard<std::mutex> hold(pixel_jobs_lock);
    if (job->vertex) vertex_jobs_pending.push_back(job->vertex_key);
    else if (!job->generic) pixel_jobs_pending.push_back(job->key);
    if (job->generic) pixel_jobs_queued.push_front(job); else pixel_jobs_queued.push_back(job);
    pixel_jobs_signal.notify_one();
}

/* Variants the workers finished since the last draw, into the cache. */
static void install_compiled_pixel_shaders()
{
    if (!pixel_jobs_finished.load(std::memory_order_relaxed)) return;
    std::vector<std::shared_ptr<PixelCompileJob>> done;
    {
        std::lock_guard<std::mutex> hold(pixel_jobs_lock);
        done.swap(pixel_jobs_done);
        pixel_jobs_finished = 0;
        for (const auto &job : done) {
            if (job->vertex)
                vertex_jobs_pending.erase(std::remove(vertex_jobs_pending.begin(), vertex_jobs_pending.end(), job->vertex_key),
                                          vertex_jobs_pending.end());
            else
                pixel_jobs_pending.erase(std::remove(pixel_jobs_pending.begin(), pixel_jobs_pending.end(), job->key),
                                         pixel_jobs_pending.end());
        }
    }
    for (const auto &job : done) {
        if (job->vertex) {
            if (!job->vertex_shader) {
                gpu_timing.vertex_shader_failures++;
                std::fprintf(stderr, "[GPU-D3D11] vertex compile: %s\n",
                             job->errors ? (const char *)job->errors->GetBufferPointer() : "failed");
                continue;
            }
            VertexVariant variant;
            variant.key = job->vertex_key;
            variant.shader = job->vertex_shader;
            variant.layout = job->vertex_layout;
            variant.input_mask = job->vertex_mask;
            variant.bytecode_bytes = job->code->GetBufferSize();
            gpu_timing.vertex_async_compiles++;
            cache_shader_variant(vertex_variants, std::move(variant));
            continue;
        }
        if (!job->shader) {
            gpu_timing.pixel_shader_failures++;
            std::fprintf(stderr, "[GPU-D3D11] %s shader compile: %s\n", job->generic ? "generic" : "guest",
                         job->errors ? (const char *)job->errors->GetBufferPointer() : "failed");
            continue;
        }
        if (job->generic) {
            generic_pixel_shader = job->shader;
            continue;
        }
        ShaderVariant variant = {};
        variant.key = job->key;
        variant.shader = job->shader;
        variant.bytecode_bytes = job->code->GetBufferSize();
        gpu_timing.pixel_async_compiles++;
        cache_shader_variant(shader_variants, std::move(variant));
    }
}

/* A vertex program not in memory or on disk compiles on the workers too.
 * There is no generic vertex shader to stand in, so its draws are skipped
 * (*pending) until it is ready: an object missing for a few frames the
 * first time this machine ever sees it, instead of a frame frozen for the
 * compile. */
static VertexVariant *get_vertex_shader(const Nv2aGpuDraw &state, bool *pending)
{
    *pending = false;
    uint32_t words = vertex_program_word_count(state);
    if (!words) return nullptr;
    /* Finished compiles are installed by get_pixel_shader, which runs first:
     * installing here could evict the pixel shader this draw just took. */
    for (auto &cached : vertex_variants) if (cached.key.size() == words &&
        !std::memcmp(cached.key.data(), state.vertex_program + state.vertex_start, words * sizeof(uint32_t))) {
        cached.last_used = ++shader_use_serial;
        gpu_timing.vertex_shader_hits++;
        return &cached;
    }
    std::vector<uint32_t> key(state.vertex_program[state.vertex_start], state.vertex_program[state.vertex_start] + words);
    bool async = pixel_compiles_async();
    if (async) {
        std::lock_guard<std::mutex> hold(pixel_jobs_lock);
        if (std::find(vertex_jobs_pending.begin(), vertex_jobs_pending.end(), key) != vertex_jobs_pending.end()) {
            gpu_timing.vertex_skipped_draws++;
            *pending = true;
            return nullptr;
        }
    }
    gpu_timing.vertex_shader_misses++;
    GpuTimer compile_timer(gpu_timing.vertex_compile);
    auto job = std::make_shared<PixelCompileJob>();
    job->vertex = true;
    job->vertex_source = vertex_program_source(key, false);
    if (job->vertex_source.empty()) return nullptr;
    job->vertex_key = std::move(key);
    job->vertex_mask = state.attribute_mask;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(compile_vertex_variant(*job, code, errors, !async))) {
        if (async && !errors) {
            queue_pixel_compile(job);
            gpu_timing.vertex_skipped_draws++;
            *pending = true;
            return nullptr;
        }
        gpu_timing.vertex_shader_failures++;
        std::fprintf(stderr, "[GPU-D3D11] vertex compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return nullptr;
    }
    VertexVariant variant;
    variant.key = std::move(job->vertex_key);
    if (!create_vertex_objects(code.Get(), variant.shader, variant.layout, state.attribute_mask)) return nullptr;
    variant.input_mask = state.attribute_mask;
    variant.bytecode_bytes = code->GetBufferSize();
    return cache_shader_variant(vertex_variants, std::move(variant));
}

static ID3D11PixelShader *get_pixel_shader(const Nv2aGpuDraw &state, uint32_t window_clip_mode)
{
    ShaderVariant variant = {};
    variant.key[0] = state.combiners.control; variant.key[1] = state.stage_program;
    variant.key[2] = state.combiners.final_input[0]; variant.key[3] = state.combiners.final_input[1];
    variant.key[36] = (state.control0 & 0x10000) != 0;
    variant.key[37] = state.depth_enable != 0;
    variant.key[42] = window_clip_mode;
    for (uint32_t stage = 0; stage < 4; stage++)
        if (nv2a_gpu_texture_mode_samples((state.stage_program >> (stage * 5)) & 31u))
            variant.key[38 + stage] = state.textures[stage].filter >> 28;
    for (uint32_t stage = 0; stage < (state.combiners.control & 255u); stage++) {
        variant.key[4+stage*4] = state.combiners.rgb_input[stage]; variant.key[5+stage*4] = state.combiners.alpha_input[stage];
        variant.key[6+stage*4] = state.combiners.rgb_output[stage]; variant.key[7+stage*4] = state.combiners.alpha_output[stage];
    }
    install_compiled_pixel_shaders();
    /* RECOMP_SHADER_GENERIC=1: every draw with the generic shader, to check it
     * against the compiled variants. */
    static const bool generic_only = std::getenv("RECOMP_SHADER_GENERIC") && *std::getenv("RECOMP_SHADER_GENERIC") == '1';
    if (generic_only && generic_pixel_shader) return generic_pixel_shader.Get();
    for (auto &cached : shader_variants) if (cached.key == variant.key) {
        cached.last_used = ++shader_use_serial;
        gpu_timing.pixel_shader_hits++;
        return cached.shader.Get();
    }
    bool async = pixel_compiles_async();
    if (async && generic_pixel_shader) {
        std::lock_guard<std::mutex> hold(pixel_jobs_lock);
        if (std::find(pixel_jobs_pending.begin(), pixel_jobs_pending.end(), variant.key) != pixel_jobs_pending.end()) {
            gpu_timing.pixel_generic_draws++;
            return generic_pixel_shader.Get();
        }
    }
    gpu_timing.pixel_shader_misses++;
    GpuTimer compile_timer(gpu_timing.pixel_compile);
    auto job = std::make_shared<PixelCompileJob>();
    job->key = variant.key;
    std::array<std::string,8> &values = job->values;
    values[0] = std::to_string(variant.key[0] & 255u) + "u";
    values[1] = std::to_string(variant.key[0]) + "u"; values[2] = std::to_string(variant.key[1]) + "u";
    values[3] = std::to_string(variant.key[2]) + "u"; values[4] = std::to_string(variant.key[3]) + "u";
    values[5] = "{";
    for (uint32_t stage = 0; stage < 8; stage++) {
        if (stage) values[5] += ",";
        values[5] += "uint4(";
        for (uint32_t component = 0; component < 4; component++) {
            if (component) values[5] += ",";
            values[5] += std::to_string(variant.key[4+stage*4+component]) + "u";
        }
        values[5] += ")";
    }
    values[5] += "}";
    values[6] = "uint4(";
    for (uint32_t stage = 0; stage < 4; stage++) {
        if (stage) values[6] += ",";
        values[6] += std::to_string(variant.key[38 + stage]) + "u";
    }
    values[6] += ")";
    values[7] = std::to_string(variant.key[42]) + "u";
    if (async && !generic_pixel_requested) {
        /* The generic shader itself, first in the workers' queue (from disk
         * after the first run); until it exists, misses compile in the draw. */
        generic_pixel_requested = true;
        auto generic = std::make_shared<PixelCompileJob>();
        generic->generic = true;
        queue_pixel_compile(generic);
    }
    if (async && generic_pixel_shader) {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(compile_pixel_variant(*job, code, errors, false))) {
            queue_pixel_compile(job);
            gpu_timing.pixel_generic_draws++;
            return generic_pixel_shader.Get();
        }
        job->code = code;
    }
    ComPtr<ID3DBlob> code = job->code, errors;
    if (!code && FAILED(compile_pixel_variant(*job, code, errors))) {
        gpu_timing.pixel_shader_failures++;
        std::fprintf(stderr, "[GPU-D3D11] guest shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return nullptr;
    }
    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &variant.shader))) return nullptr;
    variant.bytecode_bytes = code->GetBufferSize();
    auto *cached = cache_shader_variant(shader_variants, std::move(variant));
    return cached ? cached->shader.Get() : nullptr;
}

template<typename SurfaceType>
static void mark_written_mask(SurfaceType &surface, const D3D11_BOX &region)
{
    gpu_timing.coverage_mask_updates++;
    for (uint32_t row = region.top; row < region.bottom; row++) {
        size_t first = (size_t)row * surface.width + region.left;
        size_t end = (size_t)row * surface.width + region.right;
        size_t first_word = first / 64, last_word = (end - 1) / 64;
        uint64_t leading = ~uint64_t(0) << (first % 64);
        uint64_t trailing = end % 64 ? (uint64_t(1) << (end % 64)) - 1 : ~uint64_t(0);
        if (first_word == last_word) surface.written_mask[first_word] |= leading & trailing;
        else {
            surface.written_mask[first_word] |= leading;
            std::fill(surface.written_mask.begin() + first_word + 1, surface.written_mask.begin() + last_word, ~uint64_t(0));
            surface.written_mask[last_word] |= trailing;
        }
    }
}

template<typename SurfaceType>
static void mark_written_region(SurfaceType &surface, const D3D11_BOX &region)
{
    surface.clear_value_valid = false;
    if (!surface.dirty) {
        surface.written_region = region;
        surface.written_mask.clear();
    }
    else {
        auto &written = surface.written_region;
        bool contains = region.left <= written.left && region.top <= written.top &&
                        region.right >= written.right && region.bottom >= written.bottom;
        bool contained = written.left <= region.left && written.top <= region.top &&
                         written.right >= region.right && written.bottom >= region.bottom;
        bool rectangle = (written.left == region.left && written.right == region.right &&
                          region.top <= written.bottom && written.top <= region.bottom) ||
                         (written.top == region.top && written.bottom == region.bottom &&
                          region.left <= written.right && written.left <= region.right);
        if (contains) surface.written_mask.clear();
        else if (!surface.written_mask.empty()) mark_written_mask(surface, region);
        else if (!contained && !rectangle) {
            surface.written_mask.assign(((size_t)surface.width * surface.height + 63) / 64, 0);
            mark_written_mask(surface, written);
            mark_written_mask(surface, region);
        }
        if (region.left < written.left) written.left = region.left;
        if (region.top < written.top) written.top = region.top;
        if (region.right > written.right) written.right = region.right;
        if (region.bottom > written.bottom) written.bottom = region.bottom;
    }
    surface.dirty = true;
    surface.content_serial = ++target_content_serial;
}

template<typename SurfaceType, typename Publish>
static void for_each_written_span(const SurfaceType &surface, uint32_t row, const Publish &publish)
{
    const auto &region = surface.written_region;
    if (row < region.top || row >= region.bottom) return;
    if (surface.written_mask.empty()) { publish(region.left, region.right); return; }
    gpu_timing.coverage_mask_rows++;
    uint32_t column = region.left;
    auto covered = [&](uint32_t offset) {
        size_t bit = (size_t)row * surface.width + offset;
        return (surface.written_mask[bit / 64] >> (bit % 64)) & 1;
    };
    while (column < region.right) {
        while (column < region.right && !covered(column)) column++;
        uint32_t first = column;
        while (column < region.right && covered(column)) column++;
        if (first != column) publish(first, column);
    }
}

template<bool ToGuest>
static void rotate_depth_row(const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    uint32_t column = 0;
#if defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    for (; columns - column >= 4; column += 4) {
        __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + (size_t)column * 4));
        if constexpr (ToGuest) value = _mm_or_si128(_mm_slli_epi32(value, 8), _mm_srli_epi32(value, 24));
        else value = _mm_or_si128(_mm_srli_epi32(value, 8), _mm_slli_epi32(value, 24));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(destination + (size_t)column * 4), value);
    }
#endif
    for (; column < columns; column++) {
        uint32_t value;
        std::memcpy(&value, source + (size_t)column * 4, 4);
        if constexpr (ToGuest) value = (value << 8) | (value >> 24);
        else value = (value >> 8) | (value << 24);
        std::memcpy(destination + (size_t)column * 4, &value, 4);
    }
}

static void decode_depth_row(uint32_t format, const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    if (format == 1) {
        if (source != destination) std::memcpy(destination, source, (size_t)columns * 2);
    } else rotate_depth_row<true>(source, destination, columns);
}

static void encode_depth_row(uint32_t format, const uint8_t *source, uint8_t *destination, uint32_t columns)
{
    if (format == 1) {
        if (source != destination) std::memcpy(destination, source, (size_t)columns * 2);
    } else rotate_depth_row<false>(source, destination, columns);
}

template<typename SurfaceType>
static bool prepare_surface_snapshot(SurfaceType &surface, uint32_t bytes)
{
    if (!surface.source_snapshot.empty()) return true;
    const size_t budget = 64 * 1024 * 1024;
    size_t required = (size_t)surface.width * surface.height * bytes;
    size_t allocated = 0;
    for (const auto &entry : surfaces) allocated += entry.source_snapshot.size();
    for (const auto &entry : depth_surfaces) allocated += entry.source_snapshot.size();
    if (required > budget - allocated) return false;
    surface.source_snapshot.resize(required);
    return true;
}

template<typename SurfaceType>
static bool surface_memory_matches(const SurfaceType &surface, uint32_t bytes)
{
    if (!surface.snapshot_valid || surface.dirty) return false;
    size_t row_bytes = (size_t)surface.width * bytes;
    for (uint32_t row = 0; row < surface.height; row++)
        if (std::memcmp(surface.memory + (size_t)row * surface.pitch,
                        surface.source_snapshot.data() + (size_t)row * row_bytes, row_bytes)) return false;
    return true;
}

/* Draw `source` onto a target of another size, nearest texel: colour onto a
 * render target, or depth (the source's red channel, as SV_Depth) onto a depth
 * view. Only `box` (target pixels) is written when one is given. This is the
 * bridge between a scaled target and guest RAM's size, both ways. */
static bool scale_blit(ID3D11ShaderResourceView *source, ID3D11RenderTargetView *color,
                       ID3D11DepthStencilView *depth, UINT width, UINT height, const D3D11_BOX *box = nullptr)
{
    struct BlitPipeline {
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> color, depth;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11DepthStencilState> depth_write, depth_off;
    };
    static BlitPipeline pipeline;
    static bool failed;
    if (failed || !source || (!color && !depth)) return false;
    if (!pipeline.vertex) {
        static const char text[] =
            "Texture2D<float4> image:register(t0);SamplerState nearest:register(s0);"
            "struct V{float4 position:SV_Position;float2 uv:TEXCOORD0;};"
            "V vs(uint id:SV_VertexID){V v;float2 t=float2((id<<1)&2,id&2);"
            "v.position=float4(t*float2(2,-2)+float2(-1,1),0,1);v.uv=t;return v;}"
            "float4 color_ps(V v):SV_Target{return image.SampleLevel(nearest,v.uv,0);}"
            "float depth_ps(V v):SV_Depth{return image.SampleLevel(nearest,v.uv,0).r;}";
        BlitPipeline created;
        ComPtr<ID3DBlob> vertex, color_pixel, depth_pixel, errors;
        HRESULT result = D3DCompile(text, sizeof text - 1, "NV2A scale blit", nullptr, nullptr, "vs", "vs_5_0",
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors);
        if (SUCCEEDED(result))
            result = D3DCompile(text, sizeof text - 1, "NV2A scale blit", nullptr, nullptr, "color_ps", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &color_pixel, &errors);
        if (SUCCEEDED(result))
            result = D3DCompile(text, sizeof text - 1, "NV2A scale blit", nullptr, nullptr, "depth_ps", "ps_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &depth_pixel, &errors);
        if (SUCCEEDED(result))
            result = device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &created.vertex);
        if (SUCCEEDED(result))
            result = device->CreatePixelShader(color_pixel->GetBufferPointer(), color_pixel->GetBufferSize(), nullptr, &created.color);
        if (SUCCEEDED(result))
            result = device->CreatePixelShader(depth_pixel->GetBufferPointer(), depth_pixel->GetBufferSize(), nullptr, &created.depth);
        D3D11_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &created.sampler);
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE; raster.ScissorEnable = TRUE;
        if (SUCCEEDED(result)) result = device->CreateRasterizerState(&raster, &created.raster);
        D3D11_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(result)) result = device->CreateBlendState(&blend, &created.blend);
        D3D11_DEPTH_STENCIL_DESC depth_state = {};
        if (SUCCEEDED(result)) result = device->CreateDepthStencilState(&depth_state, &created.depth_off);
        depth_state.DepthEnable = TRUE; depth_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_state.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (SUCCEEDED(result)) result = device->CreateDepthStencilState(&depth_state, &created.depth_write);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] scale blit unavailable: 0x%08X %s\n", (unsigned)result,
                         errors ? (const char *)errors->GetBufferPointer() : "");
            failed = true;
            return false;
        }
        pipeline = std::move(created);
    }
    D3D11_RECT clip = {0, 0, (LONG)width, (LONG)height};
    if (box) clip = {(LONG)box->left, (LONG)box->top, (LONG)box->right, (LONG)box->bottom};
    ID3D11ShaderResourceView *none[12] = {};
    context->PSSetShaderResources(0, 12, none);
    context->OMSetRenderTargets(color ? 1 : 0, color ? &color : nullptr, depth);
    context->OMSetBlendState(pipeline.blend.Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(depth ? pipeline.depth_write.Get() : pipeline.depth_off.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(pipeline.vertex.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(color ? pipeline.color.Get() : pipeline.depth.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState *sampler = pipeline.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->RSSetState(pipeline.raster.Get());
    D3D11_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &clip);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

/* MSAA twins (with get_depth below). */
static bool create_ms_color(Surface &surface);
static void resolve_color(Surface &surface);
static void resolve_depth(DepthSurface &surface);

/* A box of guest pixels as the scaled texture's pixels, rounded outward. */
template<typename Target>
static D3D11_BOX texture_box(const Target &target, const D3D11_BOX &box)
{
    D3D11_BOX scaled = box;
    scaled.left = (UINT)((uint64_t)box.left * target.texture_width / target.width);
    scaled.top = (UINT)((uint64_t)box.top * target.texture_height / target.height);
    scaled.right = (UINT)(((uint64_t)box.right * target.texture_width + target.width - 1) / target.width);
    scaled.bottom = (UINT)(((uint64_t)box.bottom * target.texture_height + target.height - 1) / target.height);
    return scaled;
}

/* Guest RAM's pixels (`box` of them, or all) into a colour target. */
static void upload_surface(Surface &surface, const D3D11_BOX *box, const void *data, UINT row_pitch)
{
    resolve_color(surface);      /* a partial upload keeps the rest */
    if (surface.ms_texture) surface.ms_stale = true;
    if (!is_scaled(surface)) {
        context->UpdateSubresource(surface.texture.Get(), 0, box, data, row_pitch, 0);
        return;
    }
    context->UpdateSubresource(surface.upload.Get(), 0, box, data, row_pitch, 0);
    D3D11_BOX all = {0, 0, 0, surface.width, surface.height, 1};
    D3D11_BOX target = texture_box(surface, box ? *box : all);
    scale_blit(surface.upload_view.Get(), surface.target.Get(), nullptr,
               surface.texture_width, surface.texture_height, &target);
}

/* What a guest-size staging copy reads: the target itself, or for a scaled
 * one its resolve, drawn down from the texture first. */
static ID3D11Texture2D *publish_source(Surface &surface)
{
    resolve_color(surface);
    if (!is_scaled(surface)) return surface.texture.Get();
    scale_blit(surface.view.Get(), surface.resolve_target.Get(), nullptr, surface.width, surface.height);
    return surface.resolve.Get();
}

static ID3D11Texture2D *publish_source(DepthSurface &surface)
{
    resolve_depth(surface);
    if (!is_scaled(surface)) return surface.texture.Get();
    scale_blit(surface.sample_view.Get(), nullptr, surface.resolve_view.Get(), surface.width, surface.height);
    return surface.resolve.Get();
}

static void refresh_surface(Surface &surface)
{
    if (!surface.needs_refresh) return;
    /* A dirty surface holds GPU content newer than guest RAM (it has been drawn
     * into but not published). Uploading RAM here would overwrite the rendered
     * image with the previous publish -- the corruption seen when publication
     * went lazy. Keep the GPU copy; the surface is published at the next real
     * fence, and the texture path samples the GPU view while it is dirty. */
    if (surface.dirty) { surface.needs_refresh = false; return; }
    if (surface.snapshot_valid && !surface.dirty) {
        size_t row_bytes = (size_t)surface.width * 4;
        D3D11_BOX changed = {surface.width, surface.height, 0, 0, 0, 1};
        for (uint32_t row = 0; row < surface.height; row++) {
            const uint8_t *source = surface.memory + (size_t)row * surface.pitch;
            const uint8_t *cached = surface.source_snapshot.data() + (size_t)row * row_bytes;
            if (!std::memcmp(source, cached, row_bytes)) continue;
            uint32_t first = 0, end = surface.width;
            while (first < end && !std::memcmp(source + first * 4, cached + first * 4, 4)) first++;
            while (end > first && !std::memcmp(source + (end - 1) * 4, cached + (end - 1) * 4, 4)) end--;
            if (first < changed.left) changed.left = first;
            if (end > changed.right) changed.right = end;
            if (row < changed.top) changed.top = row;
            changed.bottom = row + 1;
        }
        if (changed.left < changed.right) {
            surface.clear_value_valid = false;
            for (uint32_t row = changed.top; row < changed.bottom; row++)
                std::memcpy(surface.source_snapshot.data() + (size_t)row * row_bytes + changed.left * 4,
                            surface.memory + (size_t)row * surface.pitch + changed.left * 4,
                            (size_t)(changed.right - changed.left) * 4);
            upload_surface(surface, &changed,
                           surface.source_snapshot.data() + (size_t)changed.top * row_bytes + changed.left * 4,
                           (UINT)row_bytes);
            surface.alias_copy_serial = 0;
            gpu_timing.color_refreshed++;
            gpu_timing.color_refresh_bytes += (uint64_t)(changed.right - changed.left) * (changed.bottom - changed.top) * 4;
            gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
        } else gpu_timing.color_refresh_reused++;
        surface.needs_refresh = false;
        return;
    }
    upload_surface(surface, nullptr, surface.memory, surface.pitch);
    surface.clear_value_valid = false;
    surface.alias_copy_serial = 0;
    gpu_timing.color_refresh_bytes += (uint64_t)surface.width * surface.height * 4;
    gpu_timing.color_refresh_full_bytes += (uint64_t)surface.width * surface.height * 4;
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.color_refreshed++;
}

static bool target_operation_succeeded(const char *kind, const char *operation, HRESULT result)
{
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "[GPU-D3D11] %s target %s failed: 0x%08X; device 0x%08X\n",
                 kind, operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    return false;
}

static bool refresh_depth(DepthSurface &surface)
{
    if (!surface.needs_refresh) return true;
    if (surface_memory_matches(surface, surface.format == 1 ? 2 : 4)) {
        surface.needs_refresh = false;
        gpu_timing.depth_refresh_reused++;
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!target_operation_succeeded("depth", "upload map",
        context->Map(surface.staging.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;
    for (uint32_t row = 0; row < surface.height; row++) {
        const uint8_t *source = surface.memory + (size_t)row * surface.pitch;
        uint8_t *destination = (uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
        encode_depth_row(surface.format, source, destination, surface.width);
    }
    context->Unmap(surface.staging.Get(), 0);
    if (surface.ms_texture) { surface.ms_newer = false; surface.ms_stale = true; }
    if (is_scaled(surface)) {
        context->CopyResource(surface.upload.Get(), surface.staging.Get());
        scale_blit(surface.upload_view.Get(), nullptr, surface.view.Get(), surface.texture_width, surface.texture_height);
    } else {
        context->CopyResource(surface.texture.Get(), surface.staging.Get());
    }
    surface.clear_value_valid = false;
    surface.needs_refresh = false;
    surface.snapshot_valid = false;
    gpu_timing.depth_refreshed++;
    return true;
}

/* A colour target's texture at its texture size, and for a scaled one the
 * guest-size resolve and upload; `data` fills an unscaled texture. */
static bool create_color_textures(Surface &surface, const D3D11_SUBRESOURCE_DATA *data)
{
    surface.texture.Reset(); surface.target.Reset(); surface.view.Reset();
    surface.resolve.Reset(); surface.resolve_target.Reset(); surface.upload.Reset(); surface.upload_view.Reset();
    bool scaled = is_scaled(surface);
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = surface.texture_width; description.Height = surface.texture_height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!target_operation_succeeded("color", "create texture", device->CreateTexture2D(&description, scaled ? nullptr : data, &surface.texture)) ||
        !target_operation_succeeded("color", "create view", device->CreateRenderTargetView(surface.texture.Get(), nullptr, &surface.target)) ||
        !target_operation_succeeded("color", "create sample view", device->CreateShaderResourceView(surface.texture.Get(), nullptr, &surface.view))) return false;
    if (!scaled) return true;
    description.Width = surface.width; description.Height = surface.height;
    return target_operation_succeeded("color", "create resolve", device->CreateTexture2D(&description, nullptr, &surface.resolve)) &&
           target_operation_succeeded("color", "create resolve view", device->CreateRenderTargetView(surface.resolve.Get(), nullptr, &surface.resolve_target)) &&
           target_operation_succeeded("color", "create upload", device->CreateTexture2D(&description, nullptr, &surface.upload)) &&
           target_operation_succeeded("color", "create upload view", device->CreateShaderResourceView(surface.upload.Get(), nullptr, &surface.upload_view));
}

static Surface *get_surface(const Nv2aGpuDraw &state, bool preserve = true, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    for (auto &surface : surfaces)
        if (surface.memory == state.color && surface.width == extent.width && surface.height == extent.height && surface.pitch == state.pitch) {
            surface.last_used = ++surface_use_serial;
            if (preserve) refresh_surface(surface);
            else surface.needs_refresh = false;
            return &surface;
        }
    Surface surface = {};
    surface.memory = state.color; surface.width = extent.width; surface.height = extent.height; surface.pitch = state.pitch;
    texture_size(extent.width, extent.height, true, surface.texture_width, surface.texture_height);
    surface.last_used = ++surface_use_serial;
    bool scaled = is_scaled(surface);
    D3D11_SUBRESOURCE_DATA data = {state.color, state.pitch, 0};
    if (!create_color_textures(surface, preserve ? &data : nullptr) || !create_ms_color(surface)) return nullptr;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = extent.width; description.Height = extent.height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!target_operation_succeeded("color", "create staging", device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
    surfaces.push_back(std::move(surface));
    gpu_timing.color_created++;
    Surface &created = surfaces.back();
    if (preserve && scaled) {
        created.needs_refresh = true;
        refresh_surface(created);
    }
    return &created;
}

/* A depth target's texture, its depth and sample views, and for a scaled one
 * the guest-size resolve and upload. */
static bool create_depth_textures(DepthSurface &surface)
{
    surface.texture.Reset(); surface.view.Reset(); surface.sample_view.Reset();
    surface.resolve.Reset(); surface.resolve_view.Reset(); surface.upload.Reset(); surface.upload_view.Reset();
    bool wide = surface.format != 1;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = surface.texture_width; description.Height = surface.texture_height;
    description.MipLevels = description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = wide ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R16_TYPELESS;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = wide ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D16_UNORM; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sample = {};
    sample.Format = wide ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R16_UNORM;
    sample.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sample.Texture2D.MipLevels = 1;
    if (!target_operation_succeeded("depth", "create texture", device->CreateTexture2D(&description, nullptr, &surface.texture)) ||
        !target_operation_succeeded("depth", "create view", device->CreateDepthStencilView(surface.texture.Get(), &view, &surface.view)) ||
        !target_operation_succeeded("depth", "create sample view", device->CreateShaderResourceView(surface.texture.Get(), &sample, &surface.sample_view))) return false;
    if (!is_scaled(surface)) return true;
    description.Width = surface.width; description.Height = surface.height;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (!target_operation_succeeded("depth", "create resolve", device->CreateTexture2D(&description, nullptr, &surface.resolve)) ||
        !target_operation_succeeded("depth", "create resolve view", device->CreateDepthStencilView(surface.resolve.Get(), &view, &surface.resolve_view))) return false;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return target_operation_succeeded("depth", "create upload", device->CreateTexture2D(&description, nullptr, &surface.upload)) &&
           target_operation_succeeded("depth", "create upload view", device->CreateShaderResourceView(surface.upload.Get(), &sample, &surface.upload_view));
}

/* The multisampled twins, for a screen-shaped target drawn with colour when
 * MSAA is on; none otherwise. A new twin is stale: `texture` is drawn into it
 * before the first draw. */
static bool create_ms_color(Surface &surface)
{
    surface.ms_texture.Reset(); surface.ms_target.Reset();
    surface.ms_newer = surface.ms_stale = false;
    if (msaa_samples <= 1 || !screen_shaped(surface.width, surface.height)) return true;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = surface.texture_width; description.Height = surface.texture_height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = msaa_samples;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = description.Format; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    if (!target_operation_succeeded("color", "create MSAA texture", device->CreateTexture2D(&description, nullptr, &surface.ms_texture)) ||
        !target_operation_succeeded("color", "create MSAA view", device->CreateRenderTargetView(surface.ms_texture.Get(), &view, &surface.ms_target))) {
        surface.ms_texture.Reset(); surface.ms_target.Reset();
        return false;
    }
    surface.ms_stale = true;
    return true;
}

static bool create_ms_depth(DepthSurface &surface)
{
    surface.ms_texture.Reset(); surface.ms_view.Reset(); surface.ms_sample_view.Reset();
    surface.ms_newer = surface.ms_stale = false;
    if (msaa_samples <= 1 || !depth_scaled(surface.memory) || !screen_shaped(surface.width, surface.height)) return true;
    bool wide = surface.format != 1;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = surface.texture_width; description.Height = surface.texture_height;
    description.MipLevels = description.ArraySize = 1; description.SampleDesc.Count = msaa_samples;
    description.Format = wide ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R16_TYPELESS;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = wide ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D16_UNORM; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sample = {};
    sample.Format = wide ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R16_UNORM;
    sample.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    if (!target_operation_succeeded("depth", "create MSAA texture", device->CreateTexture2D(&description, nullptr, &surface.ms_texture)) ||
        !target_operation_succeeded("depth", "create MSAA view", device->CreateDepthStencilView(surface.ms_texture.Get(), &view, &surface.ms_view)) ||
        !target_operation_succeeded("depth", "create MSAA sample view", device->CreateShaderResourceView(surface.ms_texture.Get(), &sample, &surface.ms_sample_view))) {
        surface.ms_texture.Reset(); surface.ms_view.Reset(); surface.ms_sample_view.Reset();
        return false;
    }
    surface.ms_stale = true;
    return true;
}

/* `texture` up to date with what was drawn into the twin. */
static void resolve_color(Surface &surface)
{
    if (!surface.ms_texture || !surface.ms_newer) return;
    context->ResolveSubresource(surface.texture.Get(), 0, surface.ms_texture.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    surface.ms_newer = false;
}

/* Depth has no ResolveSubresource: the first sample of each pixel, drawn as
 * SV_Depth into the single-sampled view. Stencil is not carried. */
static void resolve_depth(DepthSurface &surface)
{
    static ComPtr<ID3D11VertexShader> vertex;
    static ComPtr<ID3D11PixelShader> pixel;
    static ComPtr<ID3D11DepthStencilState> write;
    static ComPtr<ID3D11RasterizerState> raster;
    static bool failed;
    if (!surface.ms_texture || !surface.ms_newer || failed) return;
    if (!pixel) {
        static const char text[] =
            "Texture2DMS<float> image:register(t0);"
            "float4 vs(uint id:SV_VertexID):SV_Position{float2 t=float2((id<<1)&2,id&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}"
            "float ps(float4 p:SV_Position):SV_Depth{return image.Load(int2(p.xy),0);}";
        ComPtr<ID3DBlob> vs, ps, errors;
        HRESULT result = D3DCompile(text, sizeof text - 1, "MSAA depth resolve", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors);
        if (SUCCEEDED(result)) result = D3DCompile(text, sizeof text - 1, "MSAA depth resolve", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors);
        if (SUCCEEDED(result)) result = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex);
        if (SUCCEEDED(result)) result = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel);
        D3D11_DEPTH_STENCIL_DESC depth = {};
        depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (SUCCEEDED(result)) result = device->CreateDepthStencilState(&depth, &write);
        D3D11_RASTERIZER_DESC rasterizer = {};
        rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE;
        if (SUCCEEDED(result)) result = device->CreateRasterizerState(&rasterizer, &raster);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] MSAA depth resolve unavailable: 0x%08X\n", (unsigned)result);
            failed = true; pixel.Reset();
            return;
        }
    }
    ID3D11ShaderResourceView *none[12] = {}, *source = surface.ms_sample_view.Get();
    context->PSSetShaderResources(0, 12, none);
    context->OMSetRenderTargets(0, nullptr, surface.view.Get());
    context->OMSetDepthStencilState(write.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport = {0, 0, (float)surface.texture_width, (float)surface.texture_height, 0, 1};
    D3D11_RECT clip = {0, 0, (LONG)surface.texture_width, (LONG)surface.texture_height};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &clip);
    context->Draw(3, 0);
    context->PSSetShaderResources(0, 1, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    surface.ms_newer = false;
}

/* Before drawing into the twins: what was written to `texture` since. */
static void refresh_ms_color(Surface &surface)
{
    if (!surface.ms_texture || !surface.ms_stale) return;
    scale_blit(surface.view.Get(), surface.ms_target.Get(), nullptr, surface.texture_width, surface.texture_height);
    surface.ms_stale = false;
}

static void refresh_ms_depth(DepthSurface &surface)
{
    if (!surface.ms_texture || !surface.ms_stale) return;
    context->ClearDepthStencilView(surface.ms_view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    scale_blit(surface.sample_view.Get(), nullptr, surface.ms_view.Get(), surface.texture_width, surface.texture_height);
    surface.ms_stale = false;
}

/* The views a draw writes: both twins when every target it has has one;
 * otherwise the single-sampled textures, brought up to date first. */
static void bind_draw_targets(Surface *surface, DepthSurface *depth, ID3D11RenderTargetView *&color, ID3D11DepthStencilView *&depth_view)
{
    bool multisampled = (surface && surface->ms_texture) || (depth && depth->ms_texture);
    if (multisampled && ((surface && !surface->ms_texture) || (depth && !depth->ms_texture))) multisampled = false;
    if (multisampled) {
        if (surface) { refresh_ms_color(*surface); surface->ms_newer = true; }
        if (depth) { refresh_ms_depth(*depth); depth->ms_newer = true; }
        color = surface ? surface->ms_target.Get() : nullptr;
        depth_view = depth ? depth->ms_view.Get() : nullptr;
        return;
    }
    if (surface) { resolve_color(*surface); if (surface->ms_texture) surface->ms_stale = true; }
    if (depth) { resolve_depth(*depth); if (depth->ms_texture) depth->ms_stale = true; }
    color = surface ? surface->target.Get() : nullptr;
    depth_view = depth ? depth->view.Get() : nullptr;
}

static DepthSurface *get_depth(const Nv2aGpuDraw &state, bool preserve = true, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    if (!state.depth || (state.depth_format != 1 && state.depth_format != 2)) {
        std::fprintf(stderr, "[GPU-D3D11] invalid depth target: memory %p format %u\n",
                     (void *)state.depth, state.depth_format);
        return nullptr;
    }
    uint32_t bytes = state.depth_format == 1 ? 2 : 4;
    if (state.depth_pitch < extent.width * bytes) {
        std::fprintf(stderr, "[GPU-D3D11] depth target pitch %u cannot hold %u columns of format %u\n",
                     state.depth_pitch, extent.width, state.depth_format);
        return nullptr;
    }
    UINT texture_width, texture_height;
    texture_size(extent.width, extent.height, depth_scaled(state.depth), texture_width, texture_height);
    for (auto &surface : depth_surfaces)
        if (surface.memory == state.depth && surface.width == extent.width && surface.height == extent.height &&
            surface.pitch == state.depth_pitch && surface.format == state.depth_format &&
            surface.texture_width == texture_width && surface.texture_height == texture_height) {
            surface.last_used = ++surface_use_serial;
            if (!preserve) { surface.needs_refresh = false; return &surface; }
            return refresh_depth(surface) ? &surface : nullptr;
        }
    DepthSurface surface = {};
    surface.memory = state.depth; surface.width = extent.width; surface.height = extent.height; surface.pitch = state.depth_pitch; surface.format = state.depth_format;
    surface.texture_width = texture_width; surface.texture_height = texture_height;
    surface.last_used = ++surface_use_serial;
    if (!create_depth_textures(surface) || !create_ms_depth(surface)) return nullptr;
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = extent.width; description.Height = extent.height;
    description.MipLevels = description.ArraySize = 1; description.SampleDesc.Count = 1;
    description.Format = bytes == 2 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    description.Usage = D3D11_USAGE_STAGING; description.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (!target_operation_succeeded("depth", "create staging", device->CreateTexture2D(&description, nullptr, &surface.staging))) return nullptr;
    // Creation-data uploads can round D24 values; use the bit-preserving refresh path.
    surface.needs_refresh = preserve;
    if (preserve && !refresh_depth(surface)) return nullptr;
    depth_surfaces.push_back(std::move(surface)); gpu_timing.depth_surface_created++;
    return &depth_surfaces.back();
}

/* Every target at the size the scale and widescreen now give it: each is
 * drawn into a texture of its new size, so what it holds -- a frame not yet
 * published, a texture the title renders once -- carries over. A target whose
 * new texture cannot be made is dropped and comes back from guest RAM. */
static void resize_targets(const char *why)
{
    unsigned colors = 0, depths = 0;
    for (size_t index = 0; index < surfaces.size();) {
        Surface &surface = surfaces[index];
        UINT width, height;
        texture_size(surface.width, surface.height, true, width, height);
        if (width == surface.texture_width && height == surface.texture_height) { index++; continue; }
        resolve_color(surface);
        ComPtr<ID3D11ShaderResourceView> old = surface.view;
        surface.texture_width = width; surface.texture_height = height;
        if (create_color_textures(surface, nullptr) && scale_blit(old.Get(), surface.target.Get(), nullptr, width, height) &&
                create_ms_color(surface)) {
            colors++; index++;
        } else {
            surfaces.erase(surfaces.begin() + (ptrdiff_t)index);
        }
    }
    for (size_t index = 0; index < depth_surfaces.size();) {
        DepthSurface &surface = depth_surfaces[index];
        bool scaled = depth_scaled(surface.memory);
        UINT width, height;
        texture_size(surface.width, surface.height, scaled, width, height);
        if (width == surface.texture_width && height == surface.texture_height) { index++; continue; }
        resolve_depth(surface);
        ComPtr<ID3D11ShaderResourceView> old = surface.sample_view;
        surface.texture_width = width; surface.texture_height = height;
        bool made = create_depth_textures(surface) && create_ms_depth(surface);
        /* Depth carries over; stencil starts clear. */
        if (made) context->ClearDepthStencilView(surface.view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        if (made && scale_blit(old.Get(), nullptr, surface.view.Get(), width, height)) {
            surface.clear_value_valid = false;
            depths++; index++;
        } else {
            depth_surfaces.erase(depth_surfaces.begin() + (ptrdiff_t)index);
        }
    }
    std::fprintf(stderr, "[GPU-D3D11] %s: %u colour and %u depth targets redrawn\n", why, colors, depths);
}

/* Between frames: a scale or widescreen asked for, and the 2D-frame test. */
static void apply_target_sizes()
{
    char why[96] = "";
    float requested = pending_render_scale.exchange(-1.0f);
    if (requested >= 0.0f) {
        float previous = render_scale(), scale = requested_render_scale(requested);
        if (scale != previous) {
            current_render_scale = scale;
            std::snprintf(why, sizeof why, "render scale %gx -> %gx", previous, scale);
        }
    }
    requested = pending_wide_factor.exchange(-1.0f);
    if (requested > 0.0f && requested != wide_factor) {
        std::snprintf(why + std::strlen(why), sizeof why - std::strlen(why), "%swidescreen %.3g -> %.3g",
                      *why ? ", " : "", wide_factor, requested);
        wide_factor = requested;
    }
    int samples = pending_msaa.exchange(-1);
    if (samples > 0 && (UINT)samples != msaa_samples) {
        UINT quality = 0;
        while (samples > 1 && (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &quality)) ||
                               !quality || FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R24G8_TYPELESS, samples, &quality)) ||
                               !quality)) samples /= 2;
        for (auto &surface : surfaces) resolve_color(surface);
        for (auto &surface : depth_surfaces) resolve_depth(surface);
        std::fprintf(stderr, "[GPU-D3D11] MSAA %ux -> %dx\n", msaa_samples, samples);
        msaa_samples = (UINT)samples;
        for (auto &surface : surfaces) create_ms_color(surface);
        for (auto &surface : depth_surfaces) create_ms_depth(surface);
    }
    frames_without_3d = screen_3d_draws ? 0 : frames_without_3d + 1;
    screen_3d_draws = 0;
    bool wide = frames_without_3d < 2;
    if (wide != wide_frames) {
        wide_frames = wide;
        if (wide_factor != 1.0f)
            std::snprintf(why + std::strlen(why), sizeof why - std::strlen(why), "%s%s",
                          *why ? ", " : "", wide ? "3D frames, wide" : "2D frames, 4:3");
    }
    if (*why) resize_targets(why);
}

/* Samples per pixel of the screen's targets: 1 (off), 2, 4 or 8, from the next
 * flip; fewer if the adapter cannot. Any thread. */
extern "C" void nv2a_gpu_set_msaa(int samples)
{
    pending_msaa.store(samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1);
}

extern "C" void nv2a_gpu_set_render_scale(float scale)
{
    pending_render_scale.store(scale > 0.0f ? scale : 0.0f);
}

/* The width of the screen's targets over 4:3's: 1 for 4:3, 4/3 for 16:9.
 * From the next flip; any thread. */
extern "C" void nv2a_gpu_set_wide_factor(float factor)
{
    pending_wide_factor.store(factor >= 1.0f && factor <= 4.0f ? factor : 1.0f);
}

static void readback_failed(const char *kind, HRESULT result, const char *operation = "map")
{
    std::fprintf(stderr, "[GPU-D3D11] readback failed: %s %s 0x%08X; device 0x%08X\n",
                 kind, operation, (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

static void mark_target_aliases(const void *owner, const uint8_t *memory, size_t bytes)
{
    uintptr_t begin = (uintptr_t)memory, end = begin + bytes;
    auto mark = [&](auto &surface) {
        uintptr_t surface_begin = (uintptr_t)surface.memory;
        uintptr_t surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if ((const void *)&surface != owner && begin < surface_end && end > surface_begin)
            surface.needs_refresh = true;
    };
    for (auto &surface : surfaces) mark(surface);
    for (auto &surface : depth_surfaces) mark(surface);
}

static void fill_clear_row(uint8_t *destination, uint32_t columns, uint32_t value, uint32_t bytes)
{
    if (bytes == 4) {
        for (uint32_t column = 0; column < columns; column++)
            std::memcpy(destination + (size_t)column * 4, &value, 4);
    } else {
        uint16_t short_value = (uint16_t)value;
        for (uint32_t column = 0; column < columns; column++)
            std::memcpy(destination + (size_t)column * 2, &short_value, 2);
    }
}

template<typename SurfaceType>
static void publish_known_clear(SurfaceType &surface, uint32_t bytes)
{
    bool snapshot = prepare_surface_snapshot(surface, bytes);
    size_t row_bytes = (size_t)surface.width * bytes;
    if (snapshot) fill_clear_row(surface.source_snapshot.data(), surface.width, surface.clear_value, bytes);
    for (uint32_t row = 0; row < surface.height; row++) {
        uint8_t *destination = surface.memory + (size_t)row * surface.pitch;
        if (snapshot) {
            uint8_t *cached = surface.source_snapshot.data() + (size_t)row * row_bytes;
            if (row) std::memcpy(cached, surface.source_snapshot.data(), row_bytes);
            std::memcpy(destination, cached, row_bytes);
        } else fill_clear_row(destination, surface.width, surface.clear_value, bytes);
    }
    surface.snapshot_valid = snapshot;
    gpu_timing.clear_readback_bytes_avoided += row_bytes * surface.height;
    mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
    surface.dirty = false;
}

/* Texture re-validation pacing.
 *
 * Deciding whether a bound texture changed means memcmp'ing the whole image
 * from guest RAM against its snapshot. The menu bound textures ~2.7x their
 * total size per frame, so the same unchanged image was compared several times
 * a frame; measured, that was 62 GiB and ~24% of runtime for ~1,000 real
 * uploads. Validating each texture at most once per frame keeps the first
 * comparison and reuses its verdict for the rest of the frame.
 *
 * The serial advances on every flip (nv2a_gpu_sync). Disable with
 * RECOMP_TEX_VALIDATE_ONCE=0 if a title writes a texture mid-frame and samples
 * it again -- on hardware that is a race anyway, but a title might rely on it.
 */
static uint64_t s_texture_validate_serial = 1;
static int s_tex_validate_once = -1;
static bool tex_validate_once(void)
{
    if (s_tex_validate_once < 0) {
        const char *value = std::getenv("RECOMP_TEX_VALIDATE_ONCE");
        s_tex_validate_once = (value && value[0] == '0') ? 0 : 1;
    }
    return s_tex_validate_once != 0;
}

/* Depth surfaces are only ever consumed by a guest CPU read of their memory;
 * nothing presents them, and targeted publishes never carry depth anyway. The
 * full-sync publication (CopyResource + Map + de-swizzle) is otherwise pure
 * wall-clock per flip. Default on (other titles may read depth back);
 * RECOMP_DEPTH_PUBLISH=0 skips it, which this game does. */
static bool publish_depth(void)
{
    static int on = -1;
    if (on < 0) {
        const char *value = std::getenv("RECOMP_DEPTH_PUBLISH");
        on = (value && value[0] == '0') ? 0 : 1;
    }
    return on != 0;
}

/* Ordering events (WAIT_FOR_IDLE, semaphore release) only need the GPU to have
 * caught up; they do not read surfaces back. Waiting for every one serialises
 * the guest against the GPU -- ~39 full round trips per frame -- but draining
 * nothing at all was measured a net loss (the guest races ahead and the flip
 * drains a deep backlog). Keep a small ring of event queries and only wait for
 * the one from RECOMP_SYNC_LAG events ago, so the GPU runs a bounded distance
 * ahead. RECOMP_SYNC_LAG=0 restores the per-event wait. The waits are pipeline
 * latency, not GPU throughput: paired ab-perf at 1080p gameplay measured lag 4
 * 31.6/32.4, lag 7 35.1/36.1, lag 16 36.5/37.9 FPS with event-wait time
 * falling ~9 s -> ~3 s -> ~0.4 s per run, so 16 is the default (ring 32). */
static void wait_ordering_completion(void)
{
    static ComPtr<ID3D11Query> ring[32];
    static unsigned produced;
    static int lag = -1;
    if (lag < 0) {
        const char *value = std::getenv("RECOMP_SYNC_LAG");
        lag = value ? std::atoi(value) : 16;
        if (lag < 0) lag = 0;
        if (lag > 31) lag = 31;
    }
    unsigned slot = produced++ & 31u;
    if (!ring[slot]) {
        D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
        HRESULT created = device->CreateQuery(&description, &ring[slot]);
        if (FAILED(created)) readback_failed("completion event", created, "create");
    }
    context->End(ring[slot].Get());
    context->Flush();
    if (produced > (unsigned)lag) {
        ID3D11Query *old = ring[(produced - 1 - (unsigned)lag) & 31u].Get();
        while (context->GetData(old, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) {
            xbox_Nv2aAcknowledgeHandshakes();
            SwitchToThread();
        }
    }
}

static void gpu_sync_impl(bool publish, const void *only = nullptr, size_t only_bytes = 0)
{
    GpuTimer sync_timer(gpu_timing.sync);
    if (!context) return;
    const uintptr_t only_begin = (uintptr_t)only;
    const uintptr_t only_end = only_begin + only_bytes;
    auto wanted = [&](const Surface &surface) {
        if (!only) return true;
        uintptr_t begin = (uintptr_t)surface.memory;
        uintptr_t end = begin + (size_t)surface.pitch * surface.height;
        return only_begin < end && only_end > begin;
    };
    gpu_timing.sync_calls++;
    if (!publish) {
        /* Completion only: wait for the GPU to finish, but do not copy any
         * surface back to guest RAM. WAIT_FOR_IDLE and the semaphore release
         * only need ordering -- the guest is not about to read the surface --
         * and they fire ~50x a frame, so a full readback on each was most of
         * the sync cost. Nothing is marked clean, so the next real publish
         * still writes the final contents. The texture serial is deliberately
         * NOT advanced here: this is not a frame boundary. */
        if (pending_draws) {
            GpuTimer wait_timer(gpu_timing.sync_event_wait);
            gpu_timing.completion_waits++;
            wait_ordering_completion();
            pending_draws = false;
        }
        return;
    }
    ID3D11RenderTargetView *empty = nullptr;
    context->OMSetRenderTargets(1, &empty, nullptr);
    bool has_readback = false;
    bool has_known_clear = false;
    {
        GpuTimer copy_timer(gpu_timing.sync_copy);
        for (auto &surface : surfaces)
            if (surface.dirty) {
                if (!wanted(surface)) continue;
                if (surface.clear_value_valid) {
                    has_known_clear = true;
                    continue;
                }
                ID3D11Texture2D *source = publish_source(surface);
                if (surface.snapshot_valid) {
                    const auto &region = surface.written_region;
                    context->CopySubresourceRegion(surface.staging.Get(), 0, region.left, region.top, 0,
                                                  source, 0, &region);
                    gpu_timing.color_copy_bytes += (uint64_t)(region.right - region.left) * (region.bottom - region.top) * 4;
                } else {
                    context->CopyResource(surface.staging.Get(), source);
                    gpu_timing.color_copy_bytes += (uint64_t)surface.width * surface.height * 4;
                }
                has_readback = true;
            }
        if (!only && publish_depth())
        for (auto &surface : depth_surfaces)
            if (surface.dirty) {
                if (surface.clear_value_valid) {
                    has_known_clear = true;
                    continue;
                }
                context->CopyResource(surface.staging.Get(), publish_source(surface));
                has_readback = true;
            }
    }
    /* Targeted publish with nothing overlapping dirty: nothing to pull back,
     * so don't pay a GPU-completion wait per blit. */
    if (only && !has_readback && !has_known_clear) return;
    if (pending_draws && (!has_readback || has_known_clear)) {
        GpuTimer wait_timer(gpu_timing.sync_event_wait);
        gpu_timing.completion_waits++;
        if (!completion_event) {
            D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
            HRESULT result = device->CreateQuery(&description, &completion_event);
            if (FAILED(result)) readback_failed("completion event", result, "create");
        }
        context->End(completion_event.Get());
        context->Flush();
        HRESULT result;
        while ((result = context->GetData(completion_event.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            xbox_Nv2aAcknowledgeHandshakes();
            SwitchToThread();
        }
        if (FAILED(result)) readback_failed("completion event", result, "wait");
    }
    for (auto &surface : surfaces) {
        if (!surface.dirty) continue;
        if (!wanted(surface)) continue;
        if (surface.clear_value_valid) {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer color_timer(gpu_timing.color_publish);
            publish_known_clear(surface, 4);
            gpu_timing.color_publish_bytes += (uint64_t)surface.width * surface.height * 4;
            gpu_timing.color_clear_publications++;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result;
        {
            GpuTimer wait_timer(gpu_timing.sync_wait);
            result = context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (FAILED(result)) readback_failed("color", result);
        gpu_timing.color_readbacks++;
        {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer color_timer(gpu_timing.color_publish);
            bool snapshot = prepare_surface_snapshot(surface, 4);
            bool initialize_snapshot = snapshot && !surface.snapshot_valid;
            const auto &region = surface.written_region;
            size_t row_bytes = (size_t)surface.width * 4;
            uint32_t first_row = initialize_snapshot ? 0 : region.top;
            uint32_t last_row = initialize_snapshot ? surface.height : region.bottom;
            if (surface.written_mask.empty() && region.left == 0 && region.right == surface.width &&
                mapped.RowPitch == row_bytes && surface.pitch == row_bytes) {
                const uint8_t *source = (const uint8_t *)mapped.pData;
                size_t offset = (size_t)region.top * row_bytes;
                size_t bytes = (size_t)(region.bottom - region.top) * row_bytes;
                if (snapshot) {
                    if (initialize_snapshot)
                        std::memcpy(surface.source_snapshot.data(), source, row_bytes * surface.height);
                    else
                        std::memcpy(surface.source_snapshot.data() + offset, source + offset, bytes);
                    source = surface.source_snapshot.data();
                }
                std::memcpy(surface.memory + offset, source + offset, bytes);
                gpu_timing.color_publish_bytes += bytes;
                gpu_timing.color_bulk_readbacks++;
            } else for (uint32_t row = first_row; row < last_row; row++) {
                const uint8_t *source = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
                uint8_t *cached = snapshot ? surface.source_snapshot.data() + (size_t)row * row_bytes : nullptr;
                if (initialize_snapshot) std::memcpy(cached, source, row_bytes);
                for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                    size_t bytes = (size_t)(end - first) * 4;
                    if (cached && !initialize_snapshot) std::memcpy(cached + first * 4, source + first * 4, bytes);
                    std::memcpy(surface.memory + (size_t)row * surface.pitch + first * 4,
                                (cached ? cached : source) + first * 4, bytes);
                    gpu_timing.color_publish_bytes += bytes;
                });
            }
            surface.snapshot_valid = snapshot;
        }
        {
            GpuTimer unmap_timer(gpu_timing.sync_unmap);
            context->Unmap(surface.staging.Get(), 0);
        }
        mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
        surface.dirty = false;
    }
    if (!only && !publish_depth())
        for (auto &surface : depth_surfaces) surface.dirty = false;
    if (!only && publish_depth())
    for (auto &surface : depth_surfaces) {
        if (!surface.dirty) continue;
        if (surface.clear_value_valid) {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer depth_timer(gpu_timing.depth_publish);
            uint32_t bytes = surface.format == 1 ? 2 : 4;
            publish_known_clear(surface, bytes);
            gpu_timing.depth_publish_bytes += (uint64_t)surface.width * surface.height * bytes;
            gpu_timing.depth_clear_publications++;
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT result;
        {
            GpuTimer wait_timer(gpu_timing.sync_wait);
            result = context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
        if (FAILED(result)) readback_failed("depth", result);
        gpu_timing.depth_readbacks++;
        {
            GpuTimer publish_timer(gpu_timing.sync_publish);
            GpuTimer depth_timer(gpu_timing.depth_publish);
            uint32_t bytes = surface.format == 1 ? 2 : 4;
            bool snapshot = prepare_surface_snapshot(surface, bytes);
            bool initialize_snapshot = snapshot && !surface.snapshot_valid;
            const auto &region = surface.written_region;
            size_t row_bytes = (size_t)surface.width * bytes;
            uint32_t first_row = initialize_snapshot ? 0 : region.top;
            uint32_t last_row = initialize_snapshot ? surface.height : region.bottom;
            for (uint32_t row = first_row; row < last_row; row++) {
                uint8_t *cached = snapshot ? surface.source_snapshot.data() + (size_t)row * row_bytes : nullptr;
                const uint8_t *source = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
                if (initialize_snapshot) {
                    decode_depth_row(surface.format, source, cached, surface.width);
                    if (surface.format == 2) gpu_timing.depth_convert_bytes += row_bytes;
                }
                for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                    uint8_t *destination = surface.memory + (size_t)row * surface.pitch + first * bytes;
                    uint8_t *decoded = cached ? cached + first * bytes : destination;
                    size_t span_bytes = (size_t)(end - first) * bytes;
                    if (!initialize_snapshot) {
                        decode_depth_row(surface.format, source + first * bytes, decoded, end - first);
                        if (surface.format == 2) gpu_timing.depth_convert_bytes += span_bytes;
                    }
                    if (cached) std::memcpy(destination, decoded, span_bytes);
                    gpu_timing.depth_publish_bytes += span_bytes;
                });
            }
            surface.snapshot_valid = snapshot;
        }
        {
            GpuTimer unmap_timer(gpu_timing.sync_unmap);
            context->Unmap(surface.staging.Get(), 0);
        }
        mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
        surface.dirty = false;
    }
    pending_draws = false;
    if (!only) s_texture_validate_serial++;
}

static uint32_t resolve_pending_flip();
static bool pending_flip_overlaps(const void *addr, size_t bytes);
static bool cloak_debug();

/* Full sync: publish every dirty surface to guest RAM (flips, invalidate). */
extern "C" void nv2a_gpu_sync(void)
{
    if (resolve_pending_flip()) gpu_timing.async_resolved_sync++;
    gpu_sync_impl(true);
}

/* Targeted publish: pull just the source surface a GPU-engine blit is about to
 * read from guest RAM back into guest RAM. A 2D blit reads guest RAM while the
 * renderer may still hold the surface in a D3D11 texture (publish normally only
 * happens on flip), so a mid-frame CopyRects -- the loading-screen back-buffer
 * capture -- read a stale/black image. No texture-serial bump: not a frame
 * boundary. The staged flip only has to land first when it covers these bytes;
 * resolving it for every unrelated publish (the per-frame post-process alias
 * reads) forced a synchronous flip nearly every frame. */
extern "C" void nv2a_gpu_surface_publish(const void *addr, size_t bytes)
{
    if (pending_flip_overlaps(addr, bytes) && resolve_pending_flip())
        gpu_timing.async_resolved_publish++;
    gpu_sync_impl(true, addr, bytes);
}

/* Completion-only sync for ordering events that do not read surfaces back. */
extern "C" void nv2a_gpu_wait(void)
{
    gpu_sync_impl(false);
}

extern "C" void nv2a_gpu_flush(void)
{
    nv2a_gpu_sync();
    for (auto &surface : surfaces) surface.needs_refresh = true;
    for (auto &surface : depth_surfaces) surface.needs_refresh = true;
}

/* Frame boundary (FLIP_STALL): publish only the surface about to be shown,
 * advance the texture-validation serial once for the frame, and mark only CLEAN
 * surfaces for refresh.
 *
 * A flip only needs the presented surface in guest RAM, because that is the
 * only one the window and the title read back here; everything else can stay
 * GPU-resident until a fence that actually reads it. The full flush above
 * read back every dirty surface per flip. Dirty surfaces are deliberately not
 * marked needs_refresh: refreshing one would upload stale RAM over the rendered
 * image (see the guard in refresh_surface). If no surface is named, fall back
 * to the full flush. */

/* ---- Async (pipelined) flip readback -------------------------------------
 *
 * The synchronous flip copies the presented surface to its staging texture and
 * immediately Map()s it, which drains the GPU before the copy resolves -- the
 * ack thread blocks on the GPU once per frame (measured ~4 ms of a 25 ms frame
 * at 1080p, and the frame is CPU-bound on this thread). Instead, stage the copy
 * now and Map it on the *next* flip: the event query has signalled by then, so
 * the Map never waits. The window shows one frame behind, exactly what a real
 * double-buffered swapchain does.
 *
 * The staged copy's coverage and content_serial are frozen at stage time: the
 * title keeps rendering into the surface (the cloak refraction draws, then
 * blits the back buffer mid-frame) before the deferred publish runs, so the
 * publish must not clear dirty for -- or let the flip refresh-from-RAM over --
 * content written after the stage. Measured +19% (31.6 -> 37.5 FPS median at
 * 1080p gameplay). RECOMP_ASYNC_FLIP=0 disables it. */
/* Coverage of the staged copy, frozen at stage time: the live surface keeps
 * accumulating writes for the next frame before the deferred publish runs. */
struct StagedCoverage {
    D3D11_BOX written_region = {};
    std::vector<uint64_t> written_mask;
    uint32_t width = 0;
};
struct PendingFlip {
    const void *target = nullptr;
    size_t bytes = 0;
    uint32_t va = 0, pitch = 0;
    uint64_t content_serial = 0;
    StagedCoverage coverage;
    ComPtr<ID3D11Query> event;
    bool valid = false;
};
static PendingFlip s_pending_flip;

static bool pending_flip_overlaps(const void *addr, size_t bytes)
{
    if (!s_pending_flip.valid) return false;
    uintptr_t begin = (uintptr_t)addr, target = (uintptr_t)s_pending_flip.target;
    return begin < target + s_pending_flip.bytes && begin + bytes > target;
}

static bool async_flip_enabled()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_ASYNC_FLIP");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static Surface *surface_for_memory(const void *memory)
{
    for (auto &surface : surfaces)
        if (surface.memory == memory) return &surface;
    return nullptr;
}

/* The presented (front) surface is never CPU-diffed: the guest only reads it
 * back through a lock/fence, which takes the full-upload path. Maintaining its
 * source_snapshot in publish_staged_color therefore costs a second full-surface
 * memcpy every flip (staging -> snapshot -> guest RAM). RECOMP_PRESENT_SNAPSHOT=0
 * skips it, so the publish is one staging -> guest copy; a refresh, if one ever
 * happens, falls back to the full upload. Default on: a cloak/effect that
 * samples the front buffer regressed with it off, so keep the snapshot. */
static bool present_snapshot()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_PRESENT_SNAPSHOT");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/* Diagnostic: name the large textures that take the memcmp validation path, so
 * the hashing traffic can be attributed. RECOMP_HASHDBG=1. */
static bool hash_debug()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_HASHDBG");
        return value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/* Diagnostic: name which path a scene-sized texture binding takes in get_texture
 * (render-target feedback / zero-copy surface view / guest-RAM fallback), for
 * the render-target-as-texture cases. RECOMP_CLOAKDBG=1. */
static bool cloak_debug()
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_CLOAKDBG");
        return value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/* Publish one colour surface from its staging texture into guest RAM, over the
 * coverage that was staged. Mirrors gpu_sync_impl's publish loop. Clears dirty
 * only when nothing was rendered into the surface after staging (`current`):
 * otherwise the GPU texture is newer than guest RAM and the live content must
 * still be published (and must not be refreshed from RAM). */
static void publish_staged_color(Surface &surface, ID3D11Texture2D *staging,
                                 const StagedCoverage &coverage, bool current)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return;
    GpuTimer publish_timer(gpu_timing.sync_publish);
    GpuTimer color_timer(gpu_timing.color_publish);
    bool snapshot = present_snapshot() && prepare_surface_snapshot(surface, 4);
    bool initialize_snapshot = snapshot && !surface.snapshot_valid;
    const auto &region = coverage.written_region;
    size_t row_bytes = (size_t)surface.width * 4;
    uint32_t first_row = initialize_snapshot ? 0 : region.top;
    uint32_t last_row = initialize_snapshot ? surface.height : region.bottom;
    if (coverage.written_mask.empty() && region.left == 0 && region.right == surface.width &&
        mapped.RowPitch == row_bytes && surface.pitch == row_bytes) {
        const uint8_t *source = (const uint8_t *)mapped.pData;
        size_t offset = (size_t)region.top * row_bytes;
        size_t bytes = (size_t)(region.bottom - region.top) * row_bytes;
        if (snapshot) {
            if (initialize_snapshot)
                std::memcpy(surface.source_snapshot.data(), source, row_bytes * surface.height);
            else
                std::memcpy(surface.source_snapshot.data() + offset, source + offset, bytes);
            source = surface.source_snapshot.data();
        }
        std::memcpy(surface.memory + offset, source + offset, bytes);
        gpu_timing.color_publish_bytes += bytes;
        gpu_timing.color_bulk_readbacks++;
    } else for (uint32_t row = first_row; row < last_row; row++) {
        const uint8_t *source = (const uint8_t *)mapped.pData + (size_t)row * mapped.RowPitch;
        uint8_t *cached = snapshot ? surface.source_snapshot.data() + (size_t)row * row_bytes : nullptr;
        if (initialize_snapshot) std::memcpy(cached, source, row_bytes);
        for_each_written_span(coverage, row, [&](uint32_t first, uint32_t end) {
            size_t bytes = (size_t)(end - first) * 4;
            if (cached && !initialize_snapshot) std::memcpy(cached + first * 4, source + first * 4, bytes);
            std::memcpy(surface.memory + (size_t)row * surface.pitch + first * 4,
                        (cached ? cached : source) + first * 4, bytes);
            gpu_timing.color_publish_bytes += bytes;
        });
    }
    surface.snapshot_valid = snapshot;
    context->Unmap(staging, 0);
    mark_target_aliases(&surface, surface.memory, (size_t)surface.pitch * surface.height);
    if (current) surface.dirty = false;
}

/* Finish the deferred publish from the previous flip and return the guest VA
 * that is now displayable (0 if there was none). */
static uint32_t resolve_pending_flip()
{
    if (!s_pending_flip.valid) return 0;
    uint32_t va = s_pending_flip.va;
    Surface *surface = surface_for_memory(s_pending_flip.target);
    if (surface && s_pending_flip.event) {
        HRESULT result;
        while ((result = context->GetData(s_pending_flip.event.Get(), nullptr, 0,
                                          D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            xbox_Nv2aAcknowledgeHandshakes();
            SwitchToThread();
        }
        publish_staged_color(*surface, surface->staging.Get(), s_pending_flip.coverage,
                             surface->content_serial == s_pending_flip.content_serial);
    }
    s_pending_flip.valid = false;
    return va;
}

static bool stage_flip(const void *presented, size_t bytes, uint32_t va, uint32_t pitch)
{
    Surface *surface = surface_for_memory(presented);
    if (!surface) return false;
    ID3D11Texture2D *source = publish_source(*surface);
    if (surface->snapshot_valid) {
        const auto &region = surface->written_region;
        context->CopySubresourceRegion(surface->staging.Get(), 0, region.left, region.top, 0,
                                       source, 0, &region);
    } else {
        context->CopyResource(surface->staging.Get(), source);
    }
    if (!s_pending_flip.event) {
        D3D11_QUERY_DESC description = {D3D11_QUERY_EVENT, 0};
        if (FAILED(device->CreateQuery(&description, &s_pending_flip.event))) return false;
    }
    context->End(s_pending_flip.event.Get());
    context->Flush();
    s_pending_flip.target = presented;
    s_pending_flip.bytes = bytes;
    s_pending_flip.va = va;
    s_pending_flip.pitch = pitch;
    s_pending_flip.content_serial = surface->content_serial;
    s_pending_flip.coverage.written_region = surface->written_region;
    s_pending_flip.coverage.written_mask = surface->written_mask;
    s_pending_flip.coverage.width = surface->width;
    s_pending_flip.valid = true;
    return true;
}

/* What a flip does to the caches, whether the frame was published or not. */
static void apply_texture_replacements();

static void flip_bookkeeping()
{
    apply_target_sizes();
    apply_texture_replacements();
    /* Draws unmap their own streams; this only guards against a stray Map. */
    for (auto &stream : vertex_streams) release_stream(stream);
    release_stream(constant_stream);
    s_texture_validate_serial++;
    for (auto &surface : surfaces)
        if (!surface.dirty) surface.needs_refresh = true;
    for (auto &surface : depth_surfaces)
        if (!surface.dirty) surface.needs_refresh = true;
}

/* Returns the guest VA the caller should display this flip. In the default
 * (synchronous) path that is the buffer just published; with RECOMP_ASYNC_FLIP
 * it is the previous frame's buffer, resolved without a GPU wait. */
extern "C" uint32_t nv2a_gpu_flip(const void *presented, size_t bytes, uint32_t va, uint32_t pitch)
{
    { /* DIAG RECOMP_DEPTH_DUMP=<seconds>: save each 16-bit depth surface once, as a PGM */
        static int at = -2; static ULONGLONG start; static bool done;
        if (at == -2) { const char *e = std::getenv("RECOMP_DEPTH_DUMP"); at = e ? std::atoi(e) : -1; start = GetTickCount64(); }
        if (at >= 0 && !done && GetTickCount64() - start >= (ULONGLONG)at * 1000u) {
            done = true;
            int n = 0;
            for (auto &surface : depth_surfaces) {
                if (surface.format != 1) continue;
                context->CopyResource(surface.staging.Get(), publish_source(surface));
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (FAILED(context->Map(surface.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) continue;
                char path[64]; std::snprintf(path, sizeof path, "depth_%d_%ux%u.pgm", n++, surface.width, surface.height);
                FILE *f = std::fopen(path, "wb");
                unsigned lo = 65535, hi = 0;
                if (f) {
                    std::fprintf(f, "P5\n%u %u\n255\n", surface.width, surface.height);
                    for (uint32_t y = 0; y < surface.height; y++)
                        for (uint32_t x = 0; x < surface.width; x++) {
                            uint16_t v = ((const uint16_t *)((const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch))[x];
                            if (v < lo) lo = v; if (v > hi) hi = v;
                            std::fputc(v >> 8, f);
                        }
                    std::fclose(f);
                }
                context->Unmap(surface.staging.Get(), 0);
                std::fprintf(stderr, "[DEPTHDUMP] %s memory %p dirty %u range %u..%u\n", path,
                             (void *)surface.memory, (unsigned)surface.dirty, lo, hi);
            }
        }
    }
    if (!presented) { nv2a_gpu_flush(); return va; }
    uint32_t show = va;
    if (async_flip_enabled()) {
        show = resolve_pending_flip();
        if (show) gpu_timing.async_resolved_flip++;
        else {
            gpu_timing.async_flip_syncs++;
            /* Nothing staged to show: the first frame, or a mid-frame guest
             * readback (cloak refraction blit) already consumed the staged
             * frame and has since published a half-drawn frame into guest RAM.
             * Publish this frame synchronously so the window never shows the
             * mid-frame state; the stage below resumes the pipeline. */
            gpu_sync_impl(true, presented, bytes);
            show = va;
        }
        stage_flip(presented, bytes, va, pitch);
    } else {
        gpu_sync_impl(true, presented, bytes);
    }
    flip_bookkeeping();
    return show;
}

/* ── Window presentation ──────────────────────────────────────────────────
 *
 * A flip reaches the window through a flip-model swap chain on the window
 * fb_present opened: drawn from the presented surface's own texture, scaled
 * into the window at the frame's shape. The other route -- publish the surface
 * to guest RAM, convert it on the CPU, StretchDIBits it -- waits for the GPU
 * and reads the frame back every flip, and pins the window to 640x480. It
 * stays for frames whose pixels have to be in guest RAM (the movie overlay,
 * captures), which come here as CPU pixels instead. */
struct Presenter {
    HWND window = nullptr;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<ID3D11RenderTargetView> target;
    UINT width = 0, height = 0;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> pixels;
    ComPtr<ID3D11ShaderResourceView> pixels_view;
    UINT pixels_width = 0, pixels_height = 0;
    bool failed = false;
    /* DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING when the system has it: without it
     * a flip-model window presented with no sync interval still waits on
     * the compositor, so "vsync off" was held to the refresh rate (and a
     * variable-refresh display could not follow the frame rate). */
    UINT flags = 0;
};
static Presenter presenter;

static bool presenter_failed(const char *what, HRESULT result)
{
    std::fprintf(stderr, "[GPU-D3D11] window presentation unavailable: %s 0x%08X\n", what, (unsigned)result);
    presenter = Presenter{};
    presenter.failed = true;
    return false;
}

static bool presenter_target()
{
    ComPtr<ID3D11Texture2D> buffer;
    HRESULT result = presenter.chain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(result)) result = device->CreateRenderTargetView(buffer.Get(), nullptr, &presenter.target);
    return SUCCEEDED(result) || presenter_failed("back buffer", result);
}

static bool presenter_ready(HWND window)
{
    if (presenter.failed || !device || !context) return false;
    if (presenter.chain && presenter.window == window) {
        RECT client = {};
        GetClientRect(window, &client);
        UINT width = (UINT)std::max<LONG>(client.right - client.left, 0);
        UINT height = (UINT)std::max<LONG>(client.bottom - client.top, 0);
        if (!width || !height) return false;   /* minimised: nothing to show */
        if (width != presenter.width || height != presenter.height) {
            presenter.target.Reset();   /* unbound after every present */
            HRESULT result = presenter.chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, presenter.flags);
            if (FAILED(result)) return presenter_failed("resize", result);
            presenter.width = width; presenter.height = height;
            if (!presenter_target()) return false;
        }
        return true;
    }
    RECT client = {};
    GetClientRect(window, &client);
    presenter.width = (UINT)std::max<LONG>(client.right - client.left, 1);
    presenter.height = (UINT)std::max<LONG>(client.bottom - client.top, 1);
    ComPtr<IDXGIDevice1> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT result = device.As(&dxgi_device);
    if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(result)) return presenter_failed("DXGI factory", result);
    dxgi_device->SetMaximumFrameLatency(1);
    DXGI_SWAP_CHAIN_DESC1 description = {};
    description.Width = presenter.width; description.Height = presenter.height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH; description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    {
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
                SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) &&
                tearing)
            presenter.flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    description.Flags = presenter.flags;
    result = factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &presenter.chain);
    if (FAILED(result)) return presenter_failed("swap chain", result);
    /* The window toggles its own borderless fullscreen (fb_present, Alt+Enter);
     * DXGI's exclusive-mode switch would fight it. */
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    static const char source[] =
        "cbuffer PresentConstants:register(b0){float4 extent;};"
        "Texture2D<float4> image:register(t0);SamplerState smooth:register(s0);"
        "struct V{float4 position:SV_Position;float2 uv:TEXCOORD0;};"
        "V vs(uint id:SV_VertexID){V v;float2 t=float2((id<<1)&2,id&2);"
        "v.position=float4(t*float2(2,-2)+float2(-1,1),0,1);v.uv=t*extent.xy;return v;}"
        "float4 ps(V v):SV_Target{return float4(image.SampleLevel(smooth,v.uv,0).rgb,1);}";
    ComPtr<ID3DBlob> vertex, pixel, errors;
    result = D3DCompile(source, sizeof source - 1, "NV2A present", nullptr, nullptr, "vs", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors);
    if (SUCCEEDED(result))
        result = D3DCompile(source, sizeof source - 1, "NV2A present", nullptr, nullptr, "ps", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors);
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] present shader compile: %s\n",
                     errors ? (const char *)errors->GetBufferPointer() : "");
        return presenter_failed("shader compile", result);
    }
    result = device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &presenter.vertex);
    if (SUCCEEDED(result))
        result = device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &presenter.pixel);
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) result = device->CreateSamplerState(&sampler, &presenter.sampler);
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    if (SUCCEEDED(result)) result = device->CreateRasterizerState(&raster, &presenter.raster);
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(result)) result = device->CreateBlendState(&blend, &presenter.blend);
    D3D11_DEPTH_STENCIL_DESC depth = {};
    if (SUCCEEDED(result)) result = device->CreateDepthStencilState(&depth, &presenter.depth);
    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = 16; constants.Usage = D3D11_USAGE_DEFAULT; constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device->CreateBuffer(&constants, nullptr, &presenter.constants);
    if (FAILED(result)) return presenter_failed("pipeline", result);
    presenter.window = window;
    if (!presenter_target()) return false;
    std::fprintf(stderr, "[GPU-D3D11] window presentation: swap chain %ux%u\n", presenter.width, presenter.height);
    return true;
}

/* The shape of the last frame shown, width over height (0 before one). */
static volatile float presented_aspect;

extern "C" float nv2a_gpu_presented_aspect(void)
{
    return presented_aspect;
}

/* Draw `view` (its top-left u x v of the texture) letterboxed at `aspect`,
 * then present. */
static bool present_view(ID3D11ShaderResourceView *view, float u, float v, float aspect, int interval)
{
    ID3D11RenderTargetView *target = presenter.target.Get();
    const float black[4] = {0, 0, 0, 1};
    context->ClearRenderTargetView(target, black);
    float width = (float)presenter.width, height = (float)presenter.height;
    float shown_width = width, shown_height = width / aspect;
    presented_aspect = aspect;
    if (shown_height > height) { shown_height = height; shown_width = height * aspect; }
    D3D11_VIEWPORT viewport = {(width - shown_width) * 0.5f, (height - shown_height) * 0.5f,
                               shown_width, shown_height, 0.0f, 1.0f};
    const float extent[4] = {u, v, 0, 0};
    context->UpdateSubresource(presenter.constants.Get(), 0, nullptr, extent, 0, 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(presenter.blend.Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(presenter.depth.Get(), 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(presenter.vertex.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(presenter.pixel.Get(), nullptr, 0);
    ID3D11Buffer *buffer = presenter.constants.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    context->PSSetShaderResources(0, 1, &view);
    ID3D11SamplerState *sampler = presenter.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->RSSetState(presenter.raster.Get());
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    /* The next draw binds its own state; the surface just sampled may be its
     * render target. */
    ID3D11ShaderResourceView *no_view = nullptr;
    ID3D11RenderTargetView *no_target = nullptr;
    context->PSSetShaderResources(0, 1, &no_view);
    context->OMSetRenderTargets(1, &no_target, nullptr);
    HRESULT result = presenter.chain->Present(interval > 0 ? (UINT)interval : 0,
        interval <= 0 && (presenter.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
        return presenter_failed("present", result);
    return true;
}

extern "C" int nv2a_gpu_present_surface(void *window, const void *presented, uint32_t pitch, uint32_t rows,
                                        uint32_t aspect_w, uint32_t aspect_h, int interval)
{
    if (!window || !presented || !aspect_w || !aspect_h) return 0;
    Surface *shown = nullptr;
    for (auto &surface : surfaces)
        if (surface.memory == presented && surface.pitch == pitch && surface.view &&
                (!shown || surface.last_used > shown->last_used))
            shown = &surface;
    if (!shown) {
        static unsigned misses;
        if (misses++ < 4) {
            std::fprintf(stderr, "[GPU-D3D11] present: no surface at %p pitch %u; surfaces:", presented, pitch);
            for (auto &surface : surfaces)
                std::fprintf(stderr, " %p/%u/%ux%u", (void *)surface.memory, surface.pitch, surface.width, surface.height);
            std::fprintf(stderr, "\n");
        }
        return 0;
    }
    if (!presenter_ready((HWND)window)) return 0;
    /* Written by the CPU since the GPU last drew it (a loading screen): take
     * guest RAM's copy first, as a texture use would. */
    refresh_surface(*shown);
    resolve_color(*shown);
    uint32_t height = rows && rows < shown->height ? rows : shown->height;
    /* A widescreen target is wider than the title's aspect by its own shape. */
    float aspect = (float)aspect_w / (float)aspect_h *
                   ((float)shown->texture_width / (float)shown->width) / ((float)shown->texture_height / (float)shown->height);
    if (!present_view(shown->view.Get(), 1.0f, (float)height / (float)shown->height, aspect, interval))
        return 0;
    /* RECOMP_ASYNC_FLIP staged an earlier frame for the guest-RAM route;
     * resolving it now would show that old frame on the next such flip. */
    s_pending_flip.valid = false;
    flip_bookkeeping();
    return 1;
}

extern "C" int nv2a_gpu_present_pixels(void *window, const uint32_t *pixels, uint32_t width, uint32_t height,
                                       int interval)
{
    if (!window || !pixels || !width || !height || !presenter_ready((HWND)window)) return 0;
    if (!presenter.pixels || presenter.pixels_width != width || presenter.pixels_height != height) {
        presenter.pixels.Reset(); presenter.pixels_view.Reset();
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = width; description.Height = height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM; description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DYNAMIC; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT result = device->CreateTexture2D(&description, nullptr, &presenter.pixels);
        if (SUCCEEDED(result)) result = device->CreateShaderResourceView(presenter.pixels.Get(), nullptr, &presenter.pixels_view);
        if (FAILED(result)) return presenter_failed("frame texture", result);
        presenter.pixels_width = width; presenter.pixels_height = height;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(presenter.pixels.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 0;
    for (uint32_t y = 0; y < height; y++)
        std::memcpy((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, pixels + (size_t)y * width, (size_t)width * 4);
    context->Unmap(presenter.pixels.Get(), 0);
    return present_view(presenter.pixels_view.Get(), 1.0f, 1.0f, (float)width / (float)height, interval) ? 1 : 0;
}

/* A GPU-engine blit wrote guest RAM a cached surface mirrors. Refresh only the
 * surfaces that overlap those bytes from guest RAM; never bump the global
 * texture-validation serial, which would re-upload GPU-only textures (UI). */
extern "C" void nv2a_gpu_surface_modified(const void *addr, size_t bytes)
{
    uintptr_t begin = (uintptr_t)addr, end = begin + bytes;
    for (auto &surface : surfaces) {
        uintptr_t surface_begin = (uintptr_t)surface.memory;
        uintptr_t surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin < surface_end && end > surface_begin) surface.needs_refresh = true;
    }
    for (auto &texture : textures) {
        uintptr_t texture_begin = (uintptr_t)texture.source;
        uintptr_t texture_end = texture_begin + texture.source_snapshot.size();
        if (begin < texture_end && end > texture_begin) texture.force_validate = true;
    }
}

extern "C" void nv2a_gpu_invalidate(void)
{
    nv2a_gpu_sync();
    for (auto &stream : vertex_streams) release_stream(stream);
    release_stream(constant_stream);
    surfaces.clear();
    depth_surfaces.clear();
}

static bool matches_color_target(const Surface &surface, const Nv2aGpuDraw &state, TargetExtent extent)
{
    return surface.memory == state.color && surface.width == extent.width &&
        surface.height == extent.height && surface.pitch == state.pitch;
}

static bool matches_depth_target(const DepthSurface &surface, const Nv2aGpuDraw &state, TargetExtent extent)
{
    UINT texture_width, texture_height;
    texture_size(extent.width, extent.height, depth_scaled(state.depth), texture_width, texture_height);
    return surface.memory == state.depth && surface.width == extent.width &&
        surface.height == extent.height && surface.pitch == state.depth_pitch && surface.format == state.depth_format &&
        surface.texture_width == texture_width && surface.texture_height == texture_height;
}

static TargetExtent reusable_target_extent(const Nv2aGpuDraw &state)
{
    static const bool enabled = []() {
        const char *value = std::getenv("RECOMP_NV2A_COMPATIBLE_TARGETS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    TargetExtent chosen = {state.width, state.height};
    if (!enabled) return chosen;
    uint64_t area = 0;
    bool needs_depth = state.depth_enable || state.stencil_enable;
    // Keep a covering owner even when an exact smaller alias is already cached.
    auto consider = [&](uint32_t width, uint32_t height) {
        if (width < state.width || height < state.height || (uint64_t)width * height <= area) return;
        TargetExtent candidate = {width, height};
        if (needs_depth && state.color && std::none_of(depth_surfaces.begin(), depth_surfaces.end(),
            [&](const DepthSurface &surface) { return matches_depth_target(surface, state, candidate); })) return;
        chosen = candidate;
        area = (uint64_t)width * height;
    };
    if (state.color) {
        for (const auto &surface : surfaces)
            if (surface.memory == state.color && surface.pitch == state.pitch)
                consider(surface.width, surface.height);
    } else if (needs_depth) {
        for (const auto &surface : depth_surfaces)
            if (surface.memory == state.depth && surface.pitch == state.depth_pitch && surface.format == state.depth_format)
                consider(surface.width, surface.height);
    }
    if (chosen.width != state.width || chosen.height != state.height)
        gpu_timing.compatible_target_views++;
    return chosen;
}

template<typename Target>
static bool overlaps_draw_target(const Target &surface, const Nv2aGpuDraw &state)
{
    uintptr_t begin = (uintptr_t)surface.memory, end = begin + (size_t)surface.pitch * surface.height;
    uintptr_t color_begin = (uintptr_t)state.color, color_end = color_begin + (size_t)state.pitch * state.height;
    if (state.color && begin < color_end && end > color_begin) return true;
    if (state.depth && (state.depth_enable || state.stencil_enable)) {
        uintptr_t depth_begin = (uintptr_t)state.depth, depth_end = depth_begin + (size_t)state.depth_pitch * state.height;
        if (begin < depth_end && end > depth_begin) return true;
    }
    return false;
}

static void prepare_target_cache(const Nv2aGpuDraw &state, TargetExtent extent = {})
{
    if (!extent.width) extent = {state.width, state.height};
    auto color_conflict = [&](const Surface &surface) {
        return surface.dirty && !matches_color_target(surface, state, extent) && overlaps_draw_target(surface, state);
    };
    auto depth_conflict = [&](const DepthSurface &surface) {
        return surface.dirty && !matches_depth_target(surface, state, extent) && overlaps_draw_target(surface, state);
    };
    bool new_color = state.color && std::none_of(surfaces.begin(), surfaces.end(),
        [&](const Surface &surface) { return matches_color_target(surface, state, extent); });
    bool new_depth = (state.depth_enable || state.stencil_enable) &&
        std::none_of(depth_surfaces.begin(), depth_surfaces.end(),
            [&](const DepthSurface &surface) { return matches_depth_target(surface, state, extent); });
    bool layout_conflict = std::any_of(surfaces.begin(), surfaces.end(), color_conflict) ||
        std::any_of(depth_surfaces.begin(), depth_surfaces.end(), depth_conflict);
    if (!layout_conflict &&
        !(new_color && surfaces.size() >= 64) && !(new_depth && depth_surfaces.size() >= 64)) return;
    if (layout_conflict) gpu_timing.target_layout_syncs++;
    nv2a_gpu_sync();
    size_t old_count = surfaces.size() + depth_surfaces.size();
    if (new_color && surfaces.size() >= 64) {
        auto oldest = std::min_element(surfaces.begin(), surfaces.end(),
            [](const Surface &left, const Surface &right) { return left.last_used < right.last_used; });
        surfaces.erase(oldest);
    }
    if (new_depth && depth_surfaces.size() >= 64) {
        auto oldest = std::min_element(depth_surfaces.begin(), depth_surfaces.end(),
            [](const DepthSurface &left, const DepthSurface &right) { return left.last_used < right.last_used; });
        depth_surfaces.erase(oldest);
    }
    gpu_timing.target_evictions += old_count - surfaces.size() - depth_surfaces.size();
}

static bool clear_target_rectangle(const Nv2aGpuDraw &state, Surface *surface, DepthSurface *depth,
                                   bool clear_depth, bool clear_stencil, uint32_t color, uint32_t depth_value);

extern "C" int nv2a_gpu_clear(const Nv2aGpuDraw *state, uint32_t flags, uint32_t color, uint32_t depth_value)
{
    if (!state) {
        std::fprintf(stderr, "[GPU-D3D11] native clear has no surface state\n");
        return -1;
    }
    bool clear_color = (flags & 0xF0u) != 0;
    bool clear_depth = (flags & 1u) != 0;
    bool clear_stencil = (flags & 2u) != 0 && state->depth_format == 2;
    uint32_t depth_bytes = state->depth_format == 1 ? 2u : 4u;
    auto cpu_path = []() { gpu_timing.clear_fallbacks++; return 0; };
    if ((flags & ~0xF3u) || !state->width || !state->height || state->width > 4096 || state->height > 4096 ||
        state->clip_x || state->clip_y || state->clip_width != state->width || state->clip_height != state->height)
        return cpu_path();
    if (clear_color && ((flags & 0xF0u) != 0xF0u || !state->color || state->bytes_per_pixel != 4 ||
        state->pitch < state->width * 4)) return cpu_path();
    if ((flags & 3u) && (!state->depth || (state->depth_format != 1 && state->depth_format != 2) ||
        (state->control0 & 0x1000u) || state->depth_pitch < state->width * depth_bytes)) return cpu_path();
    // Arbitrary Z24 integers need a bit-exact CPU clear, not a floating-point clear value.
    if (clear_depth && state->depth_format == 2 && (depth_value >> 8) != 0 && (depth_value >> 8) != 0xFFFFFFu)
        return cpu_path();
    if (!clear_color && !clear_depth && !clear_stencil) return 1;
    if (clear_color && (clear_depth || clear_stencil)) {
        uintptr_t color_begin = (uintptr_t)state->color, depth_begin = (uintptr_t)state->depth;
        if (color_begin < depth_begin + (size_t)state->depth_pitch * state->height &&
            depth_begin < color_begin + (size_t)state->pitch * state->height) return cpu_path();
    }
    if (!nv2a_gpu_available()) return -1;
    Nv2aGpuDraw targets = *state;
    if (!clear_color) targets.color = nullptr;
    targets.depth_enable = clear_depth || clear_stencil;
    targets.stencil_enable = clear_stencil;
    if (!targets.depth_enable) targets.depth = nullptr;
    TargetExtent extent = reusable_target_extent(targets);
    bool partial = extent.width != state->width || extent.height != state->height;
    prepare_target_cache(targets, extent);
    Surface *surface = clear_color ? get_surface(targets, partial, extent) : nullptr;
    if (clear_color && !surface) return -1;
    bool preserve_depth = state->depth_format == 2 && !(clear_depth && clear_stencil);
    DepthSurface *depth = targets.depth_enable ? get_depth(targets, preserve_depth || partial, extent) : nullptr;
    if (targets.depth_enable && !depth) return -1;
    static const bool known_clears = []() {
        const char *value = std::getenv("RECOMP_NV2A_KNOWN_CLEARS");
        return !value || std::strcmp(value, "0") != 0;
    }();
    D3D11_BOX region = {0, 0, 0, state->width, state->height, 1};
    if (partial) {
        if (!clear_target_rectangle(*state, surface, depth, clear_depth, clear_stencil, color, depth_value)) return -1;
        gpu_timing.partial_target_clears++;
    } else if (surface) {
        const float rgba[] = {(float)((color >> 16) & 255u) / 255.0f, (float)((color >> 8) & 255u) / 255.0f,
                              (float)(color & 255u) / 255.0f, (float)(color >> 24) / 255.0f};
        context->ClearRenderTargetView(surface->target.Get(), rgba);
        if (surface->ms_texture) {
            context->ClearRenderTargetView(surface->ms_target.Get(), rgba);
            surface->ms_newer = surface->ms_stale = false;
        }
    }
    if (depth && !partial) {
        UINT clear_flags = (clear_depth ? D3D11_CLEAR_DEPTH : 0u) | (clear_stencil ? D3D11_CLEAR_STENCIL : 0u);
        /* ZSTENCIL_CLEAR_VALUE holds Z16 in its low 16 bits and Z24 above the
         * stencil byte (as xemu reads it). Shifting Z16 too turned Turok's
         * 0xFFFF "far" shadow-map clear into 0x00FF, shadowing everything. */
        float normalized = state->depth_format == 1 ? (float)(depth_value & 65535u) / 65535.0f :
            (float)(depth_value >> 8) / 16777215.0f;
        context->ClearDepthStencilView(depth->view.Get(), clear_flags, normalized, (UINT8)depth_value);
        if (depth->ms_texture) {
            /* Both, if both parts were cleared; else the twin is behind. */
            bool whole = (clear_depth && clear_stencil) || depth->format == 1;
            resolve_depth(*depth);
            context->ClearDepthStencilView(depth->view.Get(), clear_flags, normalized, (UINT8)depth_value);
            if (whole) {
                context->ClearDepthStencilView(depth->ms_view.Get(), clear_flags, normalized, (UINT8)depth_value);
                depth->ms_newer = depth->ms_stale = false;
            }
            else depth->ms_stale = true;
        }
    }
    if (surface) {
        mark_written_region(*surface, region);
        if (known_clears && !partial) { surface->clear_value = color; surface->clear_value_valid = true; }
    }
    if (depth) {
        bool known = known_clears && !partial && (depth->format == 1 ? clear_depth :
            (clear_depth && clear_stencil) || depth->clear_value_valid);
        uint32_t value = depth->format == 1 ? depth_value & 65535u :
            (clear_depth ? depth_value & 0xFFFFFF00u : depth->clear_value & 0xFFFFFF00u) |
            (clear_stencil ? depth_value & 255u : depth->clear_value & 255u);
        mark_written_region(*depth, region);
        depth->clear_value = value;
        depth->clear_value_valid = known;
    }
    pending_draws = true;
    gpu_timing.native_clears++;
    return 1;
}

static void release_stream(DynamicStream &stream)
{
    if (stream.active && stream.buffer) {
        context->Unmap(stream.buffer.Get(), 0);
        stream.active = false;
    }
}

/* A ring of dynamic memory: map, append, unmap -- the caller unmaps
 * (release_stream) before the draw that reads it. DISCARD when the ring wraps,
 * NO_OVERWRITE to append behind data earlier draws may still be reading.
 *
 * This used to hold one Map across all of a frame's draws. D3D11 only promises
 * the GPU sees what was written to a mapped buffer once it is unmapped, and a
 * native driver that uploads at Unmap fed every draw last frame's bytes at the
 * same offset. Standing still that is invisible; whenever the batches before a
 * draw changed size (Turok, about once a second), the weapon and HUD drew from
 * other draws' vertices and constants -- the flicker. */
static bool map_stream(DynamicStream &stream, UINT bytes, UINT bind, const char *kind, UINT &offset)
{
    if (!stream.buffer || bytes > stream.capacity) {
        release_stream(stream);
        /* The vertex+index ring resets every frame (release_stream at flip,
         * then the next map starts at 0). A frame carries ~15 MB of fixed
         * function vertices, so 512 KB wrapped it ~30-120x per frame and each
         * DISCARD became a DXVK rename/wait. Size it to hold a whole frame. */
        UINT capacity = (bind & D3D11_BIND_VERTEX_BUFFER) ? 32u * 1024 * 1024 :
                        (bind & D3D11_BIND_CONSTANT_BUFFER) ? 32u * 1024 * 1024 : 64 * 1024;
        while (capacity < bytes) capacity *= 2;
        D3D11_BUFFER_DESC description = {};
        description.ByteWidth = capacity; description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = bind; description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        if (!target_operation_succeeded(kind, "create stream", device->CreateBuffer(&description, nullptr, &buffer)))
            return false;
        stream.buffer = std::move(buffer);
        stream.capacity = capacity;
        stream.used = 0;
    }
    release_stream(stream);
    UINT aligned = (stream.used + 15u) & ~15u;
    bool discard = stream.used == 0 || aligned > stream.capacity || bytes > stream.capacity - aligned ||
                   ((bind & D3D11_BIND_CONSTANT_BUFFER) && !constant_no_overwrite);
    if (discard) aligned = 0;
    if (!target_operation_succeeded(kind, "map stream",
        context->Map(stream.buffer.Get(), 0, discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE,
                     0, &stream.mapped)))
        return false;
    stream.active = true;
    if (discard) {
        stream.generation++;
        gpu_timing.stream_discards++;
    } else {
        gpu_timing.stream_appends++;
    }
    offset = aligned;
    stream.used = aligned + bytes;
    return true;
}

static bool initialize_pipeline()
{
    if (input_layout) return true;
    if (!nv2a_gpu_compile()) return false;
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"FOG",0,DXGI_FORMAT_R32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, fog_coordinate),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, normal),D3D11_INPUT_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,(UINT)offsetof(Nv2aGpuVertex, weights),D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = sizeof(Constants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.Usage = D3D11_USAGE_DYNAMIC; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE; raster.DepthClipEnable = FALSE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constant_buffer)) ||
        FAILED(device->CreateRasterizerState(&raster, &rasterizer))) return false;
    uploaded_constants_valid = false;
    return SUCCEEDED(device->CreateInputLayout(elements, (UINT)(sizeof elements / sizeof elements[0]),
        vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), &input_layout));
}

/* A draw samples the surface it is rendering to (cloak refraction reads the
 * back buffer). D3D11 forbids one resource being SRV and RTV at once, so the
 * direct view samples as null. Snapshot the target's current content into a
 * scratch texture and sample that. One scratch is enough: in practice one
 * surface per draw feeds back, and the view is consumed immediately. */
static ComPtr<ID3D11Texture2D> feedback_texture;
static ComPtr<ID3D11ShaderResourceView> feedback_view;

static ID3D11ShaderResourceView *get_feedback_texture(const Surface &destination)
{
    if (!context || !destination.texture) return nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    destination.texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC have = {};
    if (feedback_texture) feedback_texture->GetDesc(&have);
    if (!feedback_texture || !feedback_view ||
            have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) {
        feedback_texture.Reset();
        feedback_view.Reset();
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &feedback_texture))) return nullptr;
        if (FAILED(device->CreateShaderResourceView(feedback_texture.Get(), nullptr, &feedback_view))) return nullptr;
    }
    resolve_color(const_cast<Surface &>(destination));
    context->CopyResource(feedback_texture.Get(), destination.texture.Get());
    return feedback_view.Get();
}

static ID3D11ShaderResourceView *default_white_view()
{
    static ComPtr<ID3D11ShaderResourceView> view;
    if (view) return view.Get();
    if (!device) return nullptr;
    const uint32_t white = 0xFFFFFFFFu;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {}; data.pSysMem = &white; data.SysMemPitch = 4;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) return nullptr;
    return view.Get();
}

/* Texture replacement: an image of the title's, known by a hash of its bytes
 * in guest memory, drawn as another (a PC version's button icon, say). */
struct TextureReplacement {
    uint32_t width, height;
    std::vector<uint32_t> pixels;   /* B8G8R8A8 */
};
static std::unordered_map<uint64_t, TextureReplacement> texture_replacements;
static bool replacements_on = true;
static std::atomic<int> pending_replacements_on{-1};

static uint64_t texture_hash(const void *source, size_t bytes)
{
    const uint8_t *data = (const uint8_t *)source;
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t index = 0; index < bytes; index++) hash = (hash ^ data[index]) * 0x100000001B3ull;
    return hash;
}

/* Before the title draws: not safe against a draw in progress. */
extern "C" void nv2a_gpu_replace_texture(uint64_t hash, const uint32_t *bgra, uint32_t width, uint32_t height)
{
    TextureReplacement replacement = {width, height, std::vector<uint32_t>(bgra, bgra + (size_t)width * height)};
    texture_replacements[hash] = std::move(replacement);
}

/* Replacements in use or not, from the next flip; any thread. */
extern "C" void nv2a_gpu_set_texture_replacements(int on)
{
    pending_replacements_on.store(on != 0);
}

/* At a flip: a switch drops the cached images it changes, to be made again. */
static void apply_texture_replacements()
{
    int on = pending_replacements_on.exchange(-1);
    if (on < 0 || (on != 0) == replacements_on) return;
    replacements_on = on != 0;
    for (size_t index = textures.size(); index-- > 0;) {
        const CachedTexture &texture = textures[index];
        if (texture.cube || texture.depth || texture.width > 256 || texture.height > 256) continue;
        if (texture_replacements.count(texture_hash(texture.source_snapshot.data(), texture.source_snapshot.size())))
            textures.erase(textures.begin() + (ptrdiff_t)index);
    }
    texture_index_count = (size_t)-1;
}

/* DIAG RECOMP_TEXTURE_DUMP=<dir>: each texture up to 256x256, the first time
 * its bytes are seen, as <hash>_<format>_<w>x<h>[_l].bin -- the guest bytes,
 * for finding a replacement's hash. */
static void dump_texture(const Nv2aGpuTexture &binding, uint64_t hash)
{
    static const char *directory = std::getenv("RECOMP_TEXTURE_DUMP");
    static std::set<uint64_t> dumped;
    if (!directory || binding.width > 256 || binding.height > 256 || !dumped.insert(hash).second) return;
    char path[MAX_PATH];
    std::snprintf(path, sizeof path, "%s\\%016llX_%02X_%ux%u%s.bin", directory, (unsigned long long)hash,
                  binding.format, binding.width, binding.height, binding.linear ? "_l" : "");
    if (FILE *file = std::fopen(path, "wb")) {
        std::fwrite(binding.source, 1, binding.source_bytes, file);
        std::fclose(file);
    }
}

static ID3D11ShaderResourceView *get_texture(const Nv2aGpuTexture &binding, const Surface &destination)
{
    GpuTimer texture_timer(gpu_timing.texture);
    gpu_timing.texture_calls++;
    gpu_timing.surface_count = surfaces.size();
    gpu_timing.depth_surface_count = depth_surfaces.size();
    if (binding.shadow) {
        /* A shadow-map stage reads the depth the GPU drew there; no guest
         * round trip. Null when no pass has drawn it yet (the caller lights
         * the stage). The latest drawn of its textures: one made at the
         * render scale before the map was seen drawn without colour (and so
         * kept at the title's size) is left behind, stale. */
        DepthSurface *shadow = nullptr;
        unsigned candidates = 0;
        for (auto &surface : depth_surfaces)
            if (surface.memory == binding.source && surface.width == binding.width &&
                    surface.height == binding.height && surface.pitch == binding.pitch) {
                candidates++;
                if (!shadow || surface.last_used > shadow->last_used) shadow = &surface;
            }
        if (!shadow) return nullptr;
        static std::set<const uint8_t *> reported;
        if (candidates > 1 && reported.insert(shadow->memory).second)
            std::fprintf(stderr, "[GPU-D3D11] shadow map %p: %u textures, reading the %ux%u one drawn last\n",
                         (const void *)shadow->memory, candidates, shadow->texture_width, shadow->texture_height);
        if (!shadow->dirty && !refresh_depth(*shadow)) return nullptr;
        shadow->last_used = ++surface_use_serial;
        resolve_depth(*shadow);
        return shadow->sample_view.Get();
    }
    static const bool mirror_aliases = []() {
        const char *setting = std::getenv("RECOMP_NV2A_GPU_ALIAS_COPIES");
        return !setting || std::strcmp(setting, "0") != 0;
    }();
    if (!binding.source || (binding.cube ? !binding.decode_face : !binding.decode) || !binding.width || !binding.height || binding.width > 4096 || binding.height > 4096) return nullptr;
    if (binding.depth && (!binding.decode_volume || binding.cube ||
        std::max({binding.width, binding.height, binding.depth}) > D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)) return nullptr;
    uint32_t levels = binding.mip_levels ? binding.mip_levels : 1;
    uint32_t maximum_levels = 1, dimension = std::max(binding.width, binding.height);
    dimension = std::max(dimension, binding.depth);
    while (dimension > 1) { dimension >>= 1; maximum_levels++; }
    if (levels > maximum_levels || (levels > 1 && (!binding.decode_level || binding.linear))) return nullptr;
    if (binding.cube && (binding.width != binding.height || !binding.face_stride ||
        (uint64_t)binding.face_stride * 6 > binding.source_bytes)) return nullptr;
    uintptr_t begin = (uintptr_t)binding.source, end = begin + binding.source_bytes;
    uintptr_t destination_begin = (uintptr_t)destination.memory;
    uintptr_t destination_end = destination_begin + (size_t)destination.pitch * destination.height;
    if (begin < destination_end && end > destination_begin) {
        /* Same surface as the render target (back-buffer feedback). Sample a
         * snapshot of it instead of the illegal SRV+RTV bind. Only the exact
         * full-target case maps cleanly onto the scratch texture. */
        if (binding.width == destination.width && binding.height == destination.height &&
                binding.linear && (binding.format == 0x12 || binding.format == 0x1E))
        {
            if (cloak_debug())
                std::fprintf(stderr, "[CLOAKDBG] feedback src=%p %ux%u fmt=%X linear=%u pitch=%u; dst %ux%u pitch=%u dirty=%u\n",
                             (const void *)binding.source, binding.width, binding.height,
                             binding.format, binding.linear, binding.pitch,
                             destination.width, destination.height, destination.pitch,
                             (unsigned)destination.dirty);
            return get_feedback_texture(destination);
        }
        if (cloak_debug())
            std::fprintf(stderr, "[CLOAKDBG] target-alias-NULL src=%p %ux%u fmt=%X linear=%u; dst %ux%u\n",
                         (const void *)binding.source, binding.width, binding.height,
                         binding.format, binding.linear, destination.width, destination.height);
        std::fprintf(stderr, "[GPU-D3D11] texture target alias: source %p bytes %llu size %ux%u pitch %u format %X linear %u; target %p size %ux%u pitch %u\n",
                     (const void *)binding.source, (unsigned long long)binding.source_bytes,
                     binding.width, binding.height, binding.pitch, binding.format, binding.linear,
                     (const void *)destination.memory, destination.width, destination.height, destination.pitch);
        return nullptr;
    }
    Surface *direct = nullptr;
    /* A texture whose memory exactly matches a colour surface can be sampled
     * straight from that surface's view. 0x12/0x1E (linear A8R8G8B8/X8R8G8B8)
     * always qualify. 0x07 is the same 32bpp surface sampled as a swizzled
     * texture (the common render-target-as-texture case, e.g. 0x99753000):
     * the surface view already holds the resolved pixels, so the guest-side
     * swizzle is irrelevant and requiring binding.linear would strand it on a
     * full GPU sync every frame. 0x06 (A8R8G8B8) is the same with alpha -- the
     * 512x512 in-mission effect target samples it that way every frame. A
     * swizzled binding has no pitch (binding.pitch is whatever the last linear
     * texture set), so match it against a tightly packed surface instead.
     * RECOMP_SWIZZLED_SURFACE_VIEW=0 restores the old 0x07/pitch-matched rule. */
    static const bool swizzled_views = []() {
        const char *value = std::getenv("RECOMP_SWIZZLED_SURFACE_VIEW");
        return !value || std::strcmp(value, "0") != 0;
    }();
    bool swizzled_view = swizzled_views ? (binding.format == 0x06 || binding.format == 0x07)
                                        : binding.format == 0x07;
    bool surface_viewable = (binding.format == 0x12 || binding.format == 0x1E)
                                ? binding.linear : swizzled_view;
    uint32_t want_pitch = swizzled_view && swizzled_views ? binding.width * 4 : binding.pitch;
    if (levels == 1 && !binding.cube && !binding.depth && surface_viewable)
        for (auto &surface : surfaces) {
            gpu_timing.surface_scan_iters++;
            if (surface.memory == binding.source && surface.width == binding.width &&
                surface.height == binding.height && surface.pitch == want_pitch) {
                direct = &surface;
                break;
            }
        }
    bool dirty_alias = false, copyable_aliases = mirror_aliases && direct && !direct->dirty;
    uint64_t alias_serial = 0;
    for (auto &surface : surfaces) {
        gpu_timing.surface_scan_iters++;
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin >= surface_end || end <= surface_begin) continue;
        if (&surface != direct && surface.dirty) {
            dirty_alias = true;
            if (!direct || surface.memory != direct->memory || surface.pitch != direct->pitch)
                copyable_aliases = false;
            alias_serial = std::max(alias_serial, surface.content_serial);
        }
    }
    for (auto &surface : depth_surfaces) {
        gpu_timing.surface_scan_iters++;
        uintptr_t surface_begin = (uintptr_t)surface.memory, surface_end = surface_begin + (size_t)surface.pitch * surface.height;
        if (begin < surface_end && end > surface_begin) {
            if (surface.dirty) { dirty_alias = true; copyable_aliases = false; }
        }
    }
    if (dirty_alias && copyable_aliases) {
        if (direct->alias_copy_serial != alias_serial || direct->needs_refresh) {
            direct->needs_refresh = true;
            refresh_surface(*direct);
            if (direct->alias_copy_serial != alias_serial) {
                for (auto &surface : surfaces) {
                    if (&surface == direct || !surface.dirty || surface.memory != direct->memory ||
                        surface.pitch != direct->pitch) continue;
                    resolve_color(surface);
                    resolve_color(*direct);
                    if (direct->ms_texture) direct->ms_stale = true;
                    D3D11_BOX region = surface.written_region;
                    region.right = std::min(region.right, direct->width);
                    region.bottom = std::min(region.bottom, direct->height);
                    if (region.left >= region.right || region.top >= region.bottom) continue;
                    auto copy = [&](const D3D11_BOX &guest_box) {
                        /* Both are colour targets, so scaled alike: the box in texture pixels. */
                        D3D11_BOX box = texture_box(*direct, guest_box);
                        box.right = std::min(box.right, std::min(direct->texture_width, surface.texture_width));
                        box.bottom = std::min(box.bottom, std::min(direct->texture_height, surface.texture_height));
                        if (box.left >= box.right || box.top >= box.bottom) return;
                        context->CopySubresourceRegion(direct->texture.Get(), 0, box.left, box.top, 0,
                                                      surface.texture.Get(), 0, &box);
                        direct->clear_value_valid = false;
                        gpu_timing.alias_copies++;
                        gpu_timing.alias_copy_bytes += (uint64_t)(box.right - box.left) * (box.bottom - box.top) * 4;
                    };
                    if (surface.written_mask.empty()) copy(region);
                    else for (uint32_t row = region.top; row < region.bottom; row++)
                        for_each_written_span(surface, row, [&](uint32_t first, uint32_t end) {
                            end = std::min(end, direct->width);
                            if (first < end) {
                                D3D11_BOX span = {first, row, 0, end, row + 1, 1};
                                copy(span);
                            }
                        });
                }
                // Sampling mirrors do not create guest writes; the dirty owner still publishes at real fences.
                direct->alias_copy_serial = alias_serial;
            } else gpu_timing.alias_copy_reuses++;
        } else gpu_timing.alias_copy_reuses++;
    } else if (dirty_alias) {
        gpu_timing.texture_alias_syncs++;
        if (cloak_debug()) {
            std::fprintf(stderr, "[ALIASSYNC] src=%p bytes=%llu %ux%u fmt=%X linear=%u pitch=%u depth=%u levels=%u direct=%p\n",
                         (const void *)binding.source, (unsigned long long)binding.source_bytes,
                         binding.width, binding.height, binding.format, binding.linear, binding.pitch,
                         (unsigned)binding.depth, (unsigned)levels, (void *)direct);
            for (const auto &surface : surfaces)
                if (surface.dirty && (uintptr_t)surface.memory < end &&
                    (uintptr_t)surface.memory + (size_t)surface.pitch * surface.height > begin)
                    std::fprintf(stderr, "[ALIASSYNC]   dirty color %p %ux%u pitch=%u\n",
                                 (const void *)surface.memory, surface.width, surface.height, surface.pitch);
            for (const auto &surface : depth_surfaces)
                if (surface.dirty && (uintptr_t)surface.memory < end &&
                    (uintptr_t)surface.memory + (size_t)surface.pitch * surface.height > begin)
                    std::fprintf(stderr, "[ALIASSYNC]   dirty depth %p %ux%u pitch=%u fmt=%u\n",
                                 (const void *)surface.memory, surface.width, surface.height, surface.pitch,
                                 (unsigned)surface.format);
        }
        /* Pull just this texture's source bytes back to guest RAM. A full sync
         * here republished every dirty surface once per frame, which alone made
         * the lazy flip worthless: the surface it was about to publish had
         * already been published by this call. */
        nv2a_gpu_surface_publish(binding.source, binding.source_bytes);
    }
    if (direct) {
        direct->last_used = ++surface_use_serial;
        if (cloak_debug())
            std::fprintf(stderr, "[CLOAKDBG] direct src=%p %ux%u fmt=%X linear=%u pitch=%u; surf %ux%u pitch=%u dirty=%u needs_refresh=%u snapshot_valid=%u\n",
                         (const void *)binding.source, binding.width, binding.height,
                         binding.format, binding.linear, binding.pitch,
                         direct->width, direct->height, direct->pitch,
                         (unsigned)direct->dirty, (unsigned)direct->needs_refresh,
                         (unsigned)direct->snapshot_valid);
        /* A surface marked dirty holds GPU content newer than guest RAM (it has
         * been drawn into but not published). Re-uploading the snapshot would
         * overwrite that with the previous publish, so sample the GPU texture
         * as-is and only refresh from guest RAM when the surface is clean.
         * This is also what lets a sync skip publication without feeding the
         * GPU stale data -- see RECOMP_SYNC_LIGHT. */
        if (!direct->dirty)
            refresh_surface(*direct);
        resolve_color(*direct);
        return direct->view.Get();
    }
    CachedTexture *refresh = nullptr;
    if (cloak_debug() && binding.source_bytes >= 1048576u)
        std::fprintf(stderr, "[CLOAKDBG] fallback src=%p %ux%u fmt=%X linear=%u pitch=%u (guest-RAM upload)\n",
                     (const void *)binding.source, binding.width, binding.height,
                     binding.format, binding.linear, binding.pitch);
    {
        GpuTimer hash_timer(gpu_timing.hash);
        gpu_timing.hash_bytes += binding.source_bytes;
        auto key_matches = [&](const CachedTexture &texture) {
            return texture.source == binding.source && texture.width == binding.width &&
                   texture.height == binding.height && texture.format == binding.format &&
                   texture.cube == binding.cube && texture.depth == binding.depth &&
                   texture.pitch == binding.pitch && texture.linear == binding.linear &&
                   texture.face_stride == binding.face_stride && texture.mip_levels == levels &&
                   texture.source_snapshot.size() == binding.source_bytes;
        };
        auto check = [&](CachedTexture &texture) -> ID3D11ShaderResourceView * {
            /* Already compared this entry this frame: the verdict still holds -- unless a
             * GPU blit overwrote it since, which forces a fresh compare. */
            if (tex_validate_once() && !texture.force_validate &&
                    texture.validated_serial == s_texture_validate_serial)
                return texture.view.Get();
            gpu_timing.memcmp_bytes += binding.source_bytes;
            if (hash_debug() && binding.source_bytes >= 262144u) {
                static std::vector<uintptr_t> seen;
                if (seen.size() < 64 && std::find(seen.begin(), seen.end(), (uintptr_t)binding.source) == seen.end()) {
                    seen.push_back((uintptr_t)binding.source);
                    const Surface *match = nullptr;
                    for (const auto &surface : surfaces)
                        if (surface.memory == binding.source) { match = &surface; break; }
                    std::fprintf(stderr, "[HASHDBG] src=0x%08llX bytes=%llu %ux%u fmt=%X linear=%u pitch=%u; surface=%s",
                                 (unsigned long long)(uintptr_t)binding.source,
                                 (unsigned long long)binding.source_bytes,
                                 binding.width, binding.height, binding.format, binding.linear, binding.pitch,
                                 match ? "yes" : "no");
                    if (match)
                        std::fprintf(stderr, " (w%u h%u pitch%u dirty%u)",
                                     match->width, match->height, match->pitch, (unsigned)match->dirty);
                    std::fprintf(stderr, "\n");
                }
            }
            if (std::memcmp(texture.source_snapshot.data(), binding.source, binding.source_bytes) == 0) {
                texture.validated_serial = s_texture_validate_serial;
                texture.force_validate = false;
                return texture.view.Get();
            }
            refresh = &texture;
            return nullptr;
        };
        if (texture_index_count != textures.size()) rebuild_texture_index();
        /* key_matches includes source equality, so only same-source entries can
         * match: a multimap bounds the search to that bucket instead of the
         * whole cache. */
        auto range = texture_index_by_source.equal_range(binding.source);
        int candidates = 0;
        for (auto it = range.first; it != range.second; ++it) {
            candidates++;
            gpu_timing.scan_steps++;
            if (!key_matches(textures[it->second])) continue;
            if (auto *view = check(textures[it->second])) return view;
            break;
        }
        if (candidates > 1) gpu_timing.scan_fallbacks++;
    }
    GpuTimer upload_timer(gpu_timing.upload);
    uint32_t faces = binding.cube ? 6 : 1;
    DXGI_FORMAT format = binding.format == 0xC ? DXGI_FORMAT_BC1_UNORM : binding.format == 0xE ? DXGI_FORMAT_BC2_UNORM :
                         binding.format == 0xF ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t block_bytes = binding.format == 0xC ? 8 : 16;
    bool compressed = format != DXGI_FORMAT_B8G8R8A8_UNORM;
    if (compressed && (binding.width % 4 || binding.height % 4)) {
        compressed = false; format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (binding.depth) {
        // D3D11 block-compressed resources are 2D; volumes need decoded texels.
        compressed = false;
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    std::vector<std::vector<uint32_t>> pixels(faces * levels);
    std::vector<D3D11_SUBRESOURCE_DATA> data(faces * levels);
    uint64_t resource_bytes = 0;
    for (uint32_t face = 0; face < faces; face++) {
        uint64_t source_offset = (uint64_t)face * binding.face_stride;
        uint32_t width = binding.width, height = binding.height, depth = binding.depth ? binding.depth : 1;
        for (uint32_t level = 0; level < levels; level++) {
            uint32_t subresource = face * levels + level;
            if (compressed) {
                uint32_t row_bytes = ((width + 3) / 4) * block_bytes;
                uint64_t level_bytes = (uint64_t)row_bytes * ((height + 3) / 4);
                if (source_offset + level_bytes > binding.source_bytes ||
                    (binding.cube && source_offset + level_bytes > (uint64_t)(face + 1) * binding.face_stride)) return nullptr;
                data[subresource].pSysMem = binding.source + source_offset;
                data[subresource].SysMemPitch = row_bytes;
                resource_bytes += level_bytes;
                source_offset += level_bytes;
            } else {
                auto &image = pixels[subresource];
                image.resize((size_t)width * height * depth);
                /* Linear 32bpp textures (0x12 A8R8G8B8, 0x1E X8R8G8B8) dominate
                 * the per-frame re-uploads. The generic path calls the decode
                 * callback once per texel; for these the guest bytes are already
                 * in the target layout, so copy whole rows instead (0x1E only
                 * needs alpha forced). Swizzled/compressed/format-shuffled ones
                 * keep the callback. */
                bool bulk = binding.linear && !binding.cube && !binding.depth && levels == 1 &&
                            (binding.format == 0x12 || binding.format == 0x1E);
                /* Swizzled 32bpp (0x06 A8R8G8B8, 0x07 X8R8G8B8) is the largest
                 * per-frame re-upload. The generic path calls the decode callback
                 * per texel; bulk-unswizzle by Morton order instead. */
                bool swizzled32 = !binding.linear && !binding.cube && !binding.depth && levels == 1 &&
                            (binding.format == 0x06 || binding.format == 0x07) &&
                            width && height && !(width & (width - 1)) && !(height & (height - 1)) &&
                            (uint64_t)width * height * 4 <= binding.source_bytes;
                if (bulk) {
                    const uint8_t *base = (const uint8_t *)binding.source;
                    for (uint32_t row = 0; row < height; row++) {
                        const uint32_t *source = (const uint32_t *)(base + (size_t)row * binding.pitch);
                        uint32_t *destination = &image[(size_t)row * width];
                        if (binding.format == 0x12) std::memcpy(destination, source, (size_t)width * 4);
                        else for (uint32_t column = 0; column < width; column++)
                            destination[column] = source[column] | 0xFF000000u;
                    }
                } else if (swizzled32) {
                    xbox_unswizzle_rect(image.data(), binding.source, width, height, 4);
                    if (binding.format == 0x07)
                        for (size_t pixel = 0; pixel < image.size(); pixel++) image[pixel] |= 0xFF000000u;
                } else {
                for (uint32_t slice = 0; slice < depth; slice++)
                 for (uint32_t row = 0; row < height; row++)
                    for (uint32_t column = 0; column < width; column++) {
                        uint32_t *pixel = &image[((size_t)slice * height + row) * width + column];
                        bool decoded = binding.depth ? binding.decode_volume(binding.decode_context, level, column, row, slice, pixel) :
                            binding.decode_level ? binding.decode_level(binding.decode_context, face, level, column, row, pixel) :
                            binding.cube ? binding.decode_face(binding.decode_context, face, column, row, pixel) :
                                           binding.decode(binding.decode_context, column, row, pixel);
                        if (!decoded) {
                            static unsigned logged;
                            if (logged++ < 16)
                                std::fprintf(stderr, "[GPU-D3D11] texture decode failed: fmt 0x%X %ux%u depth %u face %u level %u linear %u\n",
                                             binding.format, binding.width, binding.height, binding.depth, face, level, binding.linear);
                            gpu_timing.texture_decode_failures++;
                            return nullptr;
                        }
                        if (binding.depth) *pixel = (*pixel & 0xFF00FF00u) | ((*pixel & 255u) << 16) | ((*pixel >> 16) & 255u);
                    }
                }
                data[subresource].pSysMem = image.data(); data[subresource].SysMemPitch = width * 4;
                data[subresource].SysMemSlicePitch = width * height * 4;
                resource_bytes += (uint64_t)width * height * depth * 4;
            }
            if (width > 1) width >>= 1;
            if (height > 1) height >>= 1;
            if (depth > 1) depth >>= 1;
        }
    }
    if (refresh) {
        ComPtr<ID3D11Resource> resource;
        refresh->view->GetResource(&resource);
        for (uint32_t subresource = 0; subresource < faces * levels; subresource++)
            context->UpdateSubresource(resource.Get(), subresource, nullptr,
                                      data[subresource].pSysMem, data[subresource].SysMemPitch, data[subresource].SysMemSlicePitch);
        refresh->source_snapshot.assign(binding.source, binding.source + binding.source_bytes);
        refresh->validated_serial = s_texture_validate_serial;
        gpu_timing.texture_updated++;
        gpu_timing.texture_update_bytes += resource_bytes;
        return refresh->view.Get();
    }
    D3D11_TEXTURE2D_DESC description = {};
    description.Width = binding.width; description.Height = binding.height;
    description.MipLevels = levels; description.ArraySize = faces; description.Format = format;
    description.MiscFlags = binding.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    description.SampleDesc.Count = 1; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    /* A replacement for this image (nv2a_gpu_replace_texture): drawn instead,
     * at its own size. */
    const TextureReplacement *replacement = nullptr;
    if (!binding.cube && !binding.depth) {
        uint64_t hash = texture_hash(binding.source, binding.source_bytes);
        auto found = texture_replacements.find(hash);
        if (replacements_on && found != texture_replacements.end()) {
            replacement = &found->second;
            description.Width = replacement->width; description.Height = replacement->height;
            description.MipLevels = 1; description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            data[0].pSysMem = replacement->pixels.data(); data[0].SysMemPitch = replacement->width * 4;
        }
        dump_texture(binding, hash);
    }
    ComPtr<ID3D11Texture2D> texture;
    CachedTexture entry = {};
    entry.source = binding.source; entry.width = binding.width; entry.height = binding.height; entry.format = binding.format;
    entry.pitch = binding.pitch; entry.linear = binding.linear; entry.face_stride = binding.face_stride;
    entry.cube = binding.cube;
    entry.depth = binding.depth;
    entry.mip_levels = levels; entry.bytes = resource_bytes;
    entry.validated_serial = s_texture_validate_serial;
    if (binding.depth) {
        D3D11_TEXTURE3D_DESC volume_description = {};
        volume_description.Width = binding.width; volume_description.Height = binding.height;
        volume_description.Depth = binding.depth; volume_description.MipLevels = levels;
        volume_description.Format = format; volume_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> volume;
        if (FAILED(device->CreateTexture3D(&volume_description, data.data(), &volume)) ||
            FAILED(device->CreateShaderResourceView(volume.Get(), nullptr, &entry.view))) {
            gpu_timing.texture_create_failures++;
            return nullptr;
        }
    } else if (FAILED(device->CreateTexture2D(&description, data.data(), &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &entry.view))) {
        gpu_timing.texture_create_failures++;
        return nullptr;
    }
    gpu_timing.texture_created++;
    entry.source_snapshot.assign(binding.source, binding.source + binding.source_bytes);
    entry.bytes += entry.source_snapshot.size();
    uint64_t cache_bytes = entry.bytes;
    for (const auto &cached : textures) cache_bytes += cached.bytes;
    while (!textures.empty() && (textures.size() >= 1024 || cache_bytes > 256ull * 1024 * 1024)) {
        cache_bytes -= textures.front().bytes;
        textures.erase(textures.begin());
        gpu_timing.texture_evicted++;
    }
    textures.push_back(std::move(entry));
    texture_index_count = (size_t)-1;
    gpu_timing.texture_cache_entries = textures.size();
    gpu_timing.texture_cache_bytes = cache_bytes;
    return textures.back().view.Get();
}

static D3D11_BLEND blend_factor(uint32_t value, bool alpha)
{
    switch (value) {
    case 0: return D3D11_BLEND_ZERO; case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA; case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA; case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D11_BLEND_BLEND_FACTOR; case 0x8002: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return (D3D11_BLEND)0;
    }
}

static bool make_blend(const Nv2aGpuDraw &state, ComPtr<ID3D11BlendState> &result)
{
    D3D11_BLEND_DESC description = {};
    auto &target = description.RenderTarget[0];
    target.BlendEnable = state.blend_enable != 0;
    target.RenderTargetWriteMask = ((state.color_mask & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) |
        ((state.color_mask & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
        ((state.color_mask & 1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) |
        ((state.color_mask & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    target.SrcBlend = blend_factor(state.blend_source, false); target.DestBlend = blend_factor(state.blend_destination, false);
    target.SrcBlendAlpha = blend_factor(state.blend_source, true); target.DestBlendAlpha = blend_factor(state.blend_destination, true);
    switch (state.blend_equation) {
    case 0x8006: target.BlendOp = D3D11_BLEND_OP_ADD; break;
    case 0x800A: target.BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
    case 0x800B: target.BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case 0x8007: target.BlendOp = D3D11_BLEND_OP_MIN; break;
    case 0x8008: target.BlendOp = D3D11_BLEND_OP_MAX; break;
    default: if (state.blend_enable) return false; target.BlendOp = D3D11_BLEND_OP_ADD;
    }
    if (state.blend_enable && (!target.SrcBlend || !target.DestBlend)) return false;
    if (!state.blend_enable) target.SrcBlend = target.DestBlend = target.SrcBlendAlpha = target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = target.BlendOp;
    const std::array<uint32_t,7> key = {(uint32_t)target.BlendEnable, (uint32_t)target.SrcBlend,
        (uint32_t)target.DestBlend, (uint32_t)target.SrcBlendAlpha, (uint32_t)target.DestBlendAlpha,
        (uint32_t)target.BlendOp, target.RenderTargetWriteMask};
    for (const auto &variant : blend_variants) {
        if (variant.key == key) { result = variant.state; return true; }
    }
    if (FAILED(device->CreateBlendState(&description, &result))) return false;
    gpu_timing.blend_created++;
    if (blend_variants.size() >= 128) blend_variants.erase(blend_variants.begin());
    blend_variants.push_back({key, result});
    return true;
}

static D3D11_STENCIL_OP stencil_operation(uint32_t operation)
{
    switch (operation) {
    case 0x1E00: return D3D11_STENCIL_OP_KEEP;
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return (D3D11_STENCIL_OP)0;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE texture_address(uint32_t address)
{
    switch (address) {
    case 0: case 3: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4: case 5: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return (D3D11_TEXTURE_ADDRESS_MODE)0;
    }
}

static std::atomic<UINT> forced_anisotropy{1};

/* Anisotropic filtering, 1 (the title's own) to 16; any thread. */
extern "C" void nv2a_gpu_set_anisotropy(int level)
{
    forced_anisotropy.store(level >= 16 ? 16u : level >= 8 ? 8u : level >= 4 ? 4u : level >= 2 ? 2u : 1u);
}

static bool make_sampler(const Nv2aGpuTexture &binding, ComPtr<ID3D11SamplerState> &result);

/* A texture whose texels offset a later stage's lookup (BUMPENVMAP, modes 6
 * and 7) read point-sampled: each texel moves a square of the screen by one
 * amount, so a heat haze (Turok Evolution's flamethrower: a 128x128 noise
 * map over a 256x256 copy of the screen) breaks into hard-edged blocks, the
 * sharper the higher the render scale. Read filtered instead, the offsets
 * blend and the haze ripples. That is not the original look -- the console
 * draws the blocks too -- so it is opt-in: RECOMP_SMOOTH_BUMP=1. */
static bool bump_source_smoothing(const Nv2aGpuDraw *state, uint32_t stage, const Nv2aGpuTexture &binding,
                                  Nv2aGpuTexture &smoothed)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = std::getenv("RECOMP_SMOOTH_BUMP");
        enabled = value && *value && *value != '0';
    }
    if (!enabled) return false;
    bool source = false;
    for (uint32_t later = stage + 1; later < 4 && !source; later++) {
        uint32_t mode = (state->stage_program >> (later * 5)) & 31u;
        uint32_t from = later == 1 ? 0 : (state->shader_other_stage_input >> (later * 4 + 8)) & 15u;
        source = (mode == 6 || mode == 7) && from == stage;
    }
    if (!source) return false;
    uint32_t minimum = (binding.filter >> 16) & 255u, maximum = (binding.filter >> 24) & 15u;
    /* point -> linear: MIN 1 -> 2, 3 -> 4, 5 -> 6; MAG 1 -> 2 */
    uint32_t smooth_minimum = minimum == 1 || minimum == 3 || minimum == 5 ? minimum + 1 : minimum;
    uint32_t smooth_maximum = maximum == 1 ? 2u : maximum;
    if (smooth_minimum == minimum && smooth_maximum == maximum) return false;
    smoothed = binding;
    smoothed.filter = (binding.filter & ~0x0FFF0000u) | (smooth_maximum << 24) | (smooth_minimum << 16);
    return true;
}

static bool make_sampler(const Nv2aGpuTexture &binding, ComPtr<ID3D11SamplerState> &result)
{
    uint32_t minimum = (binding.filter >> 16) & 255u, maximum = (binding.filter >> 24) & 15u;
    if (!minimum) minimum = 1;
    if (!maximum) maximum = 1;
    /* The 2D convolution kernels (MIN 7 CONVOLUTION_2D_LOD0, MAG 4
     * CONVOLUTION_2D) have no D3D11 sampler. Xbox D3D uses them to resolve an
     * anti-aliased surface into the displayed one (Turok Evolution's
     * 1280x480 -> 640x480 present, filter 0x04072000); rejecting them dropped
     * that draw, so nothing reached the screen. Bilinear is the nearest
     * built-in equivalent. */
    if (minimum == 7) minimum = 2;
    if (maximum == 4) maximum = 2;
    if (minimum > 6 || maximum > 2) return false;
    D3D11_SAMPLER_DESC description = {};
    description.AddressU = texture_address(binding.address_u);
    description.AddressV = texture_address(binding.address_v);
    description.AddressW = texture_address(binding.address_w);
    if (!description.AddressU || !description.AddressV || !description.AddressW) return false;
    description.Filter = (D3D11_FILTER)D3D11_ENCODE_BASIC_FILTER(
        minimum == 2 || minimum == 4 || minimum == 6 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        maximum == 2 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
        minimum >= 5 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    int32_t bias = (int32_t)(binding.filter & 0x1FFFu);
    if (bias & 0x1000) bias -= 0x2000;
    description.MipLODBias = (float)bias / 256.0f;
    if (description.MipLODBias > D3D11_MIP_LOD_BIAS_MAX) return false;
    description.MaxAnisotropy = binding.control0_valid ? 1u << ((binding.control0 >> 4) & 3u) : 1u;
    /* Anisotropic filtering asked for by the player (nv2a_gpu_set_anisotropy),
     * on mipmapped, linearly filtered textures: the title's, not its 2D. */
    UINT forced = forced_anisotropy.load();
    if (forced > description.MaxAnisotropy && maximum == 2 && minimum == 6) description.MaxAnisotropy = forced;
    if (description.MaxAnisotropy > 1) {
        if (maximum != 2 || (minimum != 2 && minimum != 6)) return false;
        description.Filter = D3D11_FILTER_ANISOTROPIC;
    }
    description.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    description.MaxLOD = D3D11_FLOAT32_MAX;
    if (minimum <= 2) {
        description.MinLOD = description.MaxLOD = 0;
    } else if (binding.control0_valid) {
        description.MinLOD = (float)((binding.control0 >> 18) & 0xFFFu) / 256.0f;
        description.MaxLOD = (float)((binding.control0 >> 6) & 0xFFFu) / 256.0f;
        if (description.MinLOD > description.MaxLOD) return false;
    }
    nv_cpu_unpack_argb(binding.border_color, description.BorderColor);
    std::array<uint32_t,9> key = {(uint32_t)description.Filter, (uint32_t)description.AddressU,
        (uint32_t)description.AddressV, (uint32_t)description.AddressW, binding.filter & 0x1FFFu,
        minimum > 2 && binding.control0_valid ? (binding.control0 >> 18) & 0xFFFu : 0,
        minimum <= 2 ? 0 : binding.control0_valid ? (binding.control0 >> 6) & 0xFFFu : UINT32_MAX,
        binding.border_color, description.MaxAnisotropy};
    for (const auto &entry : sampler_cache)
        if (entry.key == key) { result = entry.state; return true; }
    if (FAILED(device->CreateSamplerState(&description, &result))) return false;
    if (sampler_cache.size() >= 256) sampler_cache.clear();
    sampler_cache.push_back({key, result});
    gpu_timing.sampler_created++;
    return true;
}

static bool make_rasterizer(const Nv2aGpuDraw &state, ComPtr<ID3D11RasterizerState> &result)
{
    uint32_t front = state.front_face ? state.front_face : 0x900;
    uint32_t front_fill = state.polygon_front ? state.polygon_front : 0x1B02;
    uint32_t back_fill = state.polygon_back ? state.polygon_back : 0x1B02;
    if (front != 0x900 && front != 0x901) return false;
    D3D11_RASTERIZER_DESC description = {};
    description.CullMode = D3D11_CULL_NONE;
    if (state.cull_enable && state.topology == NV2A_GPU_TOPOLOGY_TRIANGLES) {
        switch (state.cull_face) {
        case 0x404: description.CullMode = D3D11_CULL_FRONT; break;
        case 0x405: description.CullMode = D3D11_CULL_BACK; break;
        case 0x408: break;
        default: return false;
        }
    }
    if (state.topology != NV2A_GPU_TOPOLOGY_TRIANGLES) front_fill = back_fill = 0x1B02;
    uint32_t fill = description.CullMode == D3D11_CULL_FRONT ? back_fill : front_fill;
    if (description.CullMode == D3D11_CULL_NONE && front_fill != back_fill && !(state.cull_enable && state.cull_face == 0x408)) return false;
    if (fill != 0x1B01 && fill != 0x1B02) return false;
    description.FillMode = fill == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    description.FrontCounterClockwise = front == 0x901;
    description.ScissorEnable = TRUE; description.DepthClipEnable = FALSE;
    uint32_t key = ((uint32_t)description.CullMode - 1) * 4 + (description.FrontCounterClockwise ? 2u : 0u) + (fill == 0x1B01 ? 1u : 0u);
    auto &entry = raster_states[key];
    if (!entry && FAILED(device->CreateRasterizerState(&description, &entry))) return false;
    result = entry;
    return true;
}

static bool rectangle_contains(const D3D11_RECT &outer, const D3D11_RECT &inner)
{
    return outer.left <= inner.left && outer.top <= inner.top &&
           outer.right >= inner.right && outer.bottom >= inner.bottom;
}

static void configure_window_clip(const Nv2aGpuDraw &state, Constants &constants, D3D11_RECT &clip)
{
    if (!state.window_clip_valid || clip.left >= clip.right || clip.top >= clip.bottom) return;
    std::array<D3D11_RECT,8> rectangles = {};
    uint32_t count = 0;
    for (uint32_t rectangle = 0; rectangle < 8; rectangle++) {
        uint32_t horizontal = state.window_clip_horizontal[rectangle];
        uint32_t vertical = state.window_clip_vertical[rectangle];
        /* Guest maxima are inclusive; native scissors and shader boxes are half-open. */
        D3D11_RECT region = {(LONG)(horizontal & 4095u), (LONG)(vertical & 4095u),
                            (LONG)(((horizontal >> 16) & 4095u) + 1),
                            (LONG)(((vertical >> 16) & 4095u) + 1)};
        region.left = std::max(region.left, clip.left); region.top = std::max(region.top, clip.top);
        region.right = std::min(region.right, clip.right); region.bottom = std::min(region.bottom, clip.bottom);
        if (region.left >= region.right || region.top >= region.bottom) continue;
        bool contained = false;
        for (uint32_t previous = 0; previous < count; previous++)
            contained |= rectangle_contains(rectangles[previous], region);
        if (contained) continue;
        for (uint32_t previous = 0; previous < count;) {
            if (rectangle_contains(region, rectangles[previous])) rectangles[previous] = rectangles[--count];
            else previous++;
        }
        rectangles[count++] = region;
    }
    if (!count) {
        if (!state.window_clip_type) { clip.right = clip.left; clip.bottom = clip.top; }
        return;
    }
    if (!state.window_clip_type && count == 1) { clip = rectangles[0]; return; }
    if (state.window_clip_type && count == 1 && rectangle_contains(rectangles[0], clip)) {
        clip.right = clip.left; clip.bottom = clip.top;
        return;
    }
    constants.window_clip_control[0] = state.window_clip_type ? 2u : 1u;
    constants.window_clip_control[1] = count;
    for (uint32_t rectangle = 0; rectangle < count; rectangle++) {
        const auto &region = rectangles[rectangle];
        constants.window_clip_rects[rectangle][0] = (uint32_t)region.left;
        constants.window_clip_rects[rectangle][1] = (uint32_t)region.top;
        constants.window_clip_rects[rectangle][2] = (uint32_t)region.right;
        constants.window_clip_rects[rectangle][3] = (uint32_t)region.bottom;
    }
    if (!state.window_clip_type) {
        clip = rectangles[0];
        for (uint32_t rectangle = 1; rectangle < count; rectangle++) {
            clip.left = std::min(clip.left, rectangles[rectangle].left);
            clip.top = std::min(clip.top, rectangles[rectangle].top);
            clip.right = std::max(clip.right, rectangles[rectangle].right);
            clip.bottom = std::max(clip.bottom, rectangles[rectangle].bottom);
        }
    }
}

static bool clear_target_rectangle(const Nv2aGpuDraw &state, Surface *surface, DepthSurface *depth,
                                   bool clear_depth, bool clear_stencil, uint32_t color, uint32_t depth_value)
{
    struct ClearPipeline {
        ComPtr<ID3D11VertexShader> vertex;
        ComPtr<ID3D11PixelShader> pixel;
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11RasterizerState> raster;
        ComPtr<ID3D11BlendState> blend[2];
        ComPtr<ID3D11DepthStencilState> depth[4];
    };
    static ClearPipeline pipeline;
    if (!pipeline.vertex) {
        static const char source[] =
            "cbuffer ClearConstants:register(b0){float4 clearColor;float4 clearDepth;};"
            "float4 vs(uint id:SV_VertexID):SV_Position{"
            "float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);return float4(p,clearDepth.x,1);}"
            "float4 ps():SV_Target{return clearColor;}";
        ClearPipeline created;
        ComPtr<ID3DBlob> vertex, pixel, errors;
        HRESULT result = D3DCompile(source, sizeof source - 1, "NV2A clear", nullptr, nullptr,
                                   "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors);
        if (SUCCEEDED(result))
            result = D3DCompile(source, sizeof source - 1, "NV2A clear", nullptr, nullptr,
                                "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] rectangle clear shader compile: 0x%08X %s\n",
                         (unsigned)result, errors ? (const char *)errors->GetBufferPointer() : "");
            return false;
        }
        if (!target_operation_succeeded("rectangle clear", "create vertex shader",
                device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &created.vertex)) ||
            !target_operation_succeeded("rectangle clear", "create pixel shader",
                device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &created.pixel))) return false;
        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!target_operation_succeeded("rectangle clear", "create constants",
                device->CreateBuffer(&buffer, nullptr, &created.constants))) return false;
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE; raster.ScissorEnable = TRUE;
        if (!target_operation_succeeded("rectangle clear", "create rasterizer",
                device->CreateRasterizerState(&raster, &created.raster))) return false;
        for (uint32_t index = 0; index < 2; index++) {
            D3D11_BLEND_DESC blend = {};
            blend.RenderTarget[0].RenderTargetWriteMask = index ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
            blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            if (!target_operation_succeeded("rectangle clear", "create blend state",
                    device->CreateBlendState(&blend, &created.blend[index]))) return false;
        }
        for (uint32_t index = 0; index < 4; index++) {
            D3D11_DEPTH_STENCIL_DESC description = {};
            description.DepthEnable = (index & 1u) != 0;
            description.DepthWriteMask = index & 1u ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            description.DepthFunc = D3D11_COMPARISON_ALWAYS;
            description.StencilEnable = (index & 2u) != 0;
            description.StencilReadMask = description.StencilWriteMask = 255;
            description.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            description.FrontFace.StencilFailOp = description.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
            description.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
            description.BackFace = description.FrontFace;
            if (!target_operation_succeeded("rectangle clear", "create depth/stencil state",
                    device->CreateDepthStencilState(&description, &created.depth[index]))) return false;
        }
        pipeline = std::move(created);
    }
    const float constants[8] = {
        (float)((color >> 16) & 255u) / 255.0f, (float)((color >> 8) & 255u) / 255.0f,
        (float)(color & 255u) / 255.0f, (float)(color >> 24) / 255.0f,
        state.depth_format == 1 ? (float)(depth_value & 65535u) / 65535.0f :
            (float)(depth_value >> 8) / 16777215.0f, 0, 0, 0
    };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!target_operation_succeeded("rectangle clear", "map constants",
            context->Map(pipeline.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, constants, sizeof constants);
    context->Unmap(pipeline.constants.Get(), 0);
    ID3D11ShaderResourceView *empty[12] = {};
    context->PSSetShaderResources(0, 12, empty);
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *depth_target = nullptr;
    bind_draw_targets(surface, depth, target, depth_target);
    context->OMSetRenderTargets(surface ? 1 : 0, surface ? &target : nullptr, depth_target);
    context->OMSetBlendState(pipeline.blend[surface ? 1 : 0].Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(pipeline.depth[(clear_depth ? 1 : 0) | (clear_stencil ? 2 : 0)].Get(), depth_value & 255u);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(pipeline.vertex.Get(), nullptr, 0);
    context->PSSetShader(pipeline.pixel.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer *buffer = pipeline.constants.Get();
    context->VSSetConstantBuffers(0, 1, &buffer);
    context->PSSetConstantBuffers(0, 1, &buffer);
    /* The rectangle is the title's; the target may be larger by the render scale. */
    uint32_t guest_width = surface ? surface->width : depth->width, guest_height = surface ? surface->height : depth->height;
    uint32_t texture_width = surface ? surface->texture_width : depth->texture_width;
    uint32_t texture_height = surface ? surface->texture_height : depth->texture_height;
    D3D11_RECT clip = {0, 0, scale_edge((LONG)state.width, guest_width, texture_width),
                       scale_edge((LONG)state.height, guest_height, texture_height)};
    D3D11_VIEWPORT viewport = {0, 0, (float)clip.right, (float)clip.bottom, 0, 1};
    context->RSSetState(pipeline.raster.Get());
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &clip);
    context->Draw(3, 0);
    return true;
}

extern "C" int nv2a_gpu_draw(const Nv2aGpuDraw *state, Nv2aGpuVertex *vertices, uint32_t count)
{
    GpuTimer draw_timer(gpu_timing.draw);
    GpuTimer phase_timer(gpu_timing.draw_setup);
    auto reject = [](const char *reason) {
        std::fprintf(stderr, "[GPU-D3D11] rejected: %s\n", reason);
        return 0;
    };
    /* No colour pointer on a 16-bit target: a depth-only pass (see
     * gpu_raster_batch), drawn with no render target bound. */
    bool depth_only = state && !state->color && state->bytes_per_pixel == 2;
    if (!state || !vertices || !count || count > NV2A_GPU_MAX_VERTICES || (!state->color && !depth_only) ||
        !state->width || !state->height || state->width > 4096 || state->height > 4096 ||
        (!depth_only && (state->pitch < state->width * 4 || state->bytes_per_pixel != 4)) ||
        (state->combiners.control & 255u) > 8 || !initialize_pipeline()) return 0;
    if (depth_only && !state->depth_enable && !state->stencil_enable) return 1;   /* nothing it can change */
    if (state->topology > NV2A_GPU_TOPOLOGY_POINTS) return reject("primitive topology");
    uint32_t primitive_width = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES ? 3u :
                               state->topology == NV2A_GPU_TOPOLOGY_LINES ? 2u : 1u;
    uint32_t element_count = state->indices ? state->index_count : count;
    if (element_count < primitive_width || element_count % primitive_width ||
        (state->indices && element_count > NV2A_GPU_MAX_INDICES)) return reject("primitive element count");
    if (state->topology == NV2A_GPU_TOPOLOGY_POINTS && state->point_size && state->point_size != 8)
        return reject("non-unit point size");
    if (state->indices) {
        for (uint32_t index = 0; index < state->index_count; index++)
            if (state->indices[index] >= count) return reject("vertex index extent");
    }
    if (state->window_clip_valid && state->window_clip_type > 1) return reject("window-clip type");
    if (state->clip_width > UINT32_MAX - state->clip_x || state->clip_height > UINT32_MAX - state->clip_y)
        return reject("surface scissor extent");
    if (state->fixed_transform) {
        if (state->vertex_program) return reject("conflicting vertex pipelines");
        if (state->skin_mode > 6) {
            static int skin_shown;
            if (skin_shown++ < 8)
                std::fprintf(stderr, "[GPU-D3D11] skin_mode=%u (0x%08X)\n",
                             state->skin_mode, state->skin_mode);
            return reject("fixed-function skin mode");
        }
        if (state->texgen_view_model > 1) return reject("texgen view model");
        for (uint32_t stage = 0; stage < 4; stage++) {
            for (uint32_t component = 0; component < 4; component++) {
                uint32_t mode = state->texgen[stage][component];
                if (mode == 0 || mode == 0x2400 || mode == 0x2401) continue;
                if ((mode == 0x2402 || mode == 0x8512) && state->texgen_view_model != 0)
                    return reject("infinite-viewer reflection texgen");
                if (mode == 0x2402 && component < 2) continue;
                if ((mode == 0x8511 || mode == 0x8512) && component < 3) continue;
                return reject("texgen mode or component");
            }
        }
        if (state->lighting_enable) {
            for (uint32_t source = 0; source < 4; source++)
                if (((state->color_material >> (source * 2)) & 3u) == 3u)
                    return reject("reserved color-material source");
            if (state->specular_enable && (!nv_cpu_finite(state->specular_power) || state->specular_power < 0))
                return reject("nonfinite or negative specular exponent");
        }
    }
    if (state->fog_enable) {
        if (state->fog_mode != 0x2601 && (state->fog_mode < 0x800 || state->fog_mode > 0x804)) return reject("fog mode");
        if (!state->vertex_program && state->fog_gen_mode > 3 && state->fog_gen_mode != 6) return reject("fog coordinate generation");
        if (!state->vertex_program && state->fog_gen_mode >= 1 && state->fog_gen_mode <= 3 && !state->fixed_transform)
            return reject("eye-space fog requires fixed transform");
    }
    if (state->depth_enable && ((state->control0 & 0x1000u) || state->depth_function < 0x200 || state->depth_function > 0x207)) return 0;
    uint32_t depth_clamp = (state->zmin_max_control >> 4) & 15u;
    float depth_clip_min = state->depth_range_valid & 1u ? state->depth_clip_min : 0.0f;
    float depth_clip_max = state->depth_range_valid & 2u ? state->depth_clip_max : state->depth_format == 1 ? 65535.0f : 16777215.0f;
    if (depth_clamp > 1 || !nv_cpu_finite(depth_clip_min) || !nv_cpu_finite(depth_clip_max) ||
        (depth_clamp && depth_clip_min > depth_clip_max)) return reject("depth clip range or clamp mode");
    auto offset_enabled = [&](uint32_t mode) {
        if (!mode) mode = 0x1B02;
        return mode >= 0x1B00 && mode <= 0x1B02 && ((state->polygon_offset_enable >> (mode - 0x1B00)) & 1u) != 0;
    };
    bool front_offset = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES &&
                        offset_enabled(state->polygon_front) && (!state->cull_enable || state->cull_face == 0x405);
    bool back_offset = state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES &&
                       offset_enabled(state->polygon_back) && (!state->cull_enable || state->cull_face == 0x404);
    if (front_offset || back_offset) {
        if (!nv_cpu_finite(state->polygon_offset_scale) || !nv_cpu_finite(state->polygon_offset_bias))
            return reject("nonfinite polygon offset");
    }
    if (state->stencil_enable && (state->depth_format != 2 || state->stencil_function < 0x200 || state->stencil_function > 0x207 ||
        !stencil_operation(state->stencil_fail) || !stencil_operation(state->stencil_depth_fail) || !stencil_operation(state->stencil_pass)))
        return reject("stencil format, comparison or operation");
    for (uint32_t stage = 0; stage < (state->combiners.control & 255u); stage++) {
        const uint32_t outputs[2] = {state->combiners.rgb_output[stage], state->combiners.alpha_output[stage]};
        for (uint32_t path = 0; path < 2; path++) {
            uint32_t mapping = (outputs[path] >> 15) & 7;
            if (mapping == 5 || mapping == 7) return reject("combiner output mapping");
            for (uint32_t product = 0; product < 3; product++) {
                uint32_t destination = (outputs[path] >> (product * 4)) & 15;
                if (destination && destination != 4 && destination != 5 && (destination < 8 || destination > 13)) return reject("combiner destination register");
            }
        }
    }
    ComPtr<ID3D11BlendState> blending;
    if (!make_blend(*state, blending)) return reject("blend state");
    ComPtr<ID3D11RasterizerState> raster_state;
    if (!make_rasterizer(*state, raster_state)) return reject("culling, front face or polygon fill");
    for (uint32_t vertex = 0; !state->vertex_program && !state->fixed_transform && vertex < count; vertex++)
        if (!nv_cpu_finite(vertices[vertex].position[0]) || !nv_cpu_finite(vertices[vertex].position[1]) ||
            !nv_cpu_finite(vertices[vertex].position[2]) || !nv_cpu_finite(vertices[vertex].position[3])) return 0;
    /* Kept at every scale, so a change of scale knows which to leave alone. */
    if (state->depth && (state->depth_enable || state->stencil_enable)) {
        if (depth_only) depth_only_targets.insert(state->depth);
        else depth_only_targets.erase(state->depth);
    }
    TargetExtent extent = reusable_target_extent(*state);
    prepare_target_cache(*state, extent);
    static const Surface no_color_target = {};
    Surface *surface = depth_only ? nullptr : get_surface(*state, true, extent);
    if (!surface && !depth_only) return 0;
    DepthSurface *depth = state->depth_enable || state->stencil_enable ? get_depth(*state, true, extent) : nullptr;
    if ((state->depth_enable || state->stencil_enable) && !depth) return reject("depth/stencil surface");
    if (surface && depth && (surface->texture_width != depth->texture_width || surface->texture_height != depth->texture_height))
        return reject("colour and depth targets at different render scales");
    /* Title pixels to target pixels (RECOMP_RENDER_SCALE). */
    uint32_t guest_width = surface ? surface->width : depth ? depth->width : state->width;
    uint32_t guest_height = surface ? surface->height : depth ? depth->height : state->height;
    uint32_t target_width = surface ? surface->texture_width : depth ? depth->texture_width : state->width;
    uint32_t target_height = surface ? surface->texture_height : depth ? depth->texture_height : state->height;
    D3D11_DEPTH_STENCIL_DESC depth_description = {};
    depth_description.DepthEnable = state->depth_enable != 0;
    depth_description.DepthWriteMask = state->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_description.DepthFunc = state->depth_enable ? (D3D11_COMPARISON_FUNC)(state->depth_function - 0x1FF) : D3D11_COMPARISON_ALWAYS;
    uint32_t depth_key = (depth_description.DepthEnable ? 8u : 0u) +
                        (depth_description.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? 16u : 0u) +
                        (uint32_t)depth_description.DepthFunc - 1u;
    depth_description.StencilEnable = state->stencil_enable != 0;
    depth_description.StencilReadMask = (UINT8)state->stencil_read_mask;
    depth_description.StencilWriteMask = (UINT8)state->stencil_write_mask;
    depth_description.FrontFace.StencilFunc = state->stencil_enable ? (D3D11_COMPARISON_FUNC)(state->stencil_function - 0x1FF) : D3D11_COMPARISON_ALWAYS;
    depth_description.FrontFace.StencilFailOp = state->stencil_enable ? stencil_operation(state->stencil_fail) : D3D11_STENCIL_OP_KEEP;
    depth_description.FrontFace.StencilDepthFailOp = state->stencil_enable ? stencil_operation(state->stencil_depth_fail) : D3D11_STENCIL_OP_KEEP;
    depth_description.FrontFace.StencilPassOp = state->stencil_enable ? stencil_operation(state->stencil_pass) : D3D11_STENCIL_OP_KEEP;
    depth_description.BackFace = depth_description.FrontFace;
    std::array<uint32_t,7> stencil_key = {depth_key, (uint32_t)depth_description.FrontFace.StencilFunc,
        depth_description.StencilReadMask, depth_description.StencilWriteMask,
        (uint32_t)depth_description.FrontFace.StencilFailOp, (uint32_t)depth_description.FrontFace.StencilDepthFailOp,
        (uint32_t)depth_description.FrontFace.StencilPassOp};
    ComPtr<ID3D11DepthStencilState> depth_state;
    if (!state->stencil_enable) depth_state = depth_states[depth_key];
    else for (const auto &entry : stencil_states)
        if (entry.key == stencil_key) { depth_state = entry.state; break; }
    if (!depth_state) {
        if (FAILED(device->CreateDepthStencilState(&depth_description, &depth_state))) return 0;
        if (!state->stencil_enable) depth_states[depth_key] = depth_state;
        else {
            if (stencil_states.size() >= 256) stencil_states.clear();
            stencil_states.push_back({stencil_key, depth_state});
        }
        gpu_timing.depth_created++;
    }
    std::array<ComPtr<ID3D11ShaderResourceView>,4> views;
    std::array<ComPtr<ID3D11SamplerState>,4> samplers;
    uint32_t signed_texture_stages = 0, packed_texture_stages = 0;
    Constants constants = {};
    D3D11_RECT clip = {(LONG)std::min(state->clip_x, state->width), (LONG)std::min(state->clip_y, state->height),
                      (LONG)std::min(state->clip_x + state->clip_width, state->width),
                      (LONG)std::min(state->clip_y + state->clip_height, state->height)};
    if (state->topology == NV2A_GPU_TOPOLOGY_TRIANGLES && state->cull_enable && state->cull_face == 0x408) { clip.right = clip.left; clip.bottom = clip.top; }
    configure_window_clip(*state, constants, clip);
    const D3D11_RECT guest_clip = clip;   /* what the draw may write, in title pixels */
    if (target_width != guest_width || target_height != guest_height) {
        clip.left = scale_edge(clip.left, guest_width, target_width);
        clip.right = scale_edge(clip.right, guest_width, target_width);
        clip.top = scale_edge(clip.top, guest_height, target_height);
        clip.bottom = scale_edge(clip.bottom, guest_height, target_height);
        for (uint32_t rectangle = 0; rectangle < constants.window_clip_control[1] && rectangle < 8; rectangle++) {
            auto &bounds = constants.window_clip_rects[rectangle];
            bounds[0] = (uint32_t)scale_edge((LONG)bounds[0], guest_width, target_width);
            bounds[1] = (uint32_t)scale_edge((LONG)bounds[1], guest_height, target_height);
            bounds[2] = (uint32_t)scale_edge((LONG)bounds[2], guest_width, target_width);
            bounds[3] = (uint32_t)scale_edge((LONG)bounds[3], guest_height, target_height);
        }
    }
    constants.control[0] = state->combiners.control & 255u; constants.control[1] = state->stage_program;
    constants.control[2] = state->alpha_enable; constants.control[3] = state->alpha_function; constants.misc[0] = state->alpha_reference;
    constants.misc[1] = state->control0;
    constants.misc[2] = state->fixed_transform; constants.misc[3] = state->flat_shading;
    constants.final_input[0] = state->combiners.final_input[0]; constants.final_input[1] = state->combiners.final_input[1];
    constants.final_input[2] = state->combiners.control;
    constants.final_input[3] = state->shader_clip_mode;
    constants.texture_control[0] = state->shader_other_stage_input;
    constants.texture_control[1] = state->shader_dot_mapping;
    std::memcpy(constants.shader_eye_vector, state->shader_eye_vector, sizeof state->shader_eye_vector);
    constants.texture_control[2] = state->fog_enable;
    constants.texture_control[3] = state->fog_mode;
    std::memcpy(constants.fog_parameters, state->fog_parameters, sizeof state->fog_parameters);
    constants.fog_control[0] = state->fog_gen_mode;
    std::memcpy(constants.fog_plane, state->fog_plane, sizeof state->fog_plane);
    std::memcpy(constants.factors, state->combiners.factors, sizeof constants.factors);
    std::memcpy(constants.fog, state->combiners.fog_color, sizeof constants.fog);
    if (state->vertex_constants) std::memcpy(constants.vertex_constants, state->vertex_constants, sizeof constants.vertex_constants);
    if (state->fixed_transform) {
        constants.light_state[0] = state->lighting_enable; constants.light_state[1] = state->specular_enable;
        constants.light_state[2] = state->light_enable_mask; constants.light_state[3] = state->color_material;
        constants.light_control[0] = state->light_control;
        constants.light_control[1] = state->normalization_enable;
        constants.light_control[2] = state->skin_mode;
        constants.light_control[3] = state->texgen_view_model;
        std::memcpy(constants.texgen, state->texgen, sizeof constants.texgen);
        std::memcpy(constants.texture_matrix_enable, state->texture_matrix_enable, sizeof constants.texture_matrix_enable);
        std::memcpy(constants.material, state->material_emission, sizeof constants.material);
        constants.material[3] = state->material_alpha;
        std::memcpy(constants.scene_ambient, state->scene_ambient, sizeof constants.scene_ambient);
        constants.specular_power[0] = state->specular_power;
        std::memcpy(constants.light_ambient, state->light_ambient, sizeof constants.light_ambient);
        std::memcpy(constants.light_diffuse, state->light_diffuse, sizeof constants.light_diffuse);
        std::memcpy(constants.light_specular, state->light_specular, sizeof constants.light_specular);
        std::memcpy(constants.light_local_position, state->light_local_position, sizeof constants.light_local_position);
        std::memcpy(constants.light_local_attenuation, state->light_local_attenuation, sizeof constants.light_local_attenuation);
        std::memcpy(constants.light_infinite_direction, state->light_infinite_direction, sizeof constants.light_infinite_direction);
        std::memcpy(constants.light_infinite_half_vector, state->light_infinite_half_vector, sizeof constants.light_infinite_half_vector);
        std::memcpy(constants.light_spot_direction, state->light_spot_direction, sizeof constants.light_spot_direction);
    }
    if (state->fixed_transform && state->vertex_constants &&
            ((state->stage_program & 31u) == 1u)) {
        static int shadow_diag = -1;
        if (shadow_diag < 0) shadow_diag = getenv("RECOMP_SHADOW_DIAG") != NULL;
        if (shadow_diag) {
            static std::set<uint64_t> seen;
            uint64_t key = ((uint64_t)state->stage_program << 24)
                         | ((uint64_t)(state->textures[0].format & 0xFFu) << 16)
                         | ((uint64_t)(state->texgen[0][0] & 0xFFFFu) << 0);
            if (seen.size() < 32 && seen.insert(key).second) {
                const float (*vc)[4] = state->vertex_constants;
                fprintf(stderr, "[SHADOW] stages=%X ctl0=%X blend=%u/%X/%X depth=%u/%X tm0=%u tgen=%X,%X,%X,%X "
                        "tex0=%X addr=%X/%X/%X filt=%X ctl=%X/%u %ux%u w=%u h=%u "
                        "pl64=%.4f,%.4f,%.4f,%.4f pl68=%.4f,%.4f,%.4f,%.4f\n",
                        state->stage_program, state->control0, state->blend_enable,
                        state->blend_source, state->blend_destination, state->depth_enable,
                        state->depth_function, state->texture_matrix_enable[0],
                        state->texgen[0][0], state->texgen[0][1], state->texgen[0][2], state->texgen[0][3],
                        state->textures[0].format, state->textures[0].address_u,
                        state->textures[0].address_v, state->textures[0].address_w,
                        state->textures[0].filter, state->textures[0].control0,
                        state->textures[0].control0_valid, state->textures[0].width, state->textures[0].height,
                        state->width, state->height,
                        vc[64][0], vc[64][1], vc[64][2], vc[64][3],
                        vc[68][0], vc[68][1], vc[68][2], vc[68][3]);
            }
        }
    }
    /* Logical size: vertex positions are in the title's logical pixels, and
     * an anti-aliased surface is larger by the AA factor (see Nv2aGpuDraw). */
    constants.viewport[0] = (float)state->width / (float)(state->aa_scale_x ? state->aa_scale_x : 1u);
    constants.viewport[1] = (float)state->height / (float)(state->aa_scale_y ? state->aa_scale_y : 1u);
    constants.viewport[2] = state->depth_format == 1 ? 65535.0f : 16777215.0f;
    /* Widescreen (see wide_factor): a 3D draw widens its view, a 2D one is
     * squeezed back (viewport.w, in gs_main). */
    int projection = projection_register(*state);
    if (projection >= 0 && surface && screen_shaped(surface->width, surface->height)) screen_3d_draws++;
    float widened = ((float)target_width / (float)guest_width) / ((float)target_height / (float)guest_height);
    bool squeeze_2d = false;
    if (widened > 1.001f) {
        if (projection >= 0)
            for (int component = 0; component < 4; component++) constants.vertex_constants[projection][component] /= widened;
        else if (!nv2a_pb_exec_batch_spans(constants.viewport[0])) {
            constants.viewport[3] = 1.0f / widened;
            squeeze_2d = true;
        }
    }
    constants.depth_range[0] = depth_clip_min; constants.depth_range[1] = depth_clip_max;
    constants.depth_range[2] = (float)depth_clamp;
    if (front_offset || back_offset) {
        constants.depth_offset[0] = state->polygon_offset_scale; constants.depth_offset[1] = state->polygon_offset_bias;
        constants.depth_offset[2] = front_offset ? 1.0f : 0.0f; constants.depth_offset[3] = back_offset ? 1.0f : 0.0f;
    }
    for (uint32_t stage = 0; stage < 8; stage++) {
        constants.stages[stage][0] = state->combiners.rgb_input[stage]; constants.stages[stage][1] = state->combiners.alpha_input[stage];
        constants.stages[stage][2] = state->combiners.rgb_output[stage]; constants.stages[stage][3] = state->combiners.alpha_output[stage];
    }
    phase_timer.next_phase(gpu_timing.draw_textures);
    for (uint32_t stage = 0; stage < 4; stage++) {
        uint32_t mode = (state->stage_program >> (stage * 5)) & 31;
        bool dependent = mode == 15 || mode == 16;
        bool bump = mode == 6 || mode == 7;
        bool diffuse_reflection = mode == 11;
        bool reflection = diffuse_reflection || mode == 12 || mode == 14 || mode == 18;
        bool dot_stage = mode == 9 || mode == 17 || reflection;
        if (mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 5 && !dependent && !bump && !dot_stage) return reject("texture shader mode");
        if (dot_stage) {
            if ((mode == 17 && stage != 1 && stage != 2) || (mode == 9 && stage < 2)) return reject("dot texture stage");
            if (diffuse_reflection) {
                if (stage != 2 || ((state->stage_program >> 5) & 31u) != 17) return reject("diffuse reflection dot predecessor");
                uint32_t next_mode = (state->stage_program >> 15) & 31u;
                if (next_mode != 12 && next_mode != 18) return reject("diffuse reflection following specular stage");
                if (((state->shader_other_stage_input >> 20) & 15u) >= 2) return reject("diffuse reflection look-ahead input");
                if (((state->shader_dot_mapping >> 8) & 15u) > 4) return reject("diffuse reflection look-ahead mapping");
                for (uint32_t vertex = 0; !state->vertex_program && vertex < count; vertex++)
                    for (uint32_t component = 0; component < 3; component++)
                        if (!nv_cpu_finite(vertices[vertex].texture[3][component])) return reject("nonfinite diffuse reflection coordinate");
            } else if (reflection && (stage != 3 || ((state->stage_program >> 5) & 31u) != 17 ||
                (((state->stage_program >> 10) & 31u) != 17 && (mode == 14 || ((state->stage_program >> 10) & 31u) != 11))))
                return reject("reflection dot predecessors");
            if (mode == 18) {
                if (state->shader_eye_vector_valid != 7u) return reject("unprogrammed constant eye vector");
                for (float component : state->shader_eye_vector)
                    if (!nv_cpu_finite(component)) return reject("nonfinite constant eye vector");
            }
            if (((state->shader_dot_mapping >> ((stage - 1) * 4)) & 15u) > 4) return reject("dot texture mapping");
            if (mode == 9) {
                uint32_t previous_mode = (state->stage_program >> ((stage - 1) * 5)) & 31u;
                if (previous_mode != 9 && previous_mode != 17) return reject("dot texture predecessor");
            }
        }
        if (dependent || bump || dot_stage) {
            uint32_t source_stage = stage == 1 ? 0 : (state->shader_other_stage_input >> (stage * 4 + 8)) & 15u;
            if (!stage || source_stage >= stage) return reject("dependent texture input stage");
            if (dot_stage) {
                uint32_t source_mode = (state->stage_program >> (source_stage * 5)) & 31u;
                if (source_mode == 0 || source_mode == 5 || source_mode == 17) return reject("dot texture input has no color result");
                uint32_t mapping = (state->shader_dot_mapping >> ((stage - 1) * 4)) & 15u;
                uint32_t sign_mask = source_mode == 4 ? 0 :
                    (state->textures[source_stage].filter >> 28) & (mapping == 4 ? 15u : 14u);
                if (mapping != 0 && sign_mask) return reject("signed texture input with encoded dot mapping");
                if (diffuse_reflection && source_mode != 4 && ((state->shader_dot_mapping >> 8) & 15u) != 0 &&
                    ((state->textures[0].filter >> 28) & 15u))
                    return reject("signed reflection look-ahead with encoded dot mapping");
                if ((mapping == 4 || (diffuse_reflection && ((state->shader_dot_mapping >> 8) & 15u) == 4)) &&
                    (source_mode == 4 || source_mode == 7)) return reject("HILO requires normalized sampled color input");
                if (reflection && !diffuse_reflection && source_stage != 0) return reject("reflection normal input stage");
                if (diffuse_reflection && ((state->shader_other_stage_input >> 20) & 15u) != 0)
                    return reject("diffuse reflection look-ahead has no color result");
            }
        }
        if (!nv2a_gpu_texture_mode_samples(mode)) continue;
        const auto &binding = state->textures[stage];
        if (binding.disabled || !nv2a_gpu_texture_enabled(&binding)) {
            /* See gpu_raster_batch: a disabled texture sampled by the shader
             * collapses to a default texel. Bind 1x1 white (identity for the
             * combiner) instead of rejecting, which would delete geometry. */
            ID3D11ShaderResourceView *white = default_white_view();
            if (!white) return reject("default white texture");
            views[stage] = white;
            constants.texture_info[stage][0] = 1.0f;
            constants.texture_info[stage][1] = 1.0f;
            constants.texture_info[stage][2] = 0.0f;
            constants.texture_info[stage][3] = 0.0f;
            continue;
        }
        if (!binding.shadow &&
                ((mode == 2) != (binding.depth != 0) || (binding.depth && (binding.cube || binding.linear))))
            return reject("projective volume texture dimensionality");
        uint32_t sign_mask = binding.filter >> 28;
        uint32_t key_mode = binding.control0_valid ? binding.control0 & 3u : 0;
        if (sign_mask && key_mode) return reject("signed-channel texture color-key ordering");
        if (reflection && binding.linear) return reject("reflection texture dimensionality");
        if (bump) {
            if (binding.cube) return reject("bump texture dimensionality");
            for (float component : binding.bump_matrix)
                if (!nv_cpu_finite(component)) return reject("nonfinite bump matrix");
            if (mode == 7 && (!nv_cpu_finite(binding.bump_scale) || !nv_cpu_finite(binding.bump_offset)))
                return reject("nonfinite bump luminance");
            std::memcpy(constants.bump_matrix[stage], binding.bump_matrix, sizeof binding.bump_matrix);
            constants.bump_luminance[stage][0] = binding.bump_scale;
            constants.bump_luminance[stage][1] = binding.bump_offset;
        }
        if (dependent && (binding.cube || binding.linear)) return reject("dependent texture dimensionality");
        if (mode == 9 && (binding.cube || binding.linear)) return reject("dot texture dimensionality");
        for (uint32_t vertex = 0; !dependent && !state->vertex_program && vertex < count; vertex++) {
            for (uint32_t component = 0; component < 4; component++)
                if (state->texgen[stage][component] == 0 &&
                    !nv_cpu_finite(vertices[vertex].texture[stage][component])) {
                    static uint32_t shown;
                    if (shown++ < 16)
                        std::fprintf(stderr, "[TEXCOORD] sanitize stage %u vertex %u comp %u tex=(%.3f %.3f %.3f %.3f) texgen=(%u %u %u %u) matrix_en %u mode %u fmt %08X\n",
                            stage, vertex, component,
                            vertices[vertex].texture[stage][0], vertices[vertex].texture[stage][1],
                            vertices[vertex].texture[stage][2], vertices[vertex].texture[stage][3],
                            state->texgen[stage][0], state->texgen[stage][1], state->texgen[stage][2], state->texgen[stage][3],
                            state->texture_matrix_enable[stage], mode, binding.format);
                    /* Hardware does not reject a NaN/inf texcoord: it samples
                     * texel 0. Dropping the whole batch deletes geometry, so
                     * fold the bad component to 0 and keep drawing. */
                    vertices[vertex].texture[stage][component] = 0.0f;
                }
            if ((mode == 1 || mode == 2) && vertices[vertex].texture[stage][3] == 0) return reject("zero projective texture coordinate");
        }
        if (!binding.source_bytes) return reject("missing texture data");
        views[stage] = get_texture(binding, surface ? *surface : no_color_target);
        uint32_t shadow_func = binding.shadow_func;
        if (!views[stage] && binding.shadow) {
            views[stage] = default_white_view();   /* nothing drawn there yet: lit */
            shadow_func = 7;                       /* ALWAYS */
        }
        if (!views[stage]) return reject("texture upload or resource alias");
        Nv2aGpuTexture smoothed;
        if (bump_source_smoothing(state, stage, binding, smoothed) ? !make_sampler(smoothed, samplers[stage])
                                                                    : !make_sampler(binding, samplers[stage])) {
            static unsigned shown;   /* which state was refused, not just that one was */
            if (shown++ < 8)
                std::fprintf(stderr, "[GPU-D3D11] sampler: stage %u filter %08X address %u/%u/%u control0 %08X valid %u\n",
                             stage, binding.filter, binding.address_u, binding.address_v, binding.address_w,
                             binding.control0, binding.control0_valid);
            return reject("texture filter, addressing or LOD state");
        }
        constants.texture_info[stage][0] = (float)binding.width; constants.texture_info[stage][1] = (float)binding.height; constants.texture_info[stage][2] = (float)binding.linear;
        constants.texture_key[stage][0] = binding.color_key;
        constants.texture_key[stage][1] = binding.format == 0x03 || binding.format == 0x07 ||
            binding.format == 0x1C || binding.format == 0x1E ? 0x00FFFFFFu : UINT32_MAX;
        constants.texture_key[stage][2] = binding.control0_valid ? binding.control0 & 3u : 0;
        constants.texture_key[stage][3] = sign_mask;
        constants.texture_info[stage][3] = (float)((binding.format == 0x1E ? 1u : 0u) | (binding.cube ? 2u : 0u) |
            (binding.address_u == 5 ? 4u : 0u) | (binding.address_v == 5 ? 8u : 0u) |
            (binding.control0_valid && (binding.control0 & 4u) ? 16u : 0u) |
            (binding.address_w == 5 ? 32u : 0u) |
            /* shadow compare: 64, 128 for a 24-bit depth, function in 8..10,
             * 2048 for a linear magnification filter (four-tap PCF) */
            (binding.shadow ? 64u | ((shadow_func & 7u) << 8) |
                (binding.format == 0x2A || binding.format == 0x2B || binding.format == 0x2E ||
                 binding.format == 0x2F ? 128u : 0u) |
                (((binding.filter >> 24) & 15u) == 2u ? 2048u : 0u) : 0u));
        if (sign_mask) signed_texture_stages++;
        switch (binding.format) {
        case 0x01: case 0x16: case 0x17: case 0x1A: case 0x1B: case 0x20: case 0x28: case 0x29:
        case 0x3A: case 0x3B: case 0x3C: packed_texture_stages++; break;
        default: break;
        }
    }
    phase_timer.next_phase(gpu_timing.draw_streams);
    /* A vertex program reads only the sixteen attributes, so only that block
     * of each vertex is uploaded (256 of its ~400 bytes); the fixed-function
     * path reads only the named fields before it. */
    uint32_t packed_attributes[16], packed_count = 0;
    if (state->vertex_program)
        for (uint32_t attribute = 0; attribute < 16; attribute++)
            if (state->attribute_mask & (1u << attribute)) packed_attributes[packed_count++] = attribute;
    UINT vertex_stride = state->vertex_program ? packed_count * 16u : sizeof(Nv2aGpuVertex) - sizeof(vertices[0].attributes);
    auto &vertex_stream = vertex_streams[state->vertex_program ? 1 : 0];
    UINT vertex_bytes = count * vertex_stride;
    UINT index_bytes = state->indices ? state->index_count * sizeof(uint32_t) : 0;
    UINT vertex_offset;
    /* Vertex and index data share one dynamic buffer, appended and unmapped
     * before the draw (see map_stream). */
    if (!map_stream(vertex_stream, vertex_bytes + index_bytes,
                    D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER, "geometry", vertex_offset))
        return 0;
    uint8_t *vertex_destination = (uint8_t *)vertex_stream.mapped.pData + vertex_offset;
    if (state->vertex_program) for (uint32_t vertex = 0; vertex < count; vertex++) {
        uint8_t *packed = vertex_destination + vertex * vertex_stride;
        for (uint32_t a = 0; a < packed_count; a++)
            std::memcpy(packed + a * 16u, vertices[vertex].attributes[packed_attributes[a]], 16);
    }
    else for (uint32_t vertex = 0; vertex < count; vertex++)
        std::memcpy(vertex_destination + vertex * vertex_stride, &vertices[vertex], offsetof(Nv2aGpuVertex, attributes));
    UINT index_offset = 0;
    if (state->indices) {
        index_offset = vertex_offset + vertex_bytes;
        std::memcpy((uint8_t *)vertex_stream.mapped.pData + index_offset, state->indices, index_bytes);
    }
    release_stream(vertex_stream);
    if (state->indices) context->IASetIndexBuffer(vertex_stream.buffer.Get(), DXGI_FORMAT_R32_UINT, index_offset);
    else context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
    UINT constants_offset = 0;
    bool constants_ring = false;
    {
        GpuTimer constants_timer(gpu_timing.constants);
        if (context1 && uploaded_constants_valid && constant_stream.buffer &&
                constant_upload_generation == constant_stream.generation &&
                !std::memcmp(&uploaded_constants, &constants, sizeof constants)) {
            /* Same constants as the last upload this frame: rebind its offset. */
            gpu_timing.constant_reuses++;
            constants_offset = constant_upload_offset;
            constants_ring = true;
        } else if (context1) {
            if (!map_stream(constant_stream, kConstantSlot, D3D11_BIND_CONSTANT_BUFFER, "constants", constants_offset))
                return 0;
            std::memcpy((uint8_t *)constant_stream.mapped.pData + constants_offset, &constants, sizeof constants);
            release_stream(constant_stream);
            constant_upload_generation = constant_stream.generation;
            constant_upload_offset = constants_offset;
            uploaded_constants = constants;
            uploaded_constants_valid = true;
            gpu_timing.constant_uploads++;
            constants_ring = true;
        } else {
            D3D11_MAPPED_SUBRESOURCE constant_map = {};
            if (!target_operation_succeeded("constants", "upload map",
                context->Map(constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &constant_map))) return 0;
            std::memcpy(constant_map.pData, &constants, sizeof constants);
            context->Unmap(constant_buffer.Get(), 0);
            uploaded_constants = constants;
            uploaded_constants_valid = true;
            gpu_timing.constant_uploads++;
        }
    }
    phase_timer.next_phase(gpu_timing.draw_shaders);
    bool uncombined = !(state->combiners.control & 255u) && !state->combiners.final_input[0] && !state->combiners.final_input[1];
    for (uint32_t stage = 0; stage < 4; stage++)
        if (((state->stage_program >> (stage * 5)) & 31u) == 5u ||
            ((uint32_t)constants.texture_info[stage][3] & 16u) ||
            constants.texture_key[stage][2] == 3u) uncombined = false;
    ID3D11PixelShader *program = uncombined ? uncombined_shader.Get() : get_pixel_shader(*state, constants.window_clip_control[0]);
    if (!program) return 0;
    bool vertex_pending = false;
    VertexVariant *guest_vertex = state->vertex_program ? get_vertex_shader(*state, &vertex_pending) : nullptr;
    if (vertex_pending) return 1;   /* its shader is compiling: drawn once it exists */
    if (state->vertex_program && !guest_vertex) return reject("unsupported vertex program");
    if (guest_vertex && guest_vertex->input_mask != state->attribute_mask) {
        static unsigned shown;
        if (shown++ < 8)
            std::fprintf(stderr, "[GPU-D3D11] vertex layout mask %04X != draw's %04X; draw skipped\n",
                         guest_vertex->input_mask, state->attribute_mask);
        return 1;
    }
    phase_timer.next_phase(gpu_timing.draw_submit);
    UINT stride = vertex_stride, offset = vertex_offset;
    ID3D11Buffer *buffers[] = {vertex_stream.buffer.Get()}, *constants_buffer[] = {constant_buffer.Get()};
    ID3D11Buffer *constant_ring_buffer = constant_stream.buffer.Get();
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *depth_target = nullptr;
    bind_draw_targets(surface, depth, target, depth_target);
    ID3D11ShaderResourceView *resources[12] = {}; ID3D11SamplerState *sampler_states[4];
    for (uint32_t stage = 0; stage < 4; stage++) {
        resources[state->textures[stage].depth ? stage + 8 : state->textures[stage].cube ? stage + 4 : stage] = views[stage].Get();
        sampler_states[stage] = samplers[stage].Get();
    }
    float blend_color[4]; nv_cpu_unpack_argb(state->blend_constant, blend_color);
    context->OMSetRenderTargets(1, &target, depth_target);
    context->OMSetDepthStencilState(depth_state.Get(), state->stencil_reference & 255u); context->OMSetBlendState(blending.Get(), blend_color, ~0u);
    context->IASetInputLayout(guest_vertex ? guest_vertex->layout.Get() : input_layout.Get()); context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context->IASetPrimitiveTopology(state->topology == NV2A_GPU_TOPOLOGY_LINES ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST :
                                    state->topology == NV2A_GPU_TOPOLOGY_POINTS ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST :
                                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(guest_vertex ? guest_vertex->shader.Get() : vertex_shader.Get(), nullptr, 0);
    ID3D11GeometryShader *geometry = state->topology == NV2A_GPU_TOPOLOGY_LINES ? line_geometry_shader.Get() :
        state->topology == NV2A_GPU_TOPOLOGY_POINTS ? point_geometry_shader.Get() :
        guest_vertex || state->fixed_transform || state->flat_shading || squeeze_2d ? geometry_shader.Get() : nullptr;
    context->GSSetShader(geometry, nullptr, 0);
    context->PSSetShader(program, nullptr, 0);
    if (constants_ring && context1) {
        UINT first = constants_offset / 16u, num = kConstantCount;
        context1->VSSetConstantBuffers1(0, 1, &constant_ring_buffer, &first, &num);
        context1->PSSetConstantBuffers1(0, 1, &constant_ring_buffer, &first, &num);
        context1->GSSetConstantBuffers1(0, 1, &constant_ring_buffer, &first, &num);
    } else {
        context->VSSetConstantBuffers(0, 1, constants_buffer); context->PSSetConstantBuffers(0, 1, constants_buffer);
        context->GSSetConstantBuffers(0, 1, constants_buffer);
    }
    context->PSSetShaderResources(0, 12, resources); context->PSSetSamplers(0, 4, sampler_states);
    D3D11_VIEWPORT viewport = {0, 0, (float)scale_edge((LONG)state->width, guest_width, target_width),
                               (float)scale_edge((LONG)state->height, guest_height, target_height), 0, 1};
    D3D11_BOX region = {};
    region.left = guest_clip.left > 0 ? (UINT)guest_clip.left : 0;
    region.top = guest_clip.top > 0 ? (UINT)guest_clip.top : 0;
    region.right = guest_clip.right > 0 ? (UINT)guest_clip.right : 0;
    region.bottom = guest_clip.bottom > 0 ? (UINT)guest_clip.bottom : 0;
    if (region.right > state->width) region.right = state->width;
    if (region.bottom > state->height) region.bottom = state->height;
    region.back = 1;
    bool writes = region.left < region.right && region.top < region.bottom;
    context->RSSetState(raster_state.Get()); context->RSSetViewports(1, &viewport); context->RSSetScissorRects(1, &clip);
    GpuDrawSample *sample = begin_gpu_sample(*state);
    if (state->indices) context->DrawIndexed(state->index_count, 0, 0);
    else context->Draw(count, 0);
    end_gpu_sample(sample);
    phase_timer.next_phase(gpu_timing.draw_bookkeeping);
    if (state->topology == NV2A_GPU_TOPOLOGY_LINES) gpu_timing.line_primitives += element_count / 2;
    if (state->topology == NV2A_GPU_TOPOLOGY_POINTS) gpu_timing.point_primitives += element_count;
    gpu_timing.signed_texture_stages += signed_texture_stages;
    gpu_timing.packed_texture_stages += packed_texture_stages;
    if (state->window_clip_valid) {
        gpu_timing.window_clip_draws++;
        if (constants.window_clip_control[0]) gpu_timing.window_shader_draws++;
        else gpu_timing.window_scissor_draws++;
        if (state->window_clip_type) gpu_timing.window_exclusive_draws++;
    }
    if (state->fixed_transform) {
        gpu_timing.fixed_draws++;
        if (state->lighting_enable) gpu_timing.lit_draws++;
        if (state->skin_mode) gpu_timing.skinned_draws++;
        bool texgen_used = false, texture_matrix_used = false;
        for (uint32_t stage = 0; stage < 4; stage++) {
            texture_matrix_used |= state->texture_matrix_enable[stage] != 0;
            for (uint32_t component = 0; component < 4; component++)
                texgen_used |= state->texgen[stage][component] != 0;
        }
        if (texgen_used) gpu_timing.texgen_draws++;
        if (texture_matrix_used) gpu_timing.texture_matrix_draws++;
    }
    pending_draws = true;
    if (surface && (state->color_mask & 0x01010101u)) { if (writes) mark_written_region(*surface, region); }
    else gpu_timing.color_masked_draws++;
    bool stencil_writes = state->stencil_enable && (state->stencil_write_mask & 255u) &&
        (state->stencil_fail != 0x1E00 || state->stencil_depth_fail != 0x1E00 || state->stencil_pass != 0x1E00);
    if (depth && ((state->depth_enable && state->depth_mask) || stencil_writes) && writes) mark_written_region(*depth, region);
    ID3D11ShaderResourceView *empty[12] = {};
    context->PSSetShaderResources(0, 12, empty);
    return 1;
}

extern "C" int nv2a_gpu_compile(void)
{
    if (vertex_shader && uncombined_shader && geometry_shader && line_geometry_shader && point_geometry_shader) return 1;
    if (!nv2a_gpu_available()) return 0;
    ComPtr<ID3DBlob> pixel_code, errors;
    HRESULT result = compile_shader_cached(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr,
                               "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, vertex_code, errors);
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] shader compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
        return 0;
    }
    result = device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &vertex_shader);
    if (FAILED(result)) {
        std::fprintf(stderr, "[GPU-D3D11] base vertex shader creation failed: 0x%08X; device 0x%08X\n",
                     (unsigned)result, (unsigned)device->GetDeviceRemovedReason());
        return 0;
    }
    pixel_code.Reset(); errors.Reset();
    result = compile_shader_cached(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr,
                       "ps_uncombined", "ps_5_0", D3DCOMPILE_IEEE_STRICTNESS, pixel_code, errors);
    if (FAILED(result)) return 0;
    if (FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &uncombined_shader))) return 0;
    const struct { const char *entry; ComPtr<ID3D11GeometryShader> *shader; } geometries[] = {
        {"gs_main", &geometry_shader}, {"gs_lines_main", &line_geometry_shader}, {"gs_points_main", &point_geometry_shader}
    };
    for (const auto &geometry : geometries) {
        pixel_code.Reset(); errors.Reset();
        result = compile_shader_cached(nv2a_gpu_shader_source, sizeof nv2a_gpu_shader_source - 1, "NV2A", nullptr,
                           geometry.entry, "gs_5_0", D3DCOMPILE_IEEE_STRICTNESS, pixel_code, errors);
        if (SUCCEEDED(result))
            result = device->CreateGeometryShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr,
                                                   geometry.shader->ReleaseAndGetAddressOf());
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] geometry %s failed (0x%08lX): %s\n", geometry.entry,
                         (unsigned long)result, errors ? (const char *)errors->GetBufferPointer() : "shader creation failed");
            return 0;
        }
    }
    return 1;
}

extern "C" int nv2a_gpu_available(void)
{
    static bool attempted;
    if (!attempted) {
        attempted = true;
        D3D_FEATURE_LEVEL level;
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &device, &level, &context);
        if (FAILED(result)) {
            std::fprintf(stderr, "[GPU-D3D11] device unavailable: 0x%08X\n", (unsigned)result);
            return 0;
        }
        std::fprintf(stderr, "[GPU-D3D11] device ready: feature level 0x%X, hardware\n", (unsigned)level);
        if (SUCCEEDED(context.As(&context1))) {
            D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
            if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options)) ||
                !options.ConstantBufferOffsetting)
                context1.Reset();
            else
                constant_no_overwrite = options.MapNoOverwriteOnDynamicConstantBuffer != FALSE;
        }
        std::fprintf(stderr, "[GPU-D3D11] constant buffer offsetting: %s\n",
                     context1 ? "supported" : "unavailable, using per-draw Map");
        g_xbox_path_hook = nv2a_cloak_path_hook;
    }
    return device != nullptr;
}