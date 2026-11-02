/* XAPI GetCurrentThreadId reads fs:[0x28]->UniqueThread at +0x12C.
 * Check the real loader and worker TIB allocator without disc data. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*guest_fn)(void);
guest_fn recomp_lookup(uint32_t va) { (void)va; return NULL; }
guest_fn recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
extern RECOMP_TLS uint32_t g_fs_base;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t *G(uint32_t va) { return (uint32_t *)((uintptr_t)xbox_GetMemoryOffset()+va); }
static HANDLE entered, release, finish;
static volatile LONG attempted, owner, depth;
static LONG count = -1;
static uint32_t main_record, ids[3], records[3];
static DWORD WINAPI worker(void *opaque)
{
    unsigned i = (unsigned)(uintptr_t)opaque;
    uint32_t tib = xbox_AllocThreadTib();
    CHECK(tib != 0);
    g_fs_base = tib;
    records[i] = *G(tib+0x28);
    ids[i] = *G(records[i]+0x12C);
    CHECK(ids[i] == GetCurrentThreadId() && ids[i] != 0);
    CHECK(records[i] != main_record);
    CHECK(*G(records[i]+0x28) == *G(main_record+0x28)); /* Legacy shared RW data. */
    CHECK(*G(tib) == UINT32_MAX);
    if (i == 0) {
        /* The game's recursive lock must see another thread as a contender,
         * rather than recursively entering the main thread's critical section. */
        LONG n = InterlockedIncrement(&count);
        InterlockedExchange(&attempted,1);
        if (n != 0 && owner != (LONG)ids[i])
            CHECK(WaitForSingleObject(release,5000) == WAIT_OBJECT_0);
        else if (owner == (LONG)ids[i])
            InterlockedIncrement(&depth);
        InterlockedExchange(&owner,(LONG)ids[i]);
        SetEvent(entered);
    }
    /* Keep all IDs live until the parent checks them; hosts may recycle the
     * ID of a worker that has already exited. */
    CHECK(WaitForSingleObject(finish,5000) == WAIT_OBJECT_0);
    return 0;
}
int main(void)
{
    uint8_t xbe[0x2000] = {0};
    uint32_t *h = (uint32_t *)xbe;
    memcpy(xbe,"XBEH",4);
    h[0x104/4]=0x10000; h[0x108/4]=0x1000;
    h[0x11C/4]=1; h[0x120/4]=0x10200; h[0x12C/4]=0x10300;
    uint32_t *s=(uint32_t *)(xbe+0x200);
    s[0]=6; s[1]=0x11000; s[2]=0x1000; s[3]=0x1000; s[4]=0x1000; s[5]=0x10240;
    memcpy(xbe+0x240,".text",6);
    uint32_t *tls=(uint32_t *)(xbe+0x300);
    tls[0]=0x10320; tls[1]=0x10328; tls[2]=0x10330; tls[4]=16;
    CHECK(xbox_MemoryLayoutInit(xbe,sizeof xbe));
    main_record=*G(g_fs_base+0x28);
    uint32_t main_id=*G(main_record+0x12C);
    CHECK(main_id == GetCurrentThreadId() && main_id != 0);
    entered=CreateEvent(NULL,TRUE,FALSE,NULL);
    release=CreateEvent(NULL,FALSE,FALSE,NULL);
    finish=CreateEvent(NULL,TRUE,FALSE,NULL);
    CHECK(entered && release && finish);
    count=0; owner=(LONG)main_id; depth=1; /* Main owns the synthetic engine lock. */
    HANDLE threads[3];
    for (unsigned i=0;i<3;++i) { threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)i,0,NULL); CHECK(threads[i]); }
    for (unsigned n=0;!attempted && n<500;++n) Sleep(1);
    CHECK(attempted && WaitForSingleObject(entered,50) == WAIT_TIMEOUT);
    CHECK(owner == (LONG)main_id && depth == 1);
    InterlockedExchange(&owner,0); InterlockedDecrement(&count); SetEvent(release);
    CHECK(WaitForSingleObject(entered,5000) == WAIT_OBJECT_0);
    SetEvent(finish);
    CHECK(WaitForMultipleObjects(3,threads,TRUE,5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(entered,0) == WAIT_OBJECT_0);
    for (unsigned i=0;i<3;++i) {
        CHECK(ids[i] != main_id);
        for (unsigned j=0;j<i;++j) CHECK(ids[i] != ids[j] && records[i] != records[j]);
        CloseHandle(threads[i]);
    }
    CHECK(*G(main_record+0x12C) == main_id);
    CloseHandle(entered); CloseHandle(release); CloseHandle(finish);
    xbox_MemoryLayoutShutdown();
    puts("PASS: distinct guest thread IDs preserve engine recursive-lock exclusion");
    return 0;
}
