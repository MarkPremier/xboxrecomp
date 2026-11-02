/* Real native depth testing and draw dispatch; no title data or window. */
#include "../../src/kernel/nv2a_gpu_d3d11.cpp"
extern "C" {
void xbox_Nv2aAcknowledgeHandshakes(void) {}
void (*g_xbox_path_hook)(const char *) = nullptr;
void xbox_CloakMarker(const char *) {}
void xbox_FramebufferSize(uint32_t *w,uint32_t *h) {*w=32;*h=32;}
int nv2a_pb_exec_batch_spans(float) {return 1;}
}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int quad(Nv2aGpuDraw &state,float z)
{
    Nv2aGpuVertex vertices[6] = {};
    const float xy[6][2]={{0,0},{32,0},{0,32},{32,0},{32,32},{0,32}};
    for (int i=0;i<6;i++) {
        vertices[i].position[0]=xy[i][0];vertices[i].position[1]=xy[i][1];
        vertices[i].position[2]=z;vertices[i].position[3]=1;
        for(int j=0;j<4;j++)vertices[i].diffuse[j]=1;
    }
    return nv2a_gpu_draw(&state,vertices,6);
}
int main()
{
    _putenv_s("RECOMP_RENDER_SCALE","1");_putenv_s("RECOMP_WIDE","1");
    uint32_t color[32*32]={}; uint16_t depth[32*32];
    std::fill(std::begin(depth),std::end(depth),uint16_t(0xFFFF));
    Nv2aGpuDraw state={};state.color=(uint8_t *)color;state.depth=(uint8_t *)depth;
    state.width=state.height=state.clip_width=state.clip_height=32;
    state.pitch=128;state.bytes_per_pixel=4;state.depth_pitch=64;state.depth_format=1;
    state.depth_enable=1;state.depth_function=0x201;state.depth_mask=1;
    state.color_mask=0x01010101;state.polygon_front=state.polygon_back=0x1B02;
    state.zmin_max_control=1;
    CHECK(nv2a_gpu_available());
    CHECK(quad(state,10000)); // Write an occluding wall.
    state.depth_mask=0;state.color_mask=0;
    nv2a_gpu_zpass_reset();nv2a_gpu_zpass_enable(1);
    CHECK(quad(state,20000)); // Behind wall: no passing pixels.
    nv2a_gpu_zpass_enable(0);CHECK(nv2a_gpu_zpass_result()==0);
    CHECK(nv2a_gpu_zpass_result()==0);
    nv2a_gpu_zpass_reset();nv2a_gpu_zpass_enable(1);
    CHECK(quad(state,5000));
    nv2a_gpu_zpass_enable(0);CHECK(nv2a_gpu_zpass_result()==1024);
    CHECK(quad(state,5000)); // Disabled draws do not accumulate.
    CHECK(nv2a_gpu_zpass_result()==1024);
    nv2a_gpu_zpass_enable(1);CHECK(quad(state,5000));
    CHECK(nv2a_gpu_zpass_result()==2048); // Report does not reset.
    nv2a_gpu_zpass_reset();CHECK(nv2a_gpu_zpass_result()==0);
    CHECK(quad(state,5000)); // Reset preserves enabled state.
    CHECK(nv2a_gpu_zpass_result()==1024);
    nv2a_gpu_zpass_enable(0);
    // Non-native resolution must still count logical guest pixels.
    nv2a_gpu_set_render_scale(2); nv2a_gpu_flip(color,sizeof color,0,128);
    CHECK(quad(state,5000));
    nv2a_gpu_zpass_reset();nv2a_gpu_zpass_enable(1);CHECK(quad(state,5000));
    CHECK(nv2a_gpu_zpass_result()==1024);
    nv2a_gpu_zpass_enable(0);
    std::puts("PASS: occluded/visible, reset/enable, cumulative and scaled ZPASS counts");
}
