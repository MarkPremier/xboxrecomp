/* Real NV097 method dispatch, framebuffer bytes and GPU readback. No disc data. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#ifdef _WIN32
#include "nv2a_gpu.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*guest_fn)(void);
guest_fn recomp_lookup(uint32_t va) { (void)va; return NULL; }
guest_fn recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
extern void nv2a_pb_exec_method(uint32_t subch,uint32_t method,uint32_t param);
extern void nv2a_pb_exec_flush(void);
extern int nv2a_pb_exec_poll_reports(void);
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void env(const char *key,const char *value) {
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
static void method(uint32_t m,uint32_t p) { nv2a_pb_exec_method(0,m,p); }
static void rect(unsigned l,unsigned t,unsigned r,unsigned b) {
    method(0x1D98,l|(r<<16)); method(0x1D9C,t|(b<<16));
}
static void pixels(uint8_t *p,unsigned bytes,uint32_t value,unsigned l,unsigned t,unsigned r,unsigned b) {
    for(unsigned y=0;y<6;++y) for(unsigned x=0;x<64/bytes;++x) {
        uint32_t expected=x>=l && x<=r && y>=t && y<=b ? value : bytes==2?0x5A5Au:0x5A5A5A5Au;
        uint32_t actual=bytes==2?((uint16_t *)(p+y*64))[x]:((uint32_t *)(p+y*64))[x];
        if(actual!=expected) {fprintf(stderr,"pixel %u,%u got %08X want %08X\n",x,y,actual,expected);exit(1);}
    }
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    env("RECOMP_NV2A_NATIVE_CLEARS",strcmp(argv[1],"cpu")?"1":"0");
    env("RECOMP_CPU_RGB565","0"); env("RECOMP_RENDER_SCALE","1"); env("RECOMP_FB_WINDOW","0");
    uint8_t xbe[0x2000]={0}; uint32_t *h=(uint32_t *)xbe;
    memcpy(xbe,"XBEH",4);h[0x104/4]=0x10000;h[0x108/4]=0x1000;h[0x11C/4]=1;h[0x120/4]=0x10200;
    uint32_t *s=(uint32_t *)(xbe+0x200);
    s[0]=6;s[1]=0x11000;s[2]=0x1000;s[3]=0x1000;s[4]=0x1000;s[5]=0x10240;
    memcpy(xbe+0x240,".text",6);
    CHECK(xbox_MemoryLayoutInit(xbe,sizeof xbe));
    uint32_t color=xbox_ContiguousAlloc(0x10000,4096),depth=xbox_ContiguousAlloc(0x10000,4096);
    CHECK(color && depth);
    uint8_t *p=(uint8_t *)((uintptr_t)xbox_GetMemoryOffset()+color);
    uint8_t *z=(uint8_t *)((uintptr_t)xbox_GetMemoryOffset()+depth);
    method(0x200,8u<<16);method(0x204,6u<<16);method(0x20C,64u|(64u<<16));
    method(0x210,color&0x3FFFFFFu);method(0x214,depth&0x3FFFFFFu);
    for(unsigned bytes=2;bytes<=4;bytes+=2) {
        method(0x208,(bytes==2?3u:8u)|(1u<<4));
        memset(p,0x5A,0x10000);
        rect(2,1,4,3);method(0x1D90,bytes==2?0xCCC0u:0xFF3489ABu);method(0x1D94,0xF0);
        nv2a_pb_exec_flush();pixels(p,bytes,bytes==2?0xCCC0u:0xFF3489ABu,2,1,4,3);
        /* A rectangle wholly outside the target must preserve every byte. */
        rect(20,20,21,21);method(0x1D90,0);method(0x1D94,0xF0);nv2a_pb_exec_flush();
        pixels(p,bytes,bytes==2?0xCCC0u:0xFF3489ABu,2,1,4,3);
        /* A later full clear replaces the old rectangle; pitch padding survives. */
        rect(0,0,7,5);method(0x1D90,bytes==2?0xFFE0u:0xFF123456u);method(0x1D94,0xF0);
        nv2a_pb_exec_flush();pixels(p,bytes,bytes==2?0xFFE0u:0xFF123456u,0,0,7,5);
    }
    memset(z,0x5A,0x10000);rect(1,2,3,4);method(0x1D8C,0xFFFF);method(0x1D94,1);
    nv2a_pb_exec_flush();pixels(z,2,0xFFFF,1,2,3,4);
    method(0x208,8u|(2u<<4));memset(z,0x5A,0x10000);
    method(0x1D8C,0xFFFFFF13u);method(0x1D94,1);nv2a_pb_exec_flush();
    pixels(z,4,0xFFFFFF5Au,1,2,3,4); /* Preserve stencil during a depth clear. */
    method(0x1D94,2);nv2a_pb_exec_flush();pixels(z,4,0xFFFFFF13u,1,2,3,4);
    /* Real report DMA/handle dispatch must not invent visible pixels. */
    volatile uint32_t *ramht=xbox_Nv2aRegisterPointer(0x700000,8);
    volatile uint32_t *descriptor=xbox_Nv2aRegisterPointer(0x700200,12);
    CHECK(ramht && descriptor);
    ramht[0]=12;ramht[1]=0x80000020;
    descriptor[0]=0x3D;descriptor[1]=0xFFFF;descriptor[2]=color&0x3FFFFFFu;
    uint32_t *report=(uint32_t *)(p+0x8000);
    for(unsigned i=0;i<5;i++)report[i]=0xFFFFFFFF;
    method(0x01A8,12);method(0x17C8,1);method(0x17CC,0);
    method(0x17D0,0x02008000);CHECK(report[3]==0xFFFFFFFF);
    method(0x17D0,0x01008000);
    CHECK(report[2]==0 && report[3]==0 && report[4]==0xFFFFFFFF);
    CHECK(report[0]!=0xFFFFFFFF || report[1]!=0xFFFFFFFF);
#ifdef _WIN32
    if(strcmp(argv[1],"cpu")) {
        Nv2aGpuDraw draw={0};Nv2aGpuVertex vertices[6]={0};
        const float xy[6][2]={{0,0},{8,0},{0,6},{8,0},{8,6},{0,6}};
        draw.color=p;draw.width=draw.clip_width=8;draw.height=draw.clip_height=6;
        draw.pitch=64;draw.bytes_per_pixel=4;draw.polygon_front=draw.polygon_back=0x1B02;
        for(unsigned i=0;i<6;i++){
            vertices[i].position[0]=xy[i][0];vertices[i].position[1]=xy[i][1];
            vertices[i].position[3]=1;
        }
        method(0x17C8,1);method(0x17CC,1);
        CHECK(nv2a_gpu_draw(&draw,vertices,6));
        method(0x17CC,0);for(unsigned i=0;i<4;i++)report[i]=0xFFFFFFFF;
        method(0x17D0,0x01008000);
        CHECK(report[3]==0xFFFFFFFF); /* Completion waits for a nonblocking poll. */
        uint64_t deadline=GetTickCount64()+5000;
        while(nv2a_pb_exec_poll_reports()){CHECK(GetTickCount64()<deadline);SwitchToThread();}
        CHECK(report[2]==48 && report[3]==0 && report[4]==0xFFFFFFFF);
    }
#endif
    puts("PASS: bounded RGB565/ARGB and Z16 clears preserve surrounding pixels");
    return 0;
}
