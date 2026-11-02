#include "../../src/kernel/nv2a_frame_pacing.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void)
{
    Nv2aFramePacer p={0};
    CHECK(nv2a_video_frame_mode(0,1,1)==1);
    CHECK(nv2a_video_frame_mode(1,0,0)==1); /* Repeated surface stays limited. */
    CHECK(nv2a_video_frame_mode(1,1,0)==0); /* Menu draw releases the limit. */
    CHECK(nv2a_video_frame_mode(0,0,0)==0);
    CHECK(nv2a_frame_limit(0,60,0)==0); /* Menus/gameplay remain unlocked. */
    CHECK(nv2a_frame_limit(0,60,1)==60);
    CHECK(nv2a_frame_limit(240,60,1)==60);
    CHECK(nv2a_frame_limit(30,60,1)==30);
    CHECK(nv2a_frame_limit(-2,60,1)==-2); /* Explicit vblank policy remains. */
    CHECK(nv2a_frame_limit(0,0,1)==0);
    CHECK(nv2a_frame_limit(0,120,0)==0);
    CHECK(nv2a_frame_limit(0,120,1)==120);
    CHECK(nv2a_frame_limit(240,120,1)==120);
    CHECK(nv2a_frame_limit(60,120,1)==60);
    CHECK(nv2a_frame_limit(0,200,0)==0);
    CHECK(nv2a_frame_limit(0,200,1)==200);
    CHECK(nv2a_frame_limit(240,200,1)==200);
    CHECK(nv2a_frame_limit(120,200,1)==120);
    CHECK(nv2a_video_texture(0x24,640,480));
    CHECK(nv2a_video_texture(0x25,640,480));
    CHECK(!nv2a_video_texture(0x0C,640,480));
    CHECK(!nv2a_video_texture(0x24,64,64));
    CHECK(nv2a_video_coverage(0,0,640,480,640,480));
    CHECK(nv2a_video_coverage(0,60,640,420,640,480));
    CHECK(!nv2a_video_coverage(20,20,180,140,640,480));
    CHECK(nv2a_frame_deadline(&p,0,1000000000)==1000000000);
    CHECK(nv2a_frame_deadline(&p,60,1000000000)==1000000000);
    CHECK(nv2a_frame_deadline(&p,60,1001000000)==1016666666);
    CHECK(nv2a_frame_deadline(&p,60,1017000000)==1033333332);
    /* A stall restarts the schedule, rather than requesting a burst of frames. */
    CHECK(nv2a_frame_deadline(&p,60,2000000000)==2000000000);
    CHECK(nv2a_frame_deadline(&p,60,2001000000)==2016666666);
    /* Leaving a cutscene removes its deadline immediately. */
    CHECK(nv2a_frame_deadline(&p,0,2002000000)==2002000000);
    CHECK(p.deadline==0);
    CHECK(nv2a_frame_deadline(&p,60,2003000000)==2003000000);
    CHECK(nv2a_frame_deadline(&p,30,2004000000)==2004000000);
    CHECK(nv2a_frame_deadline(&p,30,2005000000)==2037333333);
    CHECK(nv2a_frame_deadline(&p,120,3000000000)==3000000000);
    CHECK(nv2a_frame_deadline(&p,120,3001000000)==3008333333);
    CHECK(nv2a_frame_deadline(&p,120,3009000000)==3016666666);
    CHECK(nv2a_frame_deadline(&p,0,3010000000)==3010000000);
    CHECK(nv2a_frame_deadline(&p,200,4000000000)==4000000000);
    CHECK(nv2a_frame_deadline(&p,200,4001000000)==4005000000);
    CHECK(nv2a_frame_deadline(&p,200,4006000000)==4010000000);
    puts("video detection, pacing and mode transitions passed");return 0;
}
