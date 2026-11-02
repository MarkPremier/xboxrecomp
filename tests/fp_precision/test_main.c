#include <stdio.h>
#include <fenv.h>
#include <float.h>
#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"
/* Independent reference: the prior precision helper, including its wider
 * exponent path. Compare all host rounding modes, not just nearest. */
static double reference(double x) {
    double ax=fabs(x);int e;
    if((ax<=FLT_MAX && ax>=FLT_MIN)||ax==0.0||x!=x||ax==INFINITY)return (double)(float)x;
    x=frexp(x,&e);return ldexp((double)(float)x,e);
}
static uint64_t bits(double x){uint64_t b;memcpy(&b,&x,8);return b;}
static unsigned checks;
static int check(double x){
    volatile double input=x;
    double expected=reference(input),actual=recomp_fp_round24(input);++checks;
    if((isnan(expected)&&isnan(actual))||bits(expected)==bits(actual))return 1;
    fprintf(stderr,"input=%016llX got=%016llX expected=%016llX\n",
        (unsigned long long)bits(x),(unsigned long long)bits(actual),(unsigned long long)bits(expected));return 0;
}
int main(void){
    fenv_t saved; if(feholdexcept(&saved))return 2;
    const int modes[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
    const double edge[]={0.0,-0.0,FLT_MIN,FLT_MAX,-FLT_MIN,-FLT_MAX,DBL_MIN,-DBL_MIN,
        DBL_MAX,-DBL_MAX,INFINITY,-INFINITY,NAN,1.0,-1.0,1.0+0x1p-24,1.0+3*0x1p-24};
    for(unsigned mode=0;mode<4;++mode){
        if(fesetround(modes[mode]))return 2;
        for(unsigned i=0;i<sizeof edge/sizeof *edge;++i){
            if(!check(edge[i])||!check(nextafter(edge[i],INFINITY))||!check(nextafter(edge[i],-INFINITY)))return 1;
        }
        uint64_t random=UINT64_C(0x73283F124890ABCD);
        for(unsigned n=0;n<1000000;++n){
            random^=random<<13;random^=random>>7;random^=random<<17;
            double x;memcpy(&x,&random,8);if(!check(x))return 1;
        }
    }
    fesetenv(&saved);printf("PASS: %u precision comparisons across four rounding modes\n",checks);return 0;
}
