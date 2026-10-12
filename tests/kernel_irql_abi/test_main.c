#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
static uint8_t *memory;
static HANDLE worker_raised, main_lowered;

static DWORD WINAPI concurrent_irql_worker(LPVOID parameter)
{
    (void)parameter;
    g_fs_base = 0x30000;
    unsigned old = xbox_KfRaiseIrql(3);
    int failed = old != PASSIVE_LEVEL || memory[g_fs_base + 0x24] != 3;
    if (failed)
        fprintf(stderr, "FAIL worker inherited another thread's IRQL: old=%u\n", old);
    SetEvent(worker_raised);
    if (WaitForSingleObject(main_lowered, 5000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL waiting for main IRQL lowering\n");
        failed = 1;
    }
    old = xbox_KfRaiseIrql(3);
    if (old != 3) {
        fprintf(stderr, "FAIL main lowering changed worker IRQL: old=%u\n", old);
        failed = 1;
    }
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    return (DWORD)failed;
}

static DWORD WINAPI exiting_irql_worker(LPVOID parameter)
{
    (void)parameter;
    g_fs_base = 0x32000;
    xbox_KfRaiseIrql(DISPATCH_LEVEL);
    xbox_IrqlThreadExit();
    return memory[g_fs_base + 0x24] != PASSIVE_LEVEL;
}

static int check_concurrent_irql(void)
{
    worker_raised = CreateEventW(NULL, TRUE, FALSE, NULL);
    main_lowered = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!worker_raised || !main_lowered) {
        fprintf(stderr, "FAIL creating IRQL test events: %lu\n", GetLastError());
        if (worker_raised) CloseHandle(worker_raised);
        if (main_lowered) CloseHandle(main_lowered);
        return 1;
    }
    g_fs_base = 0x31000;
    xbox_KfRaiseIrql(DISPATCH_LEVEL);
    HANDLE worker = CreateThread(NULL, 0, concurrent_irql_worker, NULL, 0, NULL);
    if (!worker) {
        fprintf(stderr, "FAIL creating IRQL test worker: %lu\n", GetLastError());
        xbox_KfLowerIrql(PASSIVE_LEVEL);
        CloseHandle(worker_raised);
        CloseHandle(main_lowered);
        return 1;
    }
    int failed = 0;
    if (WaitForSingleObject(worker_raised, 5000) != WAIT_OBJECT_0) {
        fprintf(stderr, "FAIL waiting for worker IRQL raise\n");
        failed = 1;
    }
    if (xbox_KfRaiseIrql(DISPATCH_LEVEL) != DISPATCH_LEVEL) {
        fprintf(stderr, "FAIL worker raising changed main IRQL\n");
        failed = 1;
    }
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    if (!xbox_IrqlBlocksInterrupts() || xbox_IrqlRaisedCount() != 3 ||
            memory[g_fs_base + 0x24] != PASSIVE_LEVEL) {
        fprintf(stderr, "FAIL lowering one thread released another thread's interrupt gate\n");
        failed = 1;
    }
    SetEvent(main_lowered);
    DWORD result = 1;
    if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(worker, &result)) {
        fprintf(stderr, "FAIL joining IRQL test worker: %lu\n", GetLastError());
        failed = 1;
    }
    failed |= (int)result;
    if (xbox_IrqlBlocksInterrupts() || xbox_IrqlRaisedCount() != 0) {
        fprintf(stderr, "FAIL interrupt gate leaked after both threads lowered\n");
        failed = 1;
    }
    int saved = xbox_IrqlEnterInterrupt(16);
    if (!xbox_IrqlBlocksInterrupts() || memory[g_fs_base + 0x24] != 16)
        failed = 1;
    int nested = xbox_IrqlEnterInterrupt(18);
    xbox_IrqlLeaveInterrupt(nested);
    if (memory[g_fs_base + 0x24] != 16) failed = 1;
    xbox_IrqlLeaveInterrupt(saved);
    if (xbox_IrqlBlocksInterrupts() || memory[g_fs_base + 0x24] != PASSIVE_LEVEL)
        failed = 1;
    xbox_KfRaiseIrql(DISPATCH_LEVEL);
    saved = xbox_IrqlEnterInterrupt(16);
    xbox_IrqlLeaveInterrupt(saved);
    if (!xbox_IrqlBlocksInterrupts() ||
            memory[g_fs_base + 0x24] != DISPATCH_LEVEL ||
            xbox_KfRaiseIrql(DISPATCH_LEVEL) != DISPATCH_LEVEL)
        failed = 1;
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    CloseHandle(worker);
    worker = CreateThread(NULL, 0, exiting_irql_worker, NULL, 0, NULL);
    if (!worker) {
        fprintf(stderr, "FAIL creating exiting IRQL worker: %lu\n", GetLastError());
        failed = 1;
    } else {
        if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0 ||
                !GetExitCodeThread(worker, &result) || result ||
                xbox_IrqlBlocksInterrupts()) {
            fprintf(stderr, "FAIL exiting thread leaked its interrupt gate\n");
            failed = 1;
        }
        CloseHandle(worker);
    }
    CloseHandle(worker_raised);
    CloseHandle(main_lowered);
    return failed;
}
static int call_irql(unsigned ordinal, uint32_t ecx, unsigned expected_old, unsigned expected_level) {
    uint32_t *stack=(uint32_t *)(memory+0x20000);
    stack[0]=0x12345678; stack[1]=0xA5A5A507;
    g_ecx=ecx; g_esp=0x20000;
    uint32_t token=*(uint32_t *)(memory+0x10000+(ordinal==160?0:4));
    guest_fn fn=recomp_lookup_kernel(token);
    if(!fn) return 1;
    fn();
    /* The host raise returns the prior level; raising to the expected level is a no-op after a correct bridge call. */
    unsigned level=xbox_KfRaiseIrql((KIRQL)expected_level);
    if(g_esp!=0x20004 || stack[1]!=0xA5A5A507 || level!=expected_level ||
       (ordinal==160 && g_eax!=expected_old)) {
        fprintf(stderr,"FAIL ordinal=%u ECX=%08X IRQL=%u expected=%u old=%u expected_old=%u ESP=%08X\n",
                ordinal,ecx,level,expected_level,g_eax,expected_old,g_esp);
        return 1;
    }
    return 0;
}
int main(void) {
    memory=VirtualAlloc(NULL,16*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory) return 10;
    g_xbox_mem_offset=(ptrdiff_t)memory;
    uint32_t *imports=(uint32_t *)(memory+0x10000);
    imports[0]=0x800000A0; imports[1]=0x800000A1;
    xbox_kernel_set_thunk_address(0x10000,2); xbox_kernel_bridge_init();
    int failed=0;
    failed|=call_irql(160,0xBEEF0002,0,2);
    failed|=call_irql(160,0x12340003,2,3);
    failed|=call_irql(161,0xFACE0002,0,2);
    failed|=call_irql(161,0xABCD0000,0,0);
    failed|=check_concurrent_irql();
    VirtualFree(memory,0,MEM_RELEASE);
    if(!failed) puts("PASS IRQL: fastcall ABI, thread-local levels, aggregate gating, nested ISR and thread exit");
    return failed;
}
