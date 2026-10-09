/******************************************************************************************
 * bp_weapons.h -- 武器庫：數值、開火／換彈狀態機、hitscan、彈體與第一人稱 viewmodel。
 *
 * 刻意採用資料驅動
 *   設計師想調的每一個數字都放在 assets/cfg/weapons.cfg。bp_weapons.c 裡那份編譯進去的
 *   表格只是檔案「遺失時的後備（fallback）」，如此一來，就算某個 asset 忘了隨包附上，
 *   遊戲也永遠開得起來。重新平衡一把武器，只要改文字檔、在網頁版再重新整理一次 —— 不必
 *   重新編譯。
 *
 * 為何同時需要 hitscan 與彈體
 *   對快速武器而言，hitscan 才是正確的模型：重要的是玩家按下那一瞬間的意圖，任何飛行時間
 *   都會被感覺成輸入延遲（input lag）。對火箭發射器而言，彈體才是正確的模型：預判移動
 *   目標是一門技術，而爆炸傷害也需要一個「可以抵達的位置」。因此武器庫兩者兼備，且傷害
 *   結算共用同一套（見 bp_hitdetect.h），兩者永遠不可能對「有沒有打中」給出不同答案。
 ******************************************************************************************/
#ifndef BP_WEAPONS_H
#define BP_WEAPONS_H

#include "bp_common.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "bp_hitdetect.h"

#define WPN_MAX_SLOTS 5

typedef enum {
    WPN_ID_PISTOL = 0,
    WPN_ID_SHOTGUN,
    WPN_ID_SMG,
    WPN_ID_RAIL,
    WPN_ID_ROCKET,
    WPN_ID_COUNT
} bp_weapon_id;

typedef enum {
    WCATEGORY_HITSCAN = 0,
    WCATEGORY_PROJECTILE,
    WCATEGORY_BEAM
} bp_weapon_category;

typedef struct {
    bp_weapon_id     id;
    char             cfg_id[16];
    char             display_name[32];
    int              slot;
    bp_weapon_category category;

    float            damage;
    int              pellets;
    float            spread_hip;
    float            spread_ads;
    float            spread_move;
    float            fire_rate;
    bool             full_auto;
    int              mag_size;
    int              reserve_max;
    float            reload_time;
    float            swap_time;

    float            recoil_pitch;
    float            recoil_yaw;
    float            recoil_kick;
    float            shake;

    bp_sfx_id        sfx_fire;
    bp_sfx_id        sfx_reload;
    float            tracer_width;
    float            tracer_life;
    float            range;
    float            knockback;
    int              penetration;
    int              ammo_per_pickup;
    float            headshot_mult;
    Color            color;

    /* 只有彈體武器才會用到的欄位 */
    float            proj_speed;
    float            proj_radius;
    float            proj_gravity;
    float            blast_radius;
    float            blast_damage;
    float            blast_self;
    float            blast_knockback;

    /* 第一人稱視圖模型（viewmodel） */
    bp_model_id      vm_body;
    bp_model_id      vm_part_b;
    bp_model_id      vm_part_c;
    Vector3          vm_offset;
    Vector3          vm_scale;
    Vector3          vm_muzzle;      /* 槍口在 viewmodel 座標系中的位置 */
} bp_weapon_def;

typedef struct {
    Vector3  pos;
    Vector3  vel;
    float    age;
    float    life;
    float    damage;
    float    radius;         /* 爆炸半徑 */
    float    body_radius;
    float    gravity;
    float    knockback;
    float    self_scale;
    bool     from_player;
    bool     alive;
    bool     trail;
    Color    color;
    bp_model_id model;
    bp_handle owner;
} bp_projectile;

typedef enum {
    WSTATE_IDLE = 0,
    WSTATE_FIRING,
    WSTATE_RELOADING,
    WSTATE_SWITCHING_OUT,
    WSTATE_SWITCHING_IN,
    WSTATE_EMPTY
} bp_weapon_state;

typedef struct {
    bp_weapon_def def[WPN_ID_COUNT];
    bool          def_loaded;

    bool          owned[WPN_ID_COUNT];
    int           mag[WPN_ID_COUNT];
    int           reserve[WPN_ID_COUNT];

    bp_weapon_id  current;
    bp_weapon_id  pending;          /* 正在切換過去的那把武器 */
    bp_weapon_state state;
    float         state_time;
    float         cooldown;
    float         reload_progress;
    float         switch_progress;

    /* 手感 */
    float         spread_current;
    float         recoil_viewmodel;
    float         ads;              /* 0 = 腰射（hip） .. 1 = 瞄準（aimed） */
    bool          ads_held;
    float         sway_x, sway_y;
    float         bob_phase;
    float         dry_fire_time;

    /* 統計 */
    int           shots_fired;
    int           shots_hit;
    int           headshots;
    float         damage_dealt;

    bp_projectile projectiles[MAX_PROJECTILES];
    int           projectile_count;

    bool          initialised;
} bp_weapon_sys;

extern bp_weapon_sys g_weapons;
extern const bp_weapon_def *g_weapon_table;

void  bp_weapons_init(void);
bool  bp_weapons_load_config(const char *path);
void  bp_weapons_reset_loadout(bool fullArsenal);

void  bp_weapons_update(float dt, Camera3D cam, Vector3 velocity, bool onGround);
void  bp_weapons_try_fire(bool triggerHeld, bool triggerPressed);
void  bp_weapons_try_reload(void);
bool  bp_weapons_try_select(bp_weapon_id id);
void  bp_weapons_cycle(int direction);

void  bp_weapons_give(bp_weapon_id id, int ammo, bool switchTo);
void  bp_weapons_add_ammo(bp_weapon_id id, int amount);
void  bp_weapons_add_reserve_all(int amount);

void  bp_weapons_draw_viewmodel(Camera3D cam, float speed01);
void  bp_weapons_draw_projectiles(Camera3D cam);

/* 供 HUD 與 AI 使用的「目前狀態」查詢（它們需要知道玩家是否正在換彈，以決定要不要壓上來
 * 進攻） */
const bp_weapon_def *bp_weapons_current_def(void);
bool   bp_weapons_busy(void);
float  bp_weapons_reload_fraction(void);
bool   bp_weapons_can_fire(void);
int    bp_weapons_total_reserve(void);
float  bp_weapons_accuracy(void);
bool   bp_weapons_owns(bp_weapon_id id);

/* 彈體也可以由敵方生成 */
void   bp_projectile_spawn(Vector3 pos, Vector3 dir, float speed, float damage,
                           float radius, float blastRadius, bool fromPlayer, Color color,
                           bp_model_id model, bp_handle owner);
void   bp_projectile_explode(bp_projectile *p, Vector3 at);

#endif /* BP_WEAPONS_H */
