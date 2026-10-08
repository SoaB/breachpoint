/******************************************************************************************
 * bp_gfx.h -- renderer layer: resolution scaling, post FX, debug overlays.
 *
 * Why render to a RenderTexture instead of straight to the backbuffer?
 *   1. A "retro resolution" slider. Boomer shooters are frequently played at a deliberately
 *      reduced internal resolution (320x200 up to native) for both authenticity and frame
 *      rate. Doing that requires an offscreen target.
 *   2. Post-processing (chromatic aberration on damage, film grain, vignette, palette
 *      quantisation) needs the whole frame as a texture.
 *   3. It gives us one place to control MSAA and anisotropy, so the same quality presets
 *      behave identically on desktop GL 3.3 and WebGL 1.0.
 ******************************************************************************************/
#ifndef BP_GFX_H
#define BP_GFX_H

#include "bp_common.h"

typedef enum {
    BP_QUALITY_LOW = 0,      /* 0.50x internal res, no post, no MSAA */
    BP_QUALITY_MEDIUM,       /* 0.75x, grain + vignette              */
    BP_QUALITY_HIGH,         /* 1.00x, full post, 4x aniso           */
    BP_QUALITY_ULTRA,        /* 1.25x supersample, full post         */
    BP_QUALITY_COUNT
} bp_quality;

typedef struct {
    RenderTexture2D scene;      /* 3D world + viewmodel                  */
    int             scene_w;
    int             scene_h;

    Shader          post;
    bool            post_valid;
    int             loc_time;
    int             loc_damage;
    int             loc_speed;
    int             loc_grain;
    int             loc_vignette;
    int             loc_aberration;
    int             loc_scanline;
    int             loc_resolution;

    bp_quality      quality;
    float           resolution_scale;
    bool            post_enabled;
    bool            crt_enabled;

    /* driven by gameplay each frame */
    float           damage_flash;    /* 0..1 red pulse                   */
    float           speed_lines;     /* 0..1                             */
    float           heal_flash;
    float           time;

    /* debug */
    bool            show_wireframe;
    bool            show_collision;
    bool            show_overlay;
    int             fps_smooth;
} bp_gfx;

extern bp_gfx g_gfx;

void bp_gfx_init(void);
void bp_gfx_shutdown(void);
void bp_gfx_set_quality(bp_quality q);
void bp_gfx_resize(void);              /* call on window resize */

void bp_gfx_begin_scene(void);         /* begin drawing into the offscreen target */
void bp_gfx_end_scene(void);           /* resolve to the backbuffer with post FX  */

void bp_gfx_flash_damage(float amount);
void bp_gfx_flash_heal(float amount);

/* Debug primitives drawn in world space. */
void bp_gfx_draw_aabb(Vector3 min, Vector3 max, Color c);
void bp_gfx_draw_ray(Vector3 from, Vector3 to, Color c);
void bp_gfx_draw_grid(Vector3 center, float size, float step, Color c);

const char *bp_gfx_quality_name(bp_quality q);

#endif /* BP_GFX_H */
