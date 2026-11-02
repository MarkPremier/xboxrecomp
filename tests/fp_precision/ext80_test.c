#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double value(uint64_t sig,uint16_t se) {
    uint8_t bytes[10];memcpy(bytes,&sig,8);memcpy(bytes+8,&se,2);return recomp_load_ext80(bytes);
}
static uint64_t bits(double x){uint64_t b;memcpy(&b,&x,8);return b;}
int main(void) {
    fenv_t saved;CHECK(feholdexcept(&saved)==0);
    CHECK(fesetround(FE_TONEAREST)==0);
    /* The actual x87 pi/2 encoding used by CRT asin(+/-1). */
    CHECK(bits(value(UINT64_C(0xC90FDAA22168C235),0x3FFF))==UINT64_C(0x3FF921FB54442D18));
    CHECK(bits(value(UINT64_C(0xC90FDAA22168C235),0xBFFF))==UINT64_C(0xBFF921FB54442D18));
    CHECK(value(UINT64_C(0x8000000000000000),0x3FFF)==1);
    CHECK(isinf(value(UINT64_C(0x8000000000000000),0x7FFF)));
    CHECK(isnan(value(UINT64_C(0xC000000000000001),0x7FFF)));
    CHECK(isnan(value(1,0x3FFF))); // Unsupported unnormal.
    CHECK(bits(value(0,0x8000))==UINT64_C(0x8000000000000000));
    const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    uint64_t random=UINT64_C(0x893724681ABCD120);
    for(unsigned mode=0;mode<4;mode++) {
        CHECK(fesetround(modes[mode])==0);
        for(unsigned i=0;i<250000;i++) {
            random^=random<<13;random^=random>>7;random^=random<<17;
            if(((random>>52)&0x7FF)==0x7FF)continue;
            double input;memcpy(&input,&random,8);uint8_t stored[12];memset(stored,0xA5,sizeof stored);
            recomp_store_ext80(stored,input);
            CHECK(stored[10]==0xA5 && stored[11]==0xA5);
            CHECK(bits(recomp_load_ext80(stored))==random);
        }
        int up=modes[mode]==FE_UPWARD,down=modes[mode]==FE_DOWNWARD;
        /* Halfway between 1 and the next double: nearest chooses even. */
        CHECK(bits(value(UINT64_C(0x8000000000000400),0x3FFF))==UINT64_C(0x3FF0000000000000)+(up?1:0));
        CHECK(bits(value(UINT64_C(0x8000000000000400),0xBFFF))==UINT64_C(0xBFF0000000000000)+(down?1:0));
        /* Exactly half a binary64 subnormal; cover shifts beyond word width. */
        CHECK(bits(value(UINT64_C(0x8000000000000000),16383-1075))==(uint64_t)(up?1:0));
        CHECK(bits(value(1,0))==(uint64_t)(up?1:0));
        CHECK(bits(value(UINT64_C(0x8000000000000000),16383+1024))==
              ((down||modes[mode]==FE_TOWARDZERO)?UINT64_C(0x7FEFFFFFFFFFFFFF):UINT64_C(0x7FF0000000000000)));
    }
    CHECK(fesetenv(&saved)==0);
    puts("PASS: extended constants, all rounding modes and 1M binary64 round trips");
    return 0;
}
