/* The graffiti assistant's pattern (src/runtime_pad.c): it must sweep the stencil, keep the aim
 * inside the limits, and never ask for the shake and the spray at the same time. */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include "../src/runtime_pad.c"

/* The GPU library (not linked here): the mouse, the input settings and the overlay. */
void tlggpu_mouse_take(TlgGpuMouse *mouse) { memset(mouse,0,sizeof(*mouse)); }
void tlggpu_mouse_pad_hud(int a, float x, float y, int t, int c) { (void)a; (void)x; (void)y; (void)t; (void)c; }
void tlggpu_input_settings(TlgGpuInputSettings *settings) { memset(settings,0,sizeof(*settings)); }
int tlggpu_overlay_captures_input(void) { return 0; }
void tlggpu_overlay_notice(const char *text, float seconds) { (void)text; (void)seconds; }
void tlggpu_graffiti_guide(int force) { (void)force; }
float tlggpu_sensor_aim_scale(void) { return 1.f; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* The stencil the graffiti asks for (radians of aim) and how wide the spray paints. */
#define STENCIL_YAW 0.65f
#define STENCIL_PITCH 0.55f
#define BRUSH 0.10f

/* The part of the stencil the assistant paints in `seconds`, sampling only the instants where
 * the spray is on (the brush is a disc of BRUSH radians around the aim). */
static float stencil_coverage(float seconds) {
    static char painted[64][64];
    const int cells=64;
    int hit=0, total=0;
    for (int iy=0;iy<cells;++iy) {
        const float pitch=-STENCIL_PITCH+2.f*STENCIL_PITCH*iy/(cells-1);
        for (int ix=0;ix<cells;++ix) {
            const float yaw=-STENCIL_YAW+2.f*STENCIL_YAW*ix/(cells-1);
            if ((yaw*yaw)/(STENCIL_YAW*STENCIL_YAW)+(pitch*pitch)/(STENCIL_PITCH*STENCIL_PITCH)>1.f) continue;
            ++total;
            painted[ix][iy]=0;
        }
    }
    for (float t=0.f;t<seconds;t+=1.f/60.f) {
        const MotionAssist a=motion_assist_at(t);
        if (!a.spray) continue;
        for (int iy=0;iy<cells;++iy) {
            const float pitch=-STENCIL_PITCH+2.f*STENCIL_PITCH*iy/(cells-1);
            for (int ix=0;ix<cells;++ix) {
                if (painted[ix][iy]) continue;
                const float yaw=-STENCIL_YAW+2.f*STENCIL_YAW*ix/(cells-1);
                if ((yaw*yaw)/(STENCIL_YAW*STENCIL_YAW)+(pitch*pitch)/(STENCIL_PITCH*STENCIL_PITCH)>1.f) continue;
                const float dy=a.yaw-yaw, dp=a.aim_pitch-pitch;
                if (dy*dy+dp*dp<=BRUSH*BRUSH) { painted[ix][iy]=1; ++hit; }
            }
        }
    }
    return total ? (float)hit/(float)total : 0.f;
}

int main(void) {
    /* The aim stays well inside the limits the runtime clamps to (yaw 1.4, aim pitch 1.2). */
    float max_yaw=0.f, max_pitch=0.f, max_step=0.f;
    int sprays=0, shakes=0, samples=0;
    MotionAssist prev=motion_assist_at(0.f);
    for (float t=0.f;t<=150.f;t+=1.f/60.f) {
        const MotionAssist a=motion_assist_at(t);
        const float dy=fabsf(a.yaw-prev.yaw), dp=fabsf(a.aim_pitch-prev.aim_pitch);
        if (dy>max_step) max_step=dy;
        if (dp>max_step) max_step=dp;
        assert(!(a.shake&&a.spray)); /* the shake and the spray never fight */
        if (a.spray) ++sprays;
        if (a.shake) ++shakes;
        ++samples;
        max_yaw=fmaxf(max_yaw,fabsf(a.yaw));
        max_pitch=fmaxf(max_pitch,fabsf(a.aim_pitch));
        prev=a;
    }
    assert(max_yaw<0.8f && max_pitch<0.65f);
    /* The aim moves smoothly: no teleports between two samples (1/60 s). */
    assert(max_step<0.05f);
    /* The spray is on 60% of each half second (the shake takes the other 40%). */
    assert(sprays>samples*55/100 && sprays<samples*65/100);
    assert(shakes>samples*35/100 && shakes<samples*45/100);
    /* The stencil is covered: the spiral it replaced left a quarter of it unpainted. */
    const float c30=stencil_coverage(30.f);
    assert(c30>0.95f);
    assert(stencil_coverage(60.f)>c30);
    /* The first seconds already paint most of it. */
    assert(stencil_coverage(15.f)>0.80f);
    printf("motion-assist-test: ok (stencil covered %.1f%% in 30 s, %.1f%% in 60 s)\n",
           c30*100.f,stencil_coverage(60.f)*100.f);
    return 0;
}
