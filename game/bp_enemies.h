/******************************************************************************************
 * bp_enemies.h -- 敵人原型（archetype）、AI 狀態機、動畫與命中框。
 *
 * AI 架構
 *   一個涵蓋六個狀態的有限狀態機（DORMANT / IDLE / PURSUE / ATTACK / STAGGER / DYING），
 *   外加一層負責挑選橫移方向的「意願（desire）」。沒有行為樹（behaviour tree）、沒有
 *   GOAP、沒有效用分數（utility scores）。對競技場射擊遊戲而言，正確的 AI 必須「一眼看得
 *   懂」：玩家要能看著一隻敵人，並在 150 ms 內判斷它接下來想幹什麼。每個狀態都有明顯的
 *   前兆（聲音、姿勢、顏色），每次狀態轉換都有最短停留時間，因此敵人不會在兩種行為之間
 *   不斷閃爍。
 *
 *   路徑採用操舵（steering）而非尋路（pathfinding）：追擊 + 障礙物閃避 + 分離。在掩體稀疏
 *   的競技場地圖上，調校良好的「沿牆閃避」無論在 CPU 成本或給人的智慧感上都勝過 A* 網格，
 *   因為它永遠不會產生玩家一眼就察覺的「卡在門口」故障模式。
 *
 * 命中框
 *   一個圓柱形的身體（以 AABB 表示），上方再疊一個較小的頭部方框。會有爆頭，是因為沒有
 *   精準獎勵的遊戲只會訓練玩家亂掃射。頭部方框刻意給得寬鬆（為身體半徑的 0.62 倍），因為
 *   懲罰「差一點點」比獎勵它更令人不快。
 ******************************************************************************************/
#ifndef BP_ENEMIES_H
#define BP_ENEMIES_H

#include "bp_common.h"
#include "../engine/bp_entity.h"
#include "../engine/bp_assets.h"
#include "../engine/bp_math.h"

typedef enum {
    ET_GRUNT = 0,
    ET_HOUND,
    ET_BRUTE,
    ET_TURRET,
    ET_COUNT
} bp_enemy_type;

typedef enum {
    ES_DORMANT = 0,     /* 已生成，但尚未被啟動 */
    ES_IDLE,            /* 巡邏 / 尋找玩家 */
    ES_PURSUE,          /* 取得視線（LOS）或聽見玩家，正在逼近 */
    ES_ATTACK,          /* 已進入射程，攻擊中 */
    ES_STAGGER,         /* 受痛反應，短暫 */
    ES_DYING,           /* 死亡動畫期間，不參與碰撞 */
    ES_DEAD
} bp_enemy_state;

#define ENEMY_MAX_ANIM_PARTS 6

typedef struct {
    char    cfg_id[16];
    char    display_name[32];
    float   health;
    float   armor;
    float   speed_walk;
    float   speed_run;
    float   radius;
    float   height;
    float   scale;
    float   sight_range;
    float   hear_range;
    float   attack_range;
    float   attack_damage;
    float   attack_cooldown;
    int     attack_sfx;
    float   pain_chance;
    int     score;
    float   drop_health;
    float   drop_ammo;
    float   drop_armor;
    Color   color_body;
    Color   color_trim;
    Color   eye_color;
    float   strafe;
    float   aggression;
    bool    projectile;
    float   proj_speed;
    float   proj_radius;
    float   proj_damage;
    bool    is_static;
    float   knockback_scale;
    int     body_set;         /* 0 人形、1 獵犬、2 巨漢（brute）、3 砲塔 */
    int     sfx_sight, sfx_pain, sfx_die;
} bp_enemy_def;

typedef struct {
    bp_enemy_type type;
    bp_enemy_state state;
    float   state_time;

    Vector3 pos;          /* 腳部位置 */
    Vector3 vel;
    float   yaw;          /* 度（degrees），朝向 */
    bool    on_ground;

    float   health;
    float   armor;
    float   max_health;

    float   attack_timer;
    float   stagger_timer;
    float   pain_scale;     /* 最高難度時固定為 0：刻意讓敵人不畏縮 */
    float   die_timer;
    float   pain_flash;
    float   activation_delay;

    /* 感知 */
    bool    can_see;
    float   last_known_dist;
    Vector3 last_known_pos;
    float   memory_time;
    bool    alerted_others;

    /* 操舵（steering） */
    float   strafe_dir;
    float   strafe_timer;
    float   avoid_timer;
    Vector3 avoid_dir;
    float   wander_yaw;

    /* 動畫 */
    float   anim_time;
    float   leg_phase;
    float   arm_phase;
    float   bob;
    float   attack_wind;    /* 0..1 的出招前兆（telegraph） */
    float   lunge;

    /* 簿記 */
    float   damage_dealt_to_player;
    int     hits_taken;
    bool    counted_for_wave;
    bool    dropped_pickup;
} bp_enemy;

typedef struct {
    bp_enemy_def def[ET_COUNT];
    bool         def_loaded;

    bp_pool      pool;
    bp_enemy     storage[MAX_ENEMIES];
    uint32_t     generations[MAX_ENEMIES];
    uint8_t      alive_flags[MAX_ENEMIES];
    uint32_t     free_stack[MAX_ENEMIES];
    uint32_t     iter_list[MAX_ENEMIES];

    int          alive_count;
    int          killed_total;
    int          spawned_total;
    float        damage_received_total;

    bool         initialised;
} bp_enemy_sys;

extern bp_enemy_sys g_enemies;
extern const bp_enemy_def *g_enemy_table;

void   bp_enemies_init(void);
bool   bp_enemies_load_config(const char *path);
void   bp_enemies_clear(void);

bp_handle bp_enemy_spawn(bp_enemy_type type, Vector3 pos, float yaw, bool dormant);
void   bp_enemy_update(float dt, Vector3 playerPos, Vector3 playerEye, bool playerAlive);
void   bp_enemy_draw(Camera3D cam, Vector3 viewer);
void   bp_enemy_draw_debug(void);

/* bp_hitdetect 使用的傷害入口 */
float  bp_enemy_take_damage(bp_handle h, float amount, Vector3 point, Vector3 dir,
                            bool headshot, float knockback, bool *out_killed);

/* 查詢 */
bp_enemy *bp_enemy_get(bp_handle h);
int       bp_enemy_alive_count(void);
int       bp_enemy_count_type(bp_enemy_type t);
bool      bp_enemy_head_box(bp_enemy *e, bp_aabb *out);
bool      bp_enemy_body_box(bp_enemy *e, bp_aabb *out);
Vector3   bp_enemy_center(bp_enemy *e);
bp_enemy_type bp_enemy_type_from_cfg(const char *id);
float     bp_enemy_nearest_distance(Vector3 from, bp_handle *out);
void      bp_enemy_alert_nearby(Vector3 pos, float radius);

#endif /* BP_ENEMIES_H */
