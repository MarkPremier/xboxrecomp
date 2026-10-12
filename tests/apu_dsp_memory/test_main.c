#include "apu_state.h"
#include "dsp/dsp.h"
#include "kernel/xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t address) { (void)address; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; abort(); }

static int failures;
static uint32_t mapper_memory[4];
static uint64_t mapped_address;
static uint32_t mapped_bytes;

static uint8_t *test_mapper(uint64_t physical, uint32_t bytes)
{
    mapped_address = physical;
    mapped_bytes = bytes;
    return (uint8_t *)mapper_memory;
}

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void test_window(MCPXAPUState *d, int gp, uint32_t base,
                        uint32_t words, char space, uint32_t dsp_base)
{
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    uint32_t i;
    for (i = 0; i < words; i++) {
        uint32_t address = base + i * 4;
        uint32_t value = 0x100000u + i;
        uint64_t readback;
        if (gp) {
            mcpx_apu_gp_write(d, address, value, 4);
            readback = mcpx_apu_gp_read(d, address, 4);
        } else {
            mcpx_apu_ep_write(d, address, value, 4);
            readback = mcpx_apu_ep_read(d, address, 4);
        }
        check("MMIO writes the correct DSP space and word",
              dsp_read_memory(dsp, space, dsp_base + i) == value);
        check("MMIO reads the written DSP word", readback == value);
        dsp_write_memory(dsp, space, dsp_base + i, value + 1);
        readback = gp ? mcpx_apu_gp_read(d, address, 4)
                      : mcpx_apu_ep_read(d, address, 4);
        check("MMIO reads DSP memory rather than a register shadow",
              readback == value + 1);
    }
}

static void test_register(MCPXAPUState *d, int gp, uint32_t address)
{
    if (gp) {
        mcpx_apu_gp_write(d, address, 0x123456, 4);
        check("GP register fallback writes the register", d->gp.regs[address] == 0x123456);
        check("GP register fallback reads the register",
              mcpx_apu_gp_read(d, address, 4) == 0x123456);
    } else {
        mcpx_apu_ep_write(d, address, 0x123456, 4);
        check("EP register fallback writes the register", d->ep.regs[address] == 0x123456);
        check("EP register fallback reads the register",
              mcpx_apu_ep_read(d, address, 4) == 0x123456);
    }
}

static DWORD WINAPI try_audio_guard(LPVOID parameter)
{
    int entered;
    (void)parameter;
    entered = xbox_GuestAudioGuardTryEnter();
    if (entered)
        xbox_GuestAudioGuardLeave();
    return (DWORD)entered;
}

static void check_audio_worker(DWORD expected)
{
    DWORD result = 2;
    HANDLE thread = CreateThread(NULL, 0, try_audio_guard, NULL, 0, NULL);
    if (!thread) {
        fprintf(stderr, "Cannot create audio guard test thread: %lu\n", GetLastError());
        failures++;
        return;
    }
    check("audio guard worker finishes", WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    check("audio guard worker exit code is readable", GetExitCodeThread(thread, &result));
    check("audio guard excludes other threads only while held", result == expected);
    CloseHandle(thread);
}

static void test_audio_guard(void)
{
    xbox_GuestAudioGuardEnter();
    check("audio guard supports recursive acquisition", xbox_GuestAudioGuardTryEnter());
    check_audio_worker(0);
    xbox_GuestAudioGuardLeave();
    check_audio_worker(0);
    xbox_GuestAudioGuardLeave();
    check_audio_worker(1);
}

static void test_mapped_memory(MCPXAPUState *d)
{
    g_state = d;
    d->physical_mapper = test_mapper;
    mapper_memory[0] = 0x12345678;
    check("mapped physical read reaches supplied bank",
          ldl_le_phys(NULL, 0x80123456) == 0x12345678);
    check("mapped read preserves the physical address and extent",
          mapped_address == 0x80123456 && mapped_bytes == 4);
    stw_le_phys(NULL, 0x654321, 0xABCD);
    check("mapped physical write reaches supplied bank",
          *(uint16_t *)mapper_memory == 0xABCD);
    check("mapped write preserves the physical address and extent",
          mapped_address == 0x654321 && mapped_bytes == 2);
    check("mapped bulk transfer uses the supplied bank",
          mcpx_apu_ram_address(0x112233, sizeof mapper_memory) == (uint8_t *)mapper_memory);
    check("mapped bulk transfer resolves its complete extent",
          mapped_address == 0x112233 && mapped_bytes == sizeof mapper_memory);
    d->physical_mapper = NULL;
    g_state = NULL;
}

int main(void)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    if (!d) {
        fprintf(stderr, "Cannot allocate APU test state\n");
        return EXIT_FAILURE;
    }
    check("RECOMP_APU_DIAG=0 disables diagnostics", !mcpx_apu_diagnostics_enabled());
    check("shared APU bounds cover the complete DSP trap span",
          XBOX_MCPX_APU_MMIO_END - XBOX_MCPX_BASE == 0x80000u);
    check("GP reset lies inside shared APU bounds",
          XBOX_MCPX_BASE + 0x30000u + NV_PAPU_GPRST < XBOX_MCPX_APU_MMIO_END);
    check("EP reset lies inside shared APU bounds",
          XBOX_MCPX_BASE + 0x50000u + NV_PAPU_EPRST < XBOX_MCPX_APU_MMIO_END);
    d->gp.dsp = dsp_init(NULL, NULL, NULL);
    d->ep.dsp = dsp_init(NULL, NULL, NULL);
    test_window(d, 1, NV_PAPU_GPXMEM, 0x1000, 'X', 0);
    test_window(d, 1, NV_PAPU_GPMIXBUF, 0x400, 'X', GP_DSP_MIXBUF_BASE);
    test_window(d, 1, NV_PAPU_GPYMEM, 0x800, 'Y', 0);
    test_window(d, 1, NV_PAPU_GPPMEM, 0x1000, 'P', 0);
    test_window(d, 0, NV_PAPU_EPXMEM, 0xC00, 'X', 0);
    test_window(d, 0, NV_PAPU_EPYMEM, 0x100, 'Y', 0);
    test_window(d, 0, NV_PAPU_EPPMEM, 0x1000, 'P', 0);
    test_register(d, 1, NV_PAPU_GPXMEM + 0x1000 * 4);
    test_register(d, 1, NV_PAPU_GPYMEM + 0x800 * 4);
    test_register(d, 1, NV_PAPU_GPPMEM + 0x1000 * 4);
    test_register(d, 0, NV_PAPU_EPXMEM + 0xC00 * 4);
    test_register(d, 0, NV_PAPU_EPYMEM + 0x100 * 4);
    test_register(d, 0, NV_PAPU_EPPMEM + 0x1000 * 4);
    d->ep_frame_div = 7;
    mcpx_apu_gp_write(d, NV_PAPU_GPRST, 0, 4);
    mcpx_apu_ep_write(d, NV_PAPU_EPRST, 0, 4);
    check("GP reset register is preserved", mcpx_apu_gp_read(d, NV_PAPU_GPRST, 4) == 0);
    check("EP reset register is preserved", mcpx_apu_ep_read(d, NV_PAPU_EPRST, 4) == 0);
    check("EP reset clears the frame divider", d->ep_frame_div == 0);
    test_mapped_memory(d);
    test_audio_guard();
    dsp_destroy(d->gp.dsp);
    dsp_destroy(d->ep.dsp);
    free(d);
    printf("apu_dsp_memory: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
