/******************************************************************************************
 * bp_fx.h -- 粒子、貼花（decal）、曳光彈道與畫面空間的戰鬥回饋。
 *
 * 效能契約：整個粒子系統對每張紋理只提交「一次」rlgl 渲染批次（render batch），而不是每
 * 個粒子一次繪製呼叫。在 4096 個粒子的規模下，這就是 ~200 次 draw call（在內顯上根本沒
 * 法玩）與 2 次之間的差別。
 *
 * 批次是以相機的 right/up 向量手工組裝的，好讓粒子能帶有翻滾角（roll angle）——
 * DrawBillboardRec 做不到這一點，而「碎片會旋轉」正是這類遊戲的命中之所以有物理感的主因
 * 之一。
 ******************************************************************************************/
#ifndef BP_FX_H
#define BP_FX_H

#include "../engine/bp_assets.h" /* bp_texture_id */
#include "../engine/bp_common.h"

typedef enum {
    PTX_SPARK = 0, /* 加法混合，依速度拉長                     */
    PTX_SMOKE, /* alpha 混合，逐漸膨脹並淡出               */
    PTX_BLOOD,
    PTX_DEBRIS, /* 受重力影響，會反彈                       */
    PTX_SHELL, /* 彈出的彈殼                               */
    PTX_EMBER,
    PTX_FLASH, /* 壽命極短，加法混合                       */
    PTX_RING /* 擴散的衝擊波廣告牌                       */
} bp_particle_kind;

typedef struct {
    Vector3 pos;
    Vector3 vel;
    float age;
    float life;
    float size0;
    float size1;
    float roll;
    float spin;
    Color color0;
    Color color1;
    uint8_t kind;
    uint8_t tex; /* bp_texture_id */
    uint8_t gravity; /* 0 無, 1 全, 2 半                        */
    uint8_t additive;
    uint8_t bounce;
    uint8_t alive;
} bp_particle;

typedef struct {
    Vector3 pos;
    Vector3 normal;
    float size;
    float age;
    float life;
    uint8_t tex;
    uint8_t alive;
    float roll;
    Color tint;
} bp_decal;

typedef struct {
    Vector3 from;
    Vector3 to;
    float age;
    float life;
    float width;
    Color color;
    uint8_t alive;
} bp_tracer;

typedef struct {
    bp_particle particles[MAX_PARTICLES];
    int particle_head; /* 以輪流方式指派的配置游標            */
    int particle_live;

    bp_decal decals[MAX_DECALS];
    int decal_head;
    int decal_live; /* 供除錯疊層讀取的遙測計數            */

    bp_tracer tracers[MAX_HITSCAN_TRACERS];
    int tracer_head;

    /* 遊戲性回饋（供 HUD 與相機讀取） */
    float hit_marker; /* 0..1，確認命中時飆升                */
    float kill_marker; /* 0..1，擊殺時飆升                    */
    float muzzle_flash; /* 0..1，槍口火光強度                  */
    Vector3 muzzle_pos;
    Vector3 muzzle_dir;
    float shake_amount;
    float shake_time;
    Vector3 shake_offset;
    float speedometer; /* 平滑後的玩家速度，供後製效果使用    */
    int spawned_this_frame; /* 遙測 */
} bp_fx;

extern bp_fx g_fx;

void bp_fx_init(void);
void bp_fx_reset(void);
void bp_fx_update(float dt, Vector3 worldFloorY);
void bp_fx_draw(Camera3D cam);

void bp_fx_particle(bp_particle_kind kind, Vector3 pos, Vector3 vel, float life,
    float size0, float size1, Color c0, Color c1);

/* 組合式發射器 —— 遊戲程式碼一律呼叫這些，絕不直接呼叫原始粒子。 */
void bp_fx_muzzle_flash(Vector3 pos, Vector3 dir, float intensity);
void bp_fx_impact_wall(Vector3 pos, Vector3 normal, float energy);
void bp_fx_impact_flesh(Vector3 pos, Vector3 dir, float energy);
void bp_fx_impact_metal(Vector3 pos, Vector3 normal, float energy);
void bp_fx_explosion(Vector3 pos, float radius);
void bp_fx_blood_mist(Vector3 pos, Vector3 dir, float amount);
void bp_fx_shell_eject(Vector3 pos, Vector3 right, Vector3 up);
void bp_fx_smoke_trail(Vector3 pos, float amount);
void bp_fx_dust_puff(Vector3 pos, float amount);
void bp_fx_speed_burst(Vector3 pos, Vector3 dir);

void bp_fx_decal(bp_texture_id tex, Vector3 pos, Vector3 normal, float size, float life, Color tint);
void bp_fx_tracer(Vector3 from, Vector3 to, float width, float life, Color c);

void bp_fx_hit_marker(bool kill);
void bp_fx_shake(float amount, float duration);
Vector3 bp_fx_shake_offset(void);

#endif /* BP_FX_H */
