/******************************************************************************************
 * bp_gfx.h -- 算圖層：解析度縮放、後製特效與除錯疊層。
 *
 * 為什麼要畫進 RenderTexture，而不是直接畫到後置緩衝區（backbuffer）？
 *   1. 「復古解析度」滑桿。Boomer shooter 玩家常刻意以較低的內部解析度遊玩
 *      （從 320x200 一路到原生解析度），既是為了忠實的年代感，也是為了幀率；
 *      而要做到這一點，就必須有一個離屏（offscreen）目標。
 *   2. 後製處理（受傷時的色差、底片顆粒、暗角、調色板量化）需要把整個畫面
 *      當成一張貼圖來取樣。
 *   3. 它讓 MSAA 與各向異性過濾（anisotropy）有唯一一個控制點，因此同一組
 *      品質預設在桌機 GL 3.3 與 WebGL 1.0 上的行為完全一致。
 ******************************************************************************************/
#ifndef BP_GFX_H
#define BP_GFX_H

#include "bp_common.h"

typedef enum {
    BP_QUALITY_LOW = 0,      /* 0.50x 內部解析度，無後製、無 MSAA */
    BP_QUALITY_MEDIUM,       /* 0.75x，顆粒 + 暗角              */
    BP_QUALITY_HIGH,         /* 1.00x，完整後製，4x 各向異性過濾           */
    BP_QUALITY_ULTRA,        /* 1.25x 超取樣（supersample），完整後製         */
    BP_QUALITY_COUNT
} bp_quality;

typedef struct {
    RenderTexture2D scene;      /* 3D 世界 + 視圖模型（viewmodel）                  */
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

    /* 下列數值每幀由遊戲邏輯（gameplay）驅動 */
    float           damage_flash;    /* 0..1，紅色脈動                   */
    float           speed_lines;     /* 0..1                             */
    float           heal_flash;
    float           time;

    /* 除錯顯示選項與 FPS 平滑值 */
    bool            show_wireframe;
    bool            show_collision;
    bool            show_overlay;
    int             fps_smooth;
} bp_gfx;

extern bp_gfx g_gfx;

void bp_gfx_init(void);
void bp_gfx_shutdown(void);
void bp_gfx_set_quality(bp_quality q);
void bp_gfx_resize(void);              /* 視窗大小改變時呼叫 */

void bp_gfx_begin_scene(void);         /* 開始繪製到離屏（offscreen）算圖目標 */
void bp_gfx_end_scene(void);           /* 套用後製特效並解析（resolve）回後置緩衝區 */

void bp_gfx_flash_damage(float amount);
void bp_gfx_flash_heal(float amount);

/* 繪製於世界空間中的除錯圖元（primitives）。 */
void bp_gfx_draw_aabb(Vector3 min, Vector3 max, Color c);
void bp_gfx_draw_ray(Vector3 from, Vector3 to, Color c);
void bp_gfx_draw_grid(Vector3 center, float size, float step, Color c);

const char *bp_gfx_quality_name(bp_quality q);

#endif /* BP_GFX_H */
