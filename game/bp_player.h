/******************************************************************************************
 * bp_player.h -- 玩家軀體：移動、生命／護甲、拾取物，以及整款遊戲的手感。
 *
 * 移動模型
 *   以加速度為基礎，地面與空中各有自己的常數；摩擦力只施加在「與輸入方向相反」的那個速度
 *   分量上。玩家實際感受到的後果：
 *     - 離開按鍵會很快停下來（摩擦力），但轉彎途中仍保有動量
 *     - 空中操控有限但確實存在，所以空中可以修正，卻無法反轉一整段跳躍
 *     - 繞圈橫移能把速度堆到直線奔跑上限之上（經典的「circle-strafe 比直線快」性質，正是
 *       競技場移動之所以成為一門技術的原因）。我們刻意允許一個遠高於地面速度上限的硬上限，
 *       讓這種技巧成為自我表達的空間，而不是破壞平衡的漏洞。
 *
 *   蹲下只是「高度改變 + 速度懲罰」，而不是另一套碰撞形狀。只有一種形狀，就只有一條解算
 *   路徑，也就沒有「蹲下跳穿過天花板」這類漏洞。
 ******************************************************************************************/
#ifndef BP_PLAYER_H
#define BP_PLAYER_H

#include "bp_common.h"
#include "../engine/bp_entity.h"

typedef enum {
    PICKUP_NONE = 0,
    PICKUP_HEALTH,
    PICKUP_ARMOR,
    PICKUP_AMMO,
    PICKUP_WEAPON
} bp_pickup_kind;

typedef struct {
    Vector3       pos;
    bp_pickup_kind kind;
    float         amount;
    int           weapon_id;
    float         age;
    float         respawn_at;
    bool          alive;
    bool          spinning;
    float         bob;
} bp_pickup;

typedef enum {
    PST_ALIVE = 0,
    PST_DYING,
    PST_DEAD,
    PST_RESPAWNING
} bp_player_state;

typedef struct {
    /* 身體 */
    Vector3 pos;          /* 腳部位置 */
    Vector3 vel;
    float   yaw;
    bool    on_ground;
    bool    crouched;
    bool    crouch_held;
    float   crouch_amount;   /* 0 = 站立 .. 1 = 全蹲，驅動視線高度與命中框 */
    float   height;
    float   radius;

    /* 生命指標 */
    float   health;
    float   armor;
    float   max_health;
    float   max_armor;
    bp_player_state state;
    float   death_time;
    float   invuln_time;
    float   hazard_time;

    /* 移動手感 */
    float   speed_ground;
    float   speed_sprint;
    float   speed_crouch;
    float   speed_air;
    float   accel_ground;
    float   accel_air;
    float   friction_ground;
    float   jump_velocity;
    float   coyote_time;
    float   jump_buffer;
    bool    jump_held;
    float   step_phase;
    float   land_impact;
    float   air_time;
    float   speed_current;
    float   speed_peak;

    /* 回饋 */
    float   damage_flash;
    float   heal_flash;
    float   last_damage_dir;     /* 相對視角 yaw 的弧度，供傷害方向指示器使用 */
    float   last_damage_time;
    Vector3 last_damage_source;
    float   low_health_beep;

    /* 分數 */
    int     score;
    int     kills;
    int     streak;
    float   streak_time;
    int     secrets_found;
    int     pickups_taken;
    float   time_alive;

    /* 關卡進度。玩家控制器獨佔與觸發器的接觸，因此只有這裡有資格把這兩個旗標立起來；遊戲
     * 層只是輪詢它們，再自行判定「出口」與「隱藏要素」到底代表什麼。兩者在單一關卡內都是
     * 黏著的（sticky），一旦設立就不會中途清除。 */
    bool    exit_reached;
    float   exit_time;          /* 觸碰到出口當下的 g_engine.time_total */
    bool    secret_reached;
    float   fall_speed_peak;    /* 本關最快的向下速度，供戰績畫面使用 */

    /* 拾取物 */
    bp_pickup pickups[MAX_PICKUPS];
    int       pickup_count;

    bool      initialised;
} bp_player;

extern bp_player g_player;

void    bp_player_init(void);
void    bp_player_reset(Vector3 start, float yaw);
void    bp_player_update(float dt);
void    bp_player_draw_debug(void);

void    bp_player_damage(float amount, Vector3 sourcePos, float armorAbsorb);
void    bp_player_heal(float amount);
void    bp_player_add_armor(float amount);
void    bp_player_impulse(Vector3 impulse);
Vector3 bp_player_position(void);
Vector3 bp_player_eye(void);
Vector3 bp_player_velocity(void);
bool    bp_player_alive(void);
float   bp_player_speed01(void);
bool    bp_player_on_ground(void);

/* 拾取物 */
void    bp_pickup_spawn(bp_pickup_kind kind, Vector3 pos, float amount, int weaponId);
void    bp_pickup_update(float dt);
void    bp_pickup_draw(Camera3D cam, Vector3 viewer);
void    bp_pickup_clear(void);

void    bp_player_add_score(int amount);
void    bp_player_register_kill(int scoreValue);

#endif /* BP_PLAYER_H */
