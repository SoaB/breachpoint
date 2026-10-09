/******************************************************************************************
 * bp_assets.h -- 程序化（procedural）資產庫。
 *
 * 遊戲中每一項視覺與字型資產，都是在啟動時以程式碼產生的。倉庫裡沒有任何 .png、.ttf、.obj
 * 或 .wav 檔案。理由如下：
 *
 *   - 出貨體積。整個 Web 版（引擎＋遊戲＋資產）只有幾百 KB。
 *   - 美術方向的一致性。色盤、紋素密度（texel density）與倒角規則都只存在於一處，因此場
 *     景中每一種表面看起來都屬於同一個世界。
 *   - 商業發行時的授權風險為零。
 *   - 迭代速度：改一張材質就等於改一段程式碼，可以和依賴它的玩法程式碼在同一份 diff 裡
 *     一起被審查。
 *
 * 這個庫就是一個攤平的 handle 結構體。資產在 bp_assets_init() 中建立一次、在
 * bp_assets_shutdown() 中銷毀，遊戲進行中不會重新載入任何東西。
 ******************************************************************************************/
#ifndef BP_ASSETS_H
#define BP_ASSETS_H

#include "bp_common.h"

/* -------------------------------------------------------------------------------------- */
/* 貼圖 id（texture id）                                                                     */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    TEX_NONE = -1,

    /* 建築結構 */
    TEX_WALL_BRICK, TEX_WALL_METAL, TEX_WALL_CONCRETE, TEX_WALL_PANEL, TEX_WALL_RUSTY,
    TEX_FLOOR_GRATE, TEX_FLOOR_TILE, TEX_FLOOR_CONCRETE, TEX_FLOOR_HAZARD,
    TEX_CEILING, TEX_TRIM, TEX_CRATE, TEX_BARREL, TEX_PILLAR,

    /* 自發光／細節貼圖 */
    TEX_LIGHT_PANEL, TEX_SCREEN, TEX_VENT, TEX_PIPE, TEX_BLOOD_SPLAT, TEX_SCORCH,
    TEX_BULLET_HOLE, TEX_LOGO,

    /* billboard 與特效 */
    TEX_PARTICLE_SOFT, TEX_PARTICLE_HARD, TEX_SPARK, TEX_SMOKE, TEX_RING,
    TEX_CROSSHAIR, TEX_GRADIENT, TEX_NOISE,

    TEX_COUNT
} bp_texture_id;

/* -------------------------------------------------------------------------------------- */
/* 模型 id（model id）                                                                       */
/* -------------------------------------------------------------------------------------- */
typedef enum {
    MDL_NONE = -1,

    /* 敵人身體部件（逐部件繪製、各自帶矩陣，四肢才活動得起來） */
    MDL_ENEMY_TORSO, MDL_ENEMY_HEAD, MDL_ENEMY_ARM, MDL_ENEMY_LEG,
    MDL_HOUND_BODY, MDL_HOUND_HEAD, MDL_HOUND_LEG,
    MDL_BRUTE_TORSO, MDL_BRUTE_ARM, MDL_BRUTE_LEG, MDL_BRUTE_HEAD,
    MDL_TURRET_BASE, MDL_TURRET_BARREL,

    /* 視模型（viewmodel）部件 */
    MDL_GUN_PISTOL_BODY, MDL_GUN_PISTOL_SLIDE, MDL_GUN_HAND,
    MDL_GUN_SHOTGUN_BODY, MDL_GUN_SHOTGUN_PUMP, MDL_GUN_SHOTGUN_BARREL,
    MDL_GUN_SMG_BODY, MDL_GUN_SMG_MAG, MDL_GUN_SMG_STOCK,
    MDL_GUN_RAIL_BODY, MDL_GUN_RAIL_COIL, MDL_GUN_RAIL_CORE,
    MDL_GUN_ROCKET_TUBE, MDL_GUN_ROCKET_SIGHT,

    /* 場景道具 */
    MDL_PICKUP_HEALTH, MDL_PICKUP_ARMOR, MDL_PICKUP_AMMO, MDL_PICKUP_WEAPON,
    MDL_PROJECTILE_ROCKET, MDL_PROJECTILE_PLASMA,
    MDL_BARREL, MDL_CRATE,

    MDL_COUNT
} bp_model_id;

/* -------------------------------------------------------------------------------------- */
/* 每模型的算圖後設資料                                                                      */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    Model   model;
    Vector3 origin;        /* 「掛載點（socket）」相對於模型空間的位置                     */
    Vector3 half_extent;     /* 廉價陰影（cheap shadow）與邊界計算用                       */
    Color   tint;
    bool    valid;
} bp_model;

typedef struct {
    Texture2D tex[TEX_COUNT];
    bp_model  mdl[MDL_COUNT];

    Font      font_small;    /* 8px：HUD 標籤、除錯用              */
    Font      font_main;     /* 16px：內文與選單                   */
    Font      font_big;      /* 32px：標題                         */
    Font      font_huge;     /* 64px：主標題、擊殺計數             */
    bool      fonts_valid;

    /* 色盤：HUD、粒子與關卡照明都共用這組顏色 */
    Color     pal_accent;
    Color     pal_accent2;
    Color     pal_danger;
    Color     pal_health;
    Color     pal_armor;
    Color     pal_ammo;
    Color     pal_bg;
    Color     pal_panel;
    Color     pal_text;
    Color     pal_text_dim;

    bool      ready;
    float     load_ms;
} bp_assets;

extern bp_assets g_assets;

void       bp_assets_init(void);
void       bp_assets_shutdown(void);

Texture2D  bp_tex(bp_texture_id id);
bool       bp_model_valid(bp_model_id id);
Model      bp_model_get(bp_model_id id);
Color      bp_pal(int index);          /* 0..N 索引色階（palette ramp），供照明取色 */

/* 便利函式：用位置／旋轉／縮放組出變換矩陣，直接繪製某個模型部件。 */
void bp_draw_model(bp_model_id id, Vector3 position, Vector3 axis, float angleDeg,
                   Vector3 scale, Color tint);

#endif /* BP_ASSETS_H */
