/* Drive the real executor through NV097 methods. AA targets keep their
 * physical layout for later texture reads, even when shaded on the CPU. */
#include "kernel/xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
extern void nv2a_pb_exec_method(uint32_t sub, uint32_t method, uint32_t value);
extern ptrdiff_t g_xbox_mem_offset;
/* This harness supplies plain guest RAM, without reserving the full 4 GB
 * device aperture or starting the runtime's interrupt threads. */
ptrdiff_t __wrap_xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }

static int failures;
static void check(int ok, const char *name)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
static void method(uint32_t address, uint32_t value)
{
    nv2a_pb_exec_method(0, address, value);
}

int main(void)
{
    const uint32_t color_va = 0x10000, depth_va = 0x20000, vertex_va = 0x30000;
    uint8_t *mem = calloc(1, 16 * 1024 * 1024);
    if (!mem) return 1;
    g_xbox_mem_offset = (ptrdiff_t)mem;
    uint32_t *color = (uint32_t *)(mem + color_va);
    uint32_t *depth = (uint32_t *)(mem + depth_va);
    /* Oversized triangle covers the complete logical 8x8 viewport. */
    const float positions[3][4] = {{-1, -1, 100, 1}, {20, -1, 100, 1}, {-1, 20, 100, 1}};
    memcpy(mem + vertex_va, positions, sizeof positions);
    method(0x0200, 8u << 16); method(0x0204, 8u << 16);
    method(0x0210, color_va); method(0x0214, depth_va);
    method(0x1720, vertex_va); method(0x1760, (16u << 8) | 0x42);
    method(0x030C, 1);       /* depth enable */
    method(0x0354, 0x201);   /* LESS */
    method(0x035C, 1);       /* depth writes */
    method(0x1D90, 0xFF102030);
    method(0x1D8C, 0xFFFFFFA6);
    for (uint32_t aa = 0; aa <= 2; aa++) {
        uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;
        uint32_t width = 8 * sx, height = 8 * sy;
        /* Pad each row to expose writes that ignore the physical stride. */
        uint32_t stride = width + 4, pitch = stride * 4;
        for (uint32_t i = 0; i < 512; i++) color[i] = depth[i] = 0x12345678;
        method(0x0208, 0x28 | (aa << 12));
        method(0x020C, pitch | (pitch << 16));
        method(0x1D94, 0xF3);
        int cleared = 1, padding = 1;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++)
                cleared &= color[y * stride + x] == 0xFF102030 && depth[y * stride + x] == 0xFFFFFFA6;
            for (uint32_t x = width; x < stride; x++)
                padding &= color[y * stride + x] == 0x12345678 && depth[y * stride + x] == 0x12345678;
        }
        check(cleared, "clear fills every physical AA sample");
        check(padding && color[height * stride] == 0x12345678, "clear respects row padding and height");
        method(0x17FC, 5); method(0x1810, 2u << 24); method(0x17FC, 0);
        int drawn = 1;
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
                drawn &= color[y * stride + x] == 0xFFFFFFFF && depth[y * stride + x] == 0x000064A6;
        check(drawn, "draw fills AA color and depth samples, preserving stencil");
        check(color[width] == 0x12345678 && depth[width] == 0x12345678,
              "draw respects row padding");
    }
    free(mem);
    return failures ? 1 : 0;
}
