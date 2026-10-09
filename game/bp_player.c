/******************************************************************************************
 * bp_player.c -- 移動、生命值、拾取物與觸發器接觸。
 *
 * 為什麼這個檔案是整個專案裡最重要的一個檔案
 *   在 boomer shooter 裡，玩家「就是」遊戲本體。敵人可以換皮、關卡可以重排、武器
 *   可以重新調校——但只要移動手感不對，其他一切都不重要。因此底下每個常數都是
 *   刻意為之、附有文件說明的決定，而不是隨手填的佔位值；加速度模型也刻意沿用
 *   Quake／DOOM 的血統：
 *
 *     1. wishdir（期望方向）加速，且速度上限只沿期望方向施加。
 *        後果：按住 strafe 轉向時，前進速度會被轉換成側向速度並「疊加」進總速度，
 *        因此繞圈 strafe 可以突破直線速度上限。這個湧現性質就是競技場移動技巧上限
 *        的全部，而且只靠大約 15 行程式碼就能自然成立。我們只用硬上限夾住它，
 *        防止它無限發散。
 *
 *     2. 摩擦力只施加在與輸入「相反」的速度分量上。
 *        後果：放開按鍵後約三分之一秒內就停住（俐落），但變換方向時保留大部分動量
 *        （流暢）。若改成對整個速度向量施加摩擦力——天真的寫法——遊戲手感會像在
 *        糖漿裡跋涉，而這正是獨立 FPS 移動程式碼最常見的單一錯誤。
 *
 *     3. 寬容的跳躍緩衝：coyote time（走出邊緣後一小段時間內跳躍仍然有效）與
 *        jump buffer（落地前稍微提早按跳也照樣起跳）。兩者正常運作時完全隱形，
 *        一旦缺席卻會讓玩家火大。
 *
 *   攝影機「不」由這裡驅動。我們只發布身體位置與速度，bp_camera_simulate() 再據此
 *   推導視線高度、頭部晃動（bob）、FOV 與滾轉。兩者保持分離，之後才能加入死亡攝影
 *   機／觀戰攝影機，而不必改動任何一行移動程式碼。
 ******************************************************************************************/
#include "bp_player.h"

#include "../engine/bp_math.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_input.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_gfx.h"
#include "bp_world.h"
#include "bp_fx.h"
#include "bp_weapons.h"
#include "bp_enemies.h"

bp_player g_player = { 0 };

/* -------------------------------------------------------------------------------------- */
/* 調校                                                                                      */
/* -------------------------------------------------------------------------------------- */
/*
 * 跳躍高度是推導出來的，不是憑感覺猜的：h = v^2 / (2 * |GRAVITY|)。
 *   JUMP_VELOCITY 8.6 -> 原地跳可達 1.32 m。
 * bp_levels.inc 裡的關卡高度預算正是以此為前提：0.55 m 的走上式台階、1.10 m 得
 * 原地跳才上得去的平台、1.50 m 則只有衝刺起跳才搆得到（高度加成來自空中轉向的
 * 投影，而不是更高的跳躍速度）。一旦改動 JUMP_VELOCITY，就必須重新驗證每一張
 * 地圖裡的每一座平台。
 */
#define PLAYER_SPEED_GROUND      8.2f
#define PLAYER_SPEED_SPRINT     11.5f
#define PLAYER_SPEED_CROUCH      3.4f
#define PLAYER_SPEED_AIR         1.35f   /* 空中轉向的加速度上限，不是速度上限 */
#define PLAYER_ACCEL_GROUND     72.0f    /* 0 到全速約 0.11 秒：刻意做成反應近乎瞬間 */
#define PLAYER_ACCEL_AIR        16.0f
#define PLAYER_FRICTION_GROUND  13.0f
#define PLAYER_FRICTION_STOP    17.0f    /* 未按住任何鍵時更高：停得乾淨俐落 */
#define PLAYER_JUMP_VELOCITY     8.6f
#define PLAYER_COYOTE_TIME       0.12f
#define PLAYER_JUMP_BUFFER       0.14f
#define PLAYER_SPEED_HARD_CAP   24.0f    /* 防止繞圈 strafe 疊加到無限大 */
#define PLAYER_FOV_MAX_BONUS     9.0f

#define PLAYER_HEIGHT_CROUCH     1.15f   /* 蹲下時的碰撞高度 */
#define PLAYER_EYE_CROUCH        0.95f
#define PLAYER_CROUCH_SPEED      12.0f   /* 蹲站之間 0->1 的混合速率 */
#define PLAYER_HEAD_CLEAR        0.12f   /* 要能站起身，頭頂所需的淨空 */

/* 摔落傷害。門檻以落地速度而非落下高度表示，因為我們手上真正拿得到的就是落地
 * 速度，而且它與「從移動中的升降台上跳下」之類的情境能正確疊加。
 *   14 m/s 相當於落差 3.5 m。低於此免費；超過的部分每 1 m/s 扣 3.2 點生命。
 * 因此從 SLAG PIT 的 6.9 m 高空走道一躍而下約損 13 點生命——有懲罰但不致死，
 * 對一張希望你乖乖走樓梯的地圖來說，這是正確的取捨。 */
#define PLAYER_FALL_SAFE_SPEED  14.0f
#define PLAYER_FALL_DMG_PER_MS   3.2f
#define PLAYER_LAND_SHAKE_MAX    0.55f

#define PLAYER_STEP_LEN_RUN      2.35f   /* 每步腳步聲之間前進多少公尺 */
#define PLAYER_STEP_LEN_CROUCH   1.45f

#define PLAYER_HAZARD_TICK       0.5f    /* 危險地面兩次扣血之間的間隔秒數 */
#define PLAYER_INVULN_SPAWN      1.1f
#define PLAYER_DEATH_DURATION    1.75f   /* dying 到 dead：死亡鏡頭的窗口 */
#define PLAYER_LOW_HP_THRESH    28.0f
#define PLAYER_LOW_HP_INTERVAL   1.15f

#define PLAYER_STREAK_WINDOW     3.5f    /* 讓連殺得以延續的秒數 */
#define PLAYER_STREAK_MAX        8

#define PICKUP_RADIUS            0.95f   /* 自動拾取的水平判定距離 */
#define PICKUP_HEIGHT_TOL        1.60f   /* 垂直判定範圍：從寬認定，免得撿不到 */
#define PICKUP_BOB_HEIGHT        0.16f
#define PICKUP_SPIN_SPEED       95.0f    /* 拾取物自轉的角速度（度／秒） */

/* -------------------------------------------------------------------------------------- */
/* 輔助函式                                                                                  */
/* -------------------------------------------------------------------------------------- */
static inline Vector3 vec3(float x, float y, float z) { return (Vector3){ x, y, z }; }

/* 只計水平速度。垂直速度絕不能讓玩家「感覺很快」——否則掉進豎井時 FOV 會被
 * 撐開，玩家只會覺得那是 bug。 */
static inline float hspeed(const Vector3 v) { return sqrtf(v.x * v.x + v.z * v.z); }

static inline float player_height_now(void)
{
    return BP_LERP(PLAYER_HEIGHT, PLAYER_HEIGHT_CROUCH, g_player.crouch_amount);
}

static inline float player_eye_height(void)
{
    return BP_LERP(PLAYER_EYE_HEIGHT, PLAYER_EYE_CROUCH, g_player.crouch_amount);
}

/* 目前水平位置上到底有沒有站起來的空間？要在放開蹲下之前先檢查，否則玩家會
 * 卡進箱子裡，而碰撞解算器會把他們往任意方向彈出去——就是經典的「蹲到簷下
 * 卻被彈飛」bug。 */
static bool has_stand_headroom(void)
{
    Vector3 feet = g_player.pos;
    bp_aabb standing = bp_aabb_make(
        vec3(feet.x - g_player.radius, feet.y, feet.z - g_player.radius),
        vec3(feet.x + g_player.radius, feet.y + PLAYER_HEIGHT + PLAYER_HEAD_CLEAR,
             feet.z + g_player.radius));

    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed || s->shoot_through) continue;
        if (bp_aabb_overlaps(&standing, &s->box)) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------------------- */
/* 初始化 / 重設                                                                             */
/* -------------------------------------------------------------------------------------- */
void bp_player_init(void)
{
    memset(&g_player, 0, sizeof(g_player));
    g_player.max_health     = PLAYER_MAX_HEALTH;
    g_player.max_armor      = PLAYER_MAX_ARMOUR;
    g_player.health         = PLAYER_MAX_HEALTH;
    g_player.radius         = PLAYER_RADIUS;
    g_player.height         = PLAYER_HEIGHT;

    g_player.speed_ground   = PLAYER_SPEED_GROUND;
    g_player.speed_sprint   = PLAYER_SPEED_SPRINT;
    g_player.speed_crouch   = PLAYER_SPEED_CROUCH;
    g_player.speed_air      = PLAYER_SPEED_AIR;
    g_player.accel_ground   = PLAYER_ACCEL_GROUND;
    g_player.accel_air      = PLAYER_ACCEL_AIR;
    g_player.friction_ground= PLAYER_FRICTION_GROUND;
    g_player.jump_velocity  = PLAYER_JUMP_VELOCITY;
    g_player.coyote_time    = 0.0f;
    g_player.jump_buffer    = 0.0f;

    g_player.state          = PST_ALIVE;
    g_player.initialised    = true;
    BP_LOGI("player: initialised (ground %.1f m/s, sprint %.1f, jump %.2f m)",
            (double)PLAYER_SPEED_GROUND, (double)PLAYER_SPEED_SPRINT,
            (double)(PLAYER_JUMP_VELOCITY * PLAYER_JUMP_VELOCITY / (2.0f * -GRAVITY)));
}

void bp_player_reset(Vector3 start, float yaw)
{
    int   keepScore   = g_player.score;
    int   keepKills   = g_player.kills;
    int   keepSecrets = g_player.secrets_found;
    int   keepPickups = g_player.pickups_taken;

    memset(&g_player, 0, sizeof(g_player));
    g_player.max_health      = PLAYER_MAX_HEALTH;
    g_player.max_armor       = PLAYER_MAX_ARMOUR;
    g_player.health          = PLAYER_MAX_HEALTH;
    g_player.armor           = 0.0f;
    g_player.radius          = PLAYER_RADIUS;
    g_player.height          = PLAYER_HEIGHT;

    g_player.speed_ground    = PLAYER_SPEED_GROUND;
    g_player.speed_sprint    = PLAYER_SPEED_SPRINT;
    g_player.speed_crouch    = PLAYER_SPEED_CROUCH;
    g_player.speed_air       = PLAYER_SPEED_AIR;
    g_player.accel_ground    = PLAYER_ACCEL_GROUND;
    g_player.accel_air       = PLAYER_ACCEL_AIR;
    g_player.friction_ground = PLAYER_FRICTION_GROUND;
    g_player.jump_velocity   = PLAYER_JUMP_VELOCITY;

    g_player.pos = start;
    g_player.yaw = yaw;
    g_player.vel = vec3(0, 0, 0);
    g_player.state = PST_ALIVE;
    g_player.invuln_time = PLAYER_INVULN_SPAWN;
    g_player.on_ground = true;
    g_player.initialised = true;

    /* 同一關內的累計成績要活過重生，只有載入關卡才會清空。這正是
     * bp_player_reset 要親手保留它們、而不是信任呼叫端會重新塞值的原因：
     * 死亡絕不該讓玩家賠上自己的分數。 */
    g_player.score          = keepScore;
    g_player.kills          = keepKills;
    g_player.secrets_found  = keepSecrets;
    g_player.pickups_taken  = keepPickups;

    bp_camera_reset(vec3(start.x, start.y + PLAYER_EYE_HEIGHT, start.z));
    bp_camera_set_angles(yaw, 0.0f);
}

/* -------------------------------------------------------------------------------------- */
/* 由輸入推導期望方向                                                                        */
/* -------------------------------------------------------------------------------------- */
/*
 * 移動永遠只以 yaw 為基準。把攝影機朝上仰，絕不能順便把玩家射向天花板——這是
 * 每個人寫自己第一款 FPS 時至少都會犯一次的錯。
 */
static Vector3 compute_wish(float *out_strength, bool *sprinting)
{
    /* bp_move_axis_y 是 +1 前進／-1 後退；x 是 +1 向右／-1 向左。兩者都已把遊戲
     * 手把的類比搖桿與死區（deadzone）折算進去，因此玩法程式碼完全不必碰按鍵。 */
    float fwd = bp_move_axis_y();
    float rgt = bp_move_axis_x();

    float len = sqrtf(fwd * fwd + rgt * rgt);
    if (len > 1.0f) { fwd /= len; rgt /= len; len = 1.0f; }   /* 把數位輸入的斜向正規化 */

    float yawRad = g_player.yaw * BP_DEG2RAD;
    float sx = sinf(yawRad), cz = cosf(yawRad);

    /* 攝影機前向投影到地面平面後是 (sin(yaw), 0, cos(yaw))——但注意符號約定：
     * yaw 為 0 時朝 +Z 看。因此右向是 (cos(yaw), 0, -sin(yaw))。 */
    Vector3 wish = vec3(fwd * sx + rgt * cz, 0.0f, fwd * cz - rgt * sx);

    *out_strength = len;
    *sprinting = bp_down(ACT_SPRINT) && fwd > 0.35f && !g_player.crouched;
    return wish;
}

/* -------------------------------------------------------------------------------------- */
/* 加速度 / 摩擦力                                                                           */
/* -------------------------------------------------------------------------------------- */
static void accelerate(Vector3 wish, float wishSpeed, float accel, float dt)
{
    float current = g_player.vel.x * wish.x + g_player.vel.z * wish.z;   /* 帶號投影量 */
    float add = wishSpeed - current;
    if (add <= 0.0f) return;              /* 沿此軸已達到或超過目標，不必再加速 */

    float accelSpeed = accel * dt * wishSpeed;
    if (accelSpeed > add) accelSpeed = add;

    g_player.vel.x += wish.x * accelSpeed;
    g_player.vel.z += wish.z * accelSpeed;
}

/*
 * 摩擦力，只用來對抗輸入。
 *
 * 沒有按住任何移動鍵時 `inputStrength` 為 0。此時我們把整個水平速度消光（乾淨
 * 停止）。有按住輸入時，只消去與期望方向相反的分量，因此轉向時保留動量，
 * 而不是把速度磨掉。
 */
static void apply_friction(Vector3 wish, float inputStrength, float dt)
{
    float speed = hspeed(g_player.vel);
    if (speed < 0.001f) { g_player.vel.x = 0.0f; g_player.vel.z = 0.0f; return; }

    /* 低於此速度時摩擦改為線性而非指數衰減，否則速度會漸近收斂，玩家會以
     * 0.01 m/s 的速度永遠漂個不停。 */
    const float CONTROL = 1.6f;
    float drop;

    float friction = (inputStrength < 0.01f) ? PLAYER_FRICTION_STOP : PLAYER_FRICTION_GROUND;

    /* 站在低摩擦材質（格柵、危險地面）上時應該要能滑行。 */
    const bp_material_def *md = bp_world_material(bp_world_material_at_point(
        vec3(g_player.pos.x, g_player.pos.y - 0.05f, g_player.pos.z)));
    if (md && md->friction > 0.01f) friction /= md->friction;

    if (inputStrength > 0.01f)
    {
        /* 只處理反向分量：把速度投影到期望軸上，同向的部分原封不動。 */
        float along = g_player.vel.x * wish.x + g_player.vel.z * wish.z;
        if (along >= 0.0f) return;                       /* 輸入同向：不磨減速度 */
        float opposing = -along * inputStrength;
        drop = opposing * friction * dt;
        if (drop > -along) drop = -along;
        /* 沿期望軸把反向的那一部分精確移除 */
        g_player.vel.x += wish.x * drop;
        g_player.vel.z += wish.z * drop;
        return;
    }

    float control = (speed < CONTROL) ? CONTROL : speed;
    drop = control * friction * dt;
    if (drop >= speed) { g_player.vel.x = 0.0f; g_player.vel.z = 0.0f; return; }

    float scale = (speed - drop) / speed;
    g_player.vel.x *= scale;
    g_player.vel.z *= scale;
}

static void clamp_speed(void)
{
    float s = hspeed(g_player.vel);
    if (s > PLAYER_SPEED_HARD_CAP)
    {
        float k = PLAYER_SPEED_HARD_CAP / s;
        g_player.vel.x *= k;
        g_player.vel.z *= k;
    }
}

/* -------------------------------------------------------------------------------------- */
/* 腳步聲                                                                                    */
/* -------------------------------------------------------------------------------------- */
static void update_footsteps(float travelled, int groundMaterial, float dt)
{
    BP_UNUSED(dt);
    if (!g_player.on_ground || travelled < 0.0001f) return;

    float stride = g_player.crouched ? PLAYER_STEP_LEN_CROUCH
               : (hspeed(g_player.vel) > g_player.speed_ground + 0.5f ? 2.75f : PLAYER_STEP_LEN_RUN);

    g_player.step_phase += travelled / stride;
    if (g_player.step_phase < 1.0f) return;
    g_player.step_phase -= floorf(g_player.step_phase);

    const bp_material_def *md = bp_world_material(groundMaterial);
    int sfx = (md && md->step_sfx >= 0) ? md->step_sfx : SFX_FOOTSTEP;

    /* 蹲下的腳步聲較小聲——用聲音回饋告訴玩家：蹲下確實有用。 */
    float vol = g_player.crouched ? 0.28f : BP_CLAMP(hspeed(g_player.vel) / g_player.speed_ground, 0.4f, 1.0f);
    bp_audio_play((bp_sfx_id)sfx, vol);

    /* 腳邊揚起一小撮塵土，比聲音更能把「踩在地面上」賣給玩家，而且成本趨近於零：
     * 一顆粒子、0.25 秒壽命。 */
    if (!g_player.crouched)
    {
        Vector3 foot = vec3(g_player.pos.x, g_player.pos.y + 0.04f, g_player.pos.z);
        bp_fx_particle(PTX_SMOKE, foot,
                       vec3(bp_rng_range(&g_engine.rng, -0.4f, 0.4f),
                            bp_rng_range(&g_engine.rng, 0.15f, 0.55f),
                            bp_rng_range(&g_engine.rng, -0.4f, 0.4f)),
                       0.30f, 0.10f, 0.34f,
                       (Color){ 190, 180, 168, 70 }, (Color){ 190, 180, 168, 0 });
    }
}

/* -------------------------------------------------------------------------------------- */
/* 落地                                                                                      */
/* -------------------------------------------------------------------------------------- */
static void on_land(float impactSpeed, int groundMaterial)
{
    float severity = BP_CLAMP((impactSpeed - 6.0f) / 16.0f, 0.0f, 1.0f);

    if (impactSpeed > 3.0f)
    {
        const bp_material_def *md = bp_world_material(groundMaterial);
        int sfx = (md && md->step_sfx >= 0) ? md->step_sfx : SFX_LAND;
        bp_audio_play((bp_sfx_id)sfx, 0.35f + severity * 0.65f);
    }
    if (impactSpeed > 8.0f) bp_audio_play(SFX_LAND, 0.5f + severity * 0.5f);

    g_player.land_impact = severity;
    if (severity > 0.05f)
    {
        bp_camera_add_dip(severity * 5.5f);
        bp_fx_shake(severity * PLAYER_LAND_SHAKE_MAX, 0.16f + severity * 0.14f);
    }

    /* 重落地時的塵土環：傳達重量感，而不必付出真正衝擊波的成本 */
    if (severity > 0.25f)
    {
        Vector3 at = vec3(g_player.pos.x, g_player.pos.y + 0.05f, g_player.pos.z);
        for (int i = 0; i < (int)(6 + severity * 10.0f); i++)
        {
            float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
            float sp = bp_rng_range(&g_engine.rng, 1.2f, 3.4f) * (0.5f + severity);
            bp_fx_particle(PTX_SMOKE, at, vec3(cosf(a) * sp, bp_rng_range(&g_engine.rng, 0.3f, 1.4f), sinf(a) * sp),
                           bp_rng_range(&g_engine.rng, 0.35f, 0.7f), 0.14f, 0.6f,
                           (Color){ 200, 190, 175, 90 }, (Color){ 200, 190, 175, 0 });
        }
    }

    /* 摔落傷害。放在各種回饋之後處理，讓玩家先看到／聽到落地，血條才往下掉——
     * 這個先後順序是為了讓人讀得出「那是我自己摔的」。 */
    if (impactSpeed > PLAYER_FALL_SAFE_SPEED && g_player.state == PST_ALIVE)
    {
        float dmg = (impactSpeed - PLAYER_FALL_SAFE_SPEED) * PLAYER_FALL_DMG_PER_MS;
        /* 摔落傷害不給護甲吸收：護甲是給槍火用的，若讓它硬吃摔落，護甲就成了
         * 移動能力升級，兩套系統的意義都會被弄混。 */
        bp_player_damage(dmg, g_player.pos, 0.0f);
    }
}

/* -------------------------------------------------------------------------------------- */
/* 觸發器接觸                                                                                */
/* -------------------------------------------------------------------------------------- */
static void pickup_from_trigger(bp_trigger *t)
{
    switch (t->kind)
    {
        case TRIG_HEALTH_SPAWN:
            bp_player_heal(t->param > 0 ? (float)t->param : 25.0f);
            break;
        case TRIG_ARMOR_SPAWN:
            bp_player_add_armor(t->param > 0 ? (float)t->param : 25.0f);
            break;
        case TRIG_AMMO_SPAWN:
        {
            int amount = t->param > 0 ? t->param : 24;
            bp_weapons_add_reserve_all(amount);
            bp_audio_play(SFX_AMMO_PICKUP, 0.9f);
            bp_fx_particle(PTX_FLASH, t->position, vec3(0, 0.4f, 0), 0.20f, 0.3f, 0.9f,
                           g_assets.pal_ammo, (Color){ 240, 208, 92, 0 });
            g_player.pickups_taken++;
            break;
        }
        case TRIG_WEAPON_SPAWN:
        {
            bp_weapon_id wid = (bp_weapon_id)BP_CLAMP(t->param, 0, WPN_ID_COUNT - 1);
            bool firstTime = !bp_weapons_owns(wid);
            int ammo = g_weapon_table[wid].ammo_per_pickup;
            bp_weapons_give(wid, firstTime ? g_weapon_table[wid].mag_size * 2 : ammo, firstTime);
            bp_audio_play(SFX_WEAPON_SWAP, 1.0f);
            bp_fx_shake(0.12f, 0.10f);
            g_player.pickups_taken++;
            if (firstTime)
            {
                /* 新武器：一圈光環加一撮火花，讓玩家絕對不可能錯過。這是唯一
                 * 值得花粒子去表現的拾取物。 */
                bp_fx_particle(PTX_RING, t->position, vec3(0, 0, 0), 0.45f, 0.4f, 3.2f,
                               g_assets.pal_accent, (Color){ 255, 138, 32, 0 });
                for (int i = 0; i < 14; i++)
                {
                    float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
                    bp_fx_particle(PTX_SPARK, t->position,
                                   vec3(cosf(a) * 3.0f, bp_rng_range(&g_engine.rng, 1.0f, 4.0f), sinf(a) * 3.0f),
                                   0.35f, 0.05f, 0.0f, g_assets.pal_accent, (Color){ 255, 220, 150, 0 });
                }
            }
            break;
        }
        case TRIG_EXIT:
            if (!g_player.exit_reached)
            {
                g_player.exit_reached = true;
                g_player.exit_time = g_engine.time_total;
                bp_audio_play(SFX_WAVE_CLEAR, 1.0f);
                BP_LOGI("player: exit reached at %.1fs", (double)g_engine.time_total);
            }
            return;    /* 絕不標記為已消耗：接下來發生什麼由遊戲層決定 */
        case TRIG_SECRET:
            if (!g_player.secret_reached)
            {
                g_player.secret_reached = true;
                g_player.secrets_found++;
                bp_player_add_score(500);
                bp_audio_play(SFX_SECRET, 1.0f);
                bp_fx_particle(PTX_RING, t->position, vec3(0, 0, 0), 0.6f, 0.3f, 4.0f,
                               g_assets.pal_accent2, (Color){ 92, 226, 196, 0 });
                BP_LOGI("player: secret found (%d total)", g_player.secrets_found);
            }
            break;
        case TRIG_DOOR:
            /* 門踏板是近距離持續觸發，不是碰到即消耗：站在踏板上要能不斷重新
             * 聲明「開門」，否則門會在玩家跨步到一半時迎面砸下。 */
            bp_world_activate_doors(t->position, t->radius);
            return;
        default:
            return;
    }

    t->consumed = true;
    t->timer = t->respawn;
}

static void update_triggers(void)
{
    Vector3 centre = vec3(g_player.pos.x, g_player.pos.y + g_player.height * 0.5f, g_player.pos.z);

    for (int i = 0; i < g_world.trigger_count; i++)
    {
        bp_trigger *t = &g_world.triggers[i];
        if (t->consumed) continue;
        if (t->kind == TRIG_PLAYER_START || t->kind == TRIG_ENEMY_SPAWN) continue;

        float r = t->radius > 0.05f ? t->radius : 1.2f;
        float dx = centre.x - t->position.x;
        float dz = centre.z - t->position.z;
        float dy = centre.y - t->position.y;
        if (dx * dx + dz * dz > r * r) continue;
        if (BP_ABS(dy) > BP_MAX(r, 1.8f)) continue;

        pickup_from_trigger(t);
    }
}

/* -------------------------------------------------------------------------------------- */
/* 拾取物（敵人掉落 / 關卡擺放）                                                          */
/* -------------------------------------------------------------------------------------- */
void bp_pickup_clear(void)
{
    for (int i = 0; i < MAX_PICKUPS; i++) g_player.pickups[i].alive = false;
    g_player.pickup_count = 0;
}

void bp_pickup_spawn(bp_pickup_kind kind, Vector3 pos, float amount, int weaponId)
{
    if (kind == PICKUP_NONE) return;

    /* 先重用死掉的欄位，再附加新的，最後才搶最舊的。掉落拾取物絕不能失敗：
     * 打死一隻巨漢（brute）卻什麼都沒掉，感覺就像遊戲把獎勵吞了。 */
    bp_pickup *p = NULL;
    for (int i = 0; i < MAX_PICKUPS; i++)
        if (!g_player.pickups[i].alive) { p = &g_player.pickups[i]; break; }

    if (!p)
    {
        int oldest = 0;
        float best = 1e30f;
        for (int i = 0; i < MAX_PICKUPS; i++)
            if (g_player.pickups[i].age > g_player.pickups[i].respawn_at && g_player.pickups[i].age < best)
            { best = g_player.pickups[i].age; oldest = i; }
        p = &g_player.pickups[oldest];
    }

    memset(p, 0, sizeof(*p));
    p->pos = pos;
    p->kind = kind;
    p->amount = amount;
    p->weapon_id = weaponId;
    p->alive = true;
    p->spinning = true;
    p->age = 0.0f;
    p->bob = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);   /* 讓各次掉落的浮動相位彼此錯開 */
    g_player.pickup_count = BP_MIN(g_player.pickup_count + 1, MAX_PICKUPS);
}

/* 這個拾取物現在真的用得上嗎？回傳 false 表示把它留在原地而不吃掉——現在不需要
 * 的補血包，正是之後會慶幸還留在地上的補血包；而自動吃掉它，是整個類型裡公認
 * 最惹人厭的拾取物行為。 */
static bool pickup_is_useful(const bp_pickup *p)
{
    switch (p->kind)
    {
        case PICKUP_HEALTH: return g_player.health < g_player.max_health - 0.5f;
        case PICKUP_ARMOR:  return g_player.armor  < g_player.max_armor  - 0.5f;
        case PICKUP_AMMO:   return true;   /* 備彈上限由武器庫那一端負責強制 */
        case PICKUP_WEAPON: return true;
        default: return false;
    }
}

static void collect_pickup(bp_pickup *p)
{
    switch (p->kind)
    {
        case PICKUP_HEALTH:
            bp_player_heal(p->amount);
            break;
        case PICKUP_ARMOR:
            bp_player_add_armor(p->amount);
            break;
        case PICKUP_AMMO:
            bp_weapons_add_reserve_all((int)p->amount);
            bp_audio_play(SFX_AMMO_PICKUP, 0.8f);
            break;
        case PICKUP_WEAPON:
        {
            bp_weapon_id wid = (bp_weapon_id)BP_CLAMP(p->weapon_id, 0, WPN_ID_COUNT - 1);
            bool firstTime = !bp_weapons_owns(wid);
            bp_weapons_give(wid, firstTime ? g_weapon_table[wid].mag_size * 2 : (int)p->amount, firstTime);
            bp_audio_play(SFX_WEAPON_SWAP, 1.0f);
            break;
        }
        default: return;
    }

    g_player.pickups_taken++;
    p->alive = false;

    bp_fx_particle(PTX_FLASH, p->pos, vec3(0, 0.5f, 0), 0.18f, 0.25f, 0.8f,
                   (Color){ 255, 255, 255, 220 }, (Color){ 255, 255, 255, 0 });
}

void bp_pickup_update(float dt)
{
    int live = 0;
    Vector3 centre = vec3(g_player.pos.x, g_player.pos.y + g_player.height * 0.4f, g_player.pos.z);

    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;
        live++;

        p->age += dt;
        p->bob += dt;

        /* 拾取物會停在自己腳下實際的地板上，因此從空中走道掉落的戰利品會真的
         * 落地，而不是懸浮在被擊殺當下的高度。 */
        float floorY = bp_world_floor_height(p->pos, 0.25f, p->pos.y + 0.05f);
        if (p->pos.y > floorY + 0.001f) p->pos.y = BP_MAX(floorY, p->pos.y - 6.0f * dt);
        else p->pos.y = floorY;

        if (g_player.state != PST_ALIVE) continue;

        float dx = centre.x - p->pos.x;
        float dz = centre.z - p->pos.z;
        float dy = centre.y - p->pos.y;
        if (dx * dx + dz * dz > PICKUP_RADIUS * PICKUP_RADIUS) continue;
        if (BP_ABS(dy) > PICKUP_HEIGHT_TOL) continue;

        if (pickup_is_useful(p)) collect_pickup(p);
    }
    g_player.pickup_count = live;
}

/*
 * 拾取物以公告板（billboard）繪製，而不是用它們的 3D 模型。
 *
 * 原因：模型確實存在（MDL_PICKUP_*），世界層也拿它們做靜態擺放，但 40 m 外一個
 * 旋轉中的 3D 模型看起來只是一粒小點；換成帶有亮核心的彩色公告板，任何距離都
 * 一眼可辨，而且全部加起來只要一次 draw call。競技場射擊遊戲的生死，就繫於
 * 「亂戰之中我到底看不看得到那個補血包」。
 */
void bp_pickup_draw(Camera3D cam, Vector3 viewer)
{
    BP_UNUSED(cam);
    Texture2D soft = bp_tex(TEX_PARTICLE_SOFT);
    Texture2D ring = bp_tex(TEX_RING);
    if (!soft.id) return;

    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;

        float dist = Vector3Distance(viewer, p->pos);
        if (dist > 90.0f) continue;

        Color c;
        float size;
        switch (p->kind)
        {
            case PICKUP_HEALTH: c = g_assets.pal_health; size = 0.42f; break;
            case PICKUP_ARMOR:  c = g_assets.pal_armor;  size = 0.42f; break;
            case PICKUP_AMMO:   c = g_assets.pal_ammo;   size = 0.36f; break;
            case PICKUP_WEAPON: c = g_assets.pal_accent; size = 0.52f; break;
            default: continue;
        }

        /* 前 0.25 秒淡入，讓掉落物是「冒出來」而不是瞬間閃現 */
        float appear = BP_CLAMP(p->age * 4.0f, 0.0f, 1.0f);
        float bobY = sinf(p->bob * 2.6f) * PICKUP_BOB_HEIGHT;
        Vector3 at = vec3(p->pos.x, p->pos.y + 0.42f + bobY, p->pos.z);

        /* 隨距離淡出，避免遠處拾取物與關卡幾何相互閃爍 */
        float farFade = BP_CLAMP((90.0f - dist) / 25.0f, 0.0f, 1.0f);
        int alpha = (int)(230.0f * appear * farFade);
        if (alpha <= 0) continue;

        Color glow = c; glow.a = (unsigned char)BP_CLAMP((int)(alpha * 0.55f), 0, 255);
        Color core = c; core.a = (unsigned char)alpha;

        rlSetBlendMode(RL_BLEND_ADDITIVE);
        DrawBillboard(cam, ring, at, size * 2.1f, glow);
        rlSetBlendMode(RL_BLEND_ALPHA);
        DrawBillboard(cam, soft, at, size, core);

        /* 武器拾取物再給一個更高的小光標：玩家肯為了它橫穿整場槍戰，所以它必須
         * 連眼角餘光都定位得到。 */
        if (p->kind == PICKUP_WEAPON)
        {
            rlSetBlendMode(RL_BLEND_ADDITIVE);
            DrawBillboard(cam, soft, vec3(at.x, at.y + 0.75f, at.z), size * 0.5f, glow);
            rlSetBlendMode(RL_BLEND_ALPHA);
        }
    }
}

/* -------------------------------------------------------------------------------------- */
/* 生命狀態                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_player_heal(float amount)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    float before = g_player.health;
    g_player.health = BP_MIN(g_player.health + amount, g_player.max_health);
    if (g_player.health > before + 0.01f)
    {
        g_player.heal_flash = 1.0f;
        bp_audio_play(SFX_HEAL, 0.85f);
        bp_fx_particle(PTX_RING, vec3(g_player.pos.x, g_player.pos.y + 0.2f, g_player.pos.z),
                       vec3(0, 0, 0), 0.35f, 0.3f, 1.8f,
                       g_assets.pal_health, (Color){ 86, 214, 110, 0 });
    }
}

void bp_player_add_armor(float amount)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    float before = g_player.armor;
    g_player.armor = BP_MIN(g_player.armor + amount, g_player.max_armor);
    if (g_player.armor > before + 0.01f)
    {
        bp_audio_play(SFX_ARMOR, 0.85f);
        g_player.heal_flash = BP_MAX(g_player.heal_flash, 0.5f);
    }
}

void bp_player_damage(float amount, Vector3 sourcePos, float armorAbsorb)
{
    if (amount <= 0.0f || g_player.state != PST_ALIVE) return;
    if (g_player.invuln_time > 0.0f) return;

    /* 護甲先分擔一部分傷害，之後才耗損。armorAbsorb 是允許護甲吃下這一擊的比例
     * （摔落為 0，槍擊約 0.65）。把傷害「拆分」而不是整段擋下，代表護甲永遠有
     * 存在感，卻永遠不會讓玩家刀槍不入——滿甲照樣會被巨漢狠狠咬走一大口。 */
    float absorbed = 0.0f;
    if (g_player.armor > 0.0f && armorAbsorb > 0.0f)
    {
        absorbed = BP_MIN(g_player.armor, amount * armorAbsorb);
        g_player.armor -= absorbed;
        amount -= absorbed;
    }

    g_player.health -= amount;
    g_player.damage_flash = BP_MIN(1.0f, g_player.damage_flash + BP_CLAMP(amount / 40.0f, 0.18f, 1.0f));
    g_player.last_damage_source = sourcePos;
    g_player.last_damage_time = g_engine.time_total;

    /* 以視角為基準記下受擊方向，HUD 指示器才能指向開槍的人。對（與右向的點積、
     * 與前向的點積）取 atan2，就得到地面平面上帶正負號的角度。 */
    Vector3 toSrc = Vector3Subtract(sourcePos, g_player.pos);
    toSrc.y = 0.0f;
    if (Vector3LengthSqr(toSrc) > 0.01f)
    {
        toSrc = Vector3Scale(toSrc, 1.0f / Vector3Length(toSrc));
        float yawRad = g_player.yaw * BP_DEG2RAD;
        Vector3 fwd = vec3(sinf(yawRad), 0.0f, cosf(yawRad));
        Vector3 rgt = vec3(cosf(yawRad), 0.0f, -sinf(yawRad));
        g_player.last_damage_dir = atan2f(Vector3DotProduct(toSrc, rgt), Vector3DotProduct(toSrc, fwd));
    }

    bp_audio_play(absorbed > amount ? SFX_HIT_ARMOR : SFX_HURT,
                  BP_CLAMP(0.5f + (absorbed + amount) / 60.0f, 0.5f, 1.0f));
    bp_camera_add_roll((g_player.last_damage_dir > 0.0f ? -1.0f : 1.0f) *
                       BP_CLAMP((absorbed + amount) / 55.0f, 0.0f, 2.6f));
    bp_fx_shake(BP_CLAMP((absorbed + amount) / 90.0f, 0.05f, 0.4f), 0.14f);

    /* 中彈絕不該讓你保有完美準度：一個隨這一擊強度縮放的短暫操作懲罰，才能讓
     * 對槍是一個抉擇，而不是穩賺不賠的免費生意。 */
    bp_camera_add_dip(BP_CLAMP(amount / 30.0f, 0.0f, 2.0f));

    if (g_player.health <= 0.0f)
    {
        g_player.health = 0.0f;
        g_player.state = PST_DYING;
        g_player.death_time = 0.0f;
        g_player.streak = 0;
        /* 死亡是整個遊戲唯一可以放肆大聲的時刻：爆炸級的重音，加上低血量警示
         * 循環戛然而止。音量 1.0 在此是刻意安排。 */
        bp_audio_play(SFX_EXPLOSION, 1.0f);
        bp_fx_shake(0.9f, 0.9f);
        BP_LOGI("player: died at %.1fs (score %d, %d kills)",
                (double)g_player.time_alive, g_player.score, g_player.kills);
    }
}

void bp_player_impulse(Vector3 impulse)
{
    g_player.vel.x += impulse.x;
    g_player.vel.y += impulse.y;
    g_player.vel.z += impulse.z;
    /* 任何向上的衝量都代表我們已經離地；先清掉 on_ground，摩擦力程式碼才不會在
     * 衝量發生的同一個 tick 裡就把這次彈射磨掉。 */
    if (impulse.y > 0.01f) g_player.on_ground = false;
    clamp_speed();
}

void bp_player_add_score(int amount)
{
    if (amount <= 0) return;
    /* 連殺倍率：在 PLAYER_STREAK_WINDOW 內連續擊殺，倍率最高疊到 2 倍。這正是
     * 計分系統存在的全部理由——它獎勵激進玩法，而那正是 boomer shooter 想鼓勵
     * 的行為。 */
    int mult = 1 + BP_MIN(g_player.streak, PLAYER_STREAK_MAX) / 4;
    g_player.score += amount * mult;
}

void bp_player_register_kill(int scoreValue)
{
    g_player.kills++;
    if (g_player.streak_time > 0.0f) g_player.streak++;
    else g_player.streak = 1;
    g_player.streak_time = PLAYER_STREAK_WINDOW;
    bp_player_add_score(scoreValue > 0 ? scoreValue : 100);
}

/* -------------------------------------------------------------------------------------- */
/* 存取函式                                                                               */
/* -------------------------------------------------------------------------------------- */
Vector3 bp_player_position(void) { return g_player.pos; }
Vector3 bp_player_eye(void)      { return vec3(g_player.pos.x, g_player.pos.y + player_eye_height(), g_player.pos.z); }
Vector3 bp_player_velocity(void) { return g_player.vel; }
bool    bp_player_alive(void)    { return g_player.state == PST_ALIVE; }
bool    bp_player_on_ground(void){ return g_player.on_ground; }

float bp_player_speed01(void)
{
    /* 以衝刺上限而非步行上限做正規化：HUD 與 FOV 加成應該都在玩家合法能達到的
     * 最快速度處剛好飽和。 */
    return BP_CLAMP(hspeed(g_player.vel) / g_player.speed_sprint, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------- */
/* 主要更新                                                                               */
/* -------------------------------------------------------------------------------------- */
void bp_player_update(float dt)
{
    if (!g_player.initialised || dt <= 0.0f) return;

    /* ---- 每個狀態都會推進的計時器 ---- */
    if (g_player.invuln_time > 0.0f)  g_player.invuln_time  -= dt;
    if (g_player.streak_time > 0.0f)  { g_player.streak_time -= dt; if (g_player.streak_time <= 0.0f) g_player.streak = 0; }
    g_player.damage_flash = BP_MAX(0.0f, g_player.damage_flash - dt * 2.6f);
    g_player.heal_flash   = BP_MAX(0.0f, g_player.heal_flash   - dt * 2.0f);
    g_player.land_impact  = BP_MAX(0.0f, g_player.land_impact  - dt * 4.0f);

    /* ---- 視角 ---- */
    if (g_player.state == PST_ALIVE)
    {
        /* 靈敏度在攝影機內部施加，滑鼠與手把才能共用同一條響應曲線。 */
        bp_camera_look(bp_input_look_dx(), bp_input_look_dy(), g_input.sensitivity);
        g_player.yaw = g_camera.yaw;
    }

    /* ---- 死亡 / 重生時序 ---- */
    if (g_player.state == PST_DYING)
    {
        g_player.death_time += dt;
        /* 讓速度逐步消散，屍體會就地安穩下來，而不是一路滑出平台邊。 */
        g_player.vel.x *= BP_MAX(0.0f, 1.0f - dt * 6.0f);
        g_player.vel.z *= BP_MAX(0.0f, 1.0f - dt * 6.0f);
        g_player.vel.y += GRAVITY * dt;
        g_player.pos = Vector3Add(g_player.pos, Vector3Scale(g_player.vel, dt));
        bool grounded = false;
        int mat = MAT_CONCRETE;
        bp_world_resolve_body(&g_player.pos, g_player.radius, player_height_now(),
                              &g_player.vel, &grounded, &mat);
        if (g_player.death_time >= PLAYER_DEATH_DURATION) g_player.state = PST_DEAD;
        bp_camera_simulate(dt, g_player.pos, 0.0f, grounded, false, 0.0f);
        return;
    }
    if (g_player.state != PST_ALIVE)
    {
        /* DEAD / RESPAWNING 的主導權在遊戲層手上（何時重生由它決定）。這裡仍然
         * 推進攝影機，死亡鏡頭才會持續漂移，而不是當場凍結。 */
        bp_camera_simulate(dt, g_player.pos, 0.0f, g_player.on_ground, g_player.crouched, 0.0f);
        return;
    }

    g_player.time_alive += dt;

    /* ---- 蹲下 ---- */
    g_player.crouch_held = bp_down(ACT_CROUCH);
    if (g_player.crouch_held) g_player.crouch_amount = BP_MIN(1.0f, g_player.crouch_amount + dt * PLAYER_CROUCH_SPEED);
    else if (has_stand_headroom()) g_player.crouch_amount = BP_MAX(0.0f, g_player.crouch_amount - dt * PLAYER_CROUCH_SPEED);
    /* 其他情況：維持蹲姿——在簷下放開按鍵，絕不能讓我們就地起身、一頭撞上去 */
    g_player.crouched = g_player.crouch_amount > 0.6f;
    g_player.height = player_height_now();

    /* ---- 期望方向 ---- */
    float inputStrength = 0.0f;
    bool sprinting = false;
    Vector3 wish = compute_wish(&inputStrength, &sprinting);

    float targetSpeed = g_player.crouched ? g_player.speed_crouch
                      : (sprinting ? g_player.speed_sprint : g_player.speed_ground);
    g_player.speed_current = targetSpeed;

    /* ---- 跳躍 ---- */
    if (g_player.coyote_time > 0.0f) g_player.coyote_time -= dt;
    if (g_player.jump_buffer > 0.0f) g_player.jump_buffer -= dt;

    if (bp_pressed(ACT_JUMP)) g_player.jump_buffer = PLAYER_JUMP_BUFFER;

    /* 可變跳躍高度：提早放開就會削減上升速度。少了這段，每次跳躍高度都一樣，
     * 跳台玩法會死板得像按販賣機按鈕。 */
    if (!bp_down(ACT_JUMP) && g_player.vel.y > 0.0f && g_player.jump_held)
        g_player.vel.y *= BP_MAX(0.0f, 1.0f - dt * 7.5f);
    g_player.jump_held = bp_down(ACT_JUMP);

    if (g_player.jump_buffer > 0.0f && (g_player.on_ground || g_player.coyote_time > 0.0f))
    {
        g_player.vel.y = g_player.jump_velocity;
        g_player.on_ground = false;
        g_player.coyote_time = 0.0f;
        g_player.jump_buffer = 0.0f;
        bp_audio_play(SFX_JUMP, 0.55f);
        /* 沿期望方向稍微加一點前進助力：朝敵人跳應該像一次突進，而不是像移動
         * 系統整整 0.2 秒沒在聽你說話。 */
        if (inputStrength > 0.1f)
        {
            g_player.vel.x += wish.x * targetSpeed * 0.18f;
            g_player.vel.z += wish.z * targetSpeed * 0.18f;
        }
        clamp_speed();
    }

    /* ---- 水平移動 ---- */
    bool wasGrounded = g_player.on_ground;
    if (g_player.on_ground)
    {
        apply_friction(wish, inputStrength, dt);
        accelerate(wish, targetSpeed * inputStrength, g_player.accel_ground, dt);
    }
    else
    {
        /* 空中轉向用小得多的上限（PLAYER_SPEED_AIR），但投影技巧相同——這正是
         * 讓動量能撐過整個跳躍、同時仍容許空中修正的關鍵。這個不對稱性，是手感
         * 上最重要的一行程式碼。 */
        accelerate(wish, g_player.speed_air * inputStrength, g_player.accel_air * 6.0f, dt);
    }
    clamp_speed();

    /* ---- 重力 + 位置積分 ---- */
    g_player.vel.y += GRAVITY * dt;
    if (g_player.vel.y < -60.0f) g_player.vel.y = -60.0f;    /* 終端速度：避免一個 tick 內高速穿隧（tunneling）穿越薄地板 */

    Vector3 prev = g_player.pos;
    g_player.pos = Vector3Add(g_player.pos, Vector3Scale(g_player.vel, dt));

    /* ---- 碰撞 ---- */
    float impactSpeed = -g_player.vel.y;
    bool grounded = false;
    int groundMaterial = MAT_CONCRETE;
    bp_world_resolve_body(&g_player.pos, g_player.radius, g_player.height,
                          &g_player.vel, &grounded, &groundMaterial);

    /* 解算器在落地時會把 velocity.y 歸零，所以落地速度必須在它執行「之前」先
     * 擷取——這就是上面 impactSpeed 先從解析前的值取樣的原因。 */
    if (grounded && !wasGrounded && impactSpeed > 1.0f) on_land(impactSpeed, groundMaterial);
    g_player.on_ground = grounded;
    g_player.coyote_time = grounded ? PLAYER_COYOTE_TIME : g_player.coyote_time;
    if (!grounded) g_player.air_time += dt; else g_player.air_time = 0.0f;

    /* ---- 速度統計 ---- */
    float hs = hspeed(g_player.vel);
    g_player.speed_peak = BP_MAX(g_player.speed_peak, hs);
    if (-g_player.vel.y > g_player.fall_speed_peak) g_player.fall_speed_peak = -g_player.vel.y;

    /* 超過步行上限的速度是這款遊戲的核心表達，因此它配得上回饋：一陣拖行粒子，
     * 加上（透過 bp_camera_simulate）更寬的 FOV。 */
    if (hs > g_player.speed_sprint + 1.5f && grounded && bp_rng_int(&g_engine.rng, 3) == 0)
        bp_fx_speed_burst(vec3(g_player.pos.x, g_player.pos.y + 0.3f, g_player.pos.z),
                          vec3(-g_player.vel.x, 0.0f, -g_player.vel.z));

    /* ---- 腳步聲 ---- */
    float travelled = Vector3Distance(prev, g_player.pos);
    /* 忽略移動量裡的垂直分量：落地絕不能算成一步。 */
    if (BP_ABS(g_player.pos.y - prev.y) > travelled * 0.5f) travelled *= 0.5f;
    update_footsteps(travelled, groundMaterial, dt);

    /* ---- 危險地面 ---- */
    const bp_material_def *gmd = bp_world_material(groundMaterial);
    if (grounded && gmd && gmd->damage_to_player > 0.01f)
    {
        g_player.hazard_time += dt;
        if (g_player.hazard_time >= PLAYER_HAZARD_TICK)
        {
            g_player.hazard_time -= PLAYER_HAZARD_TICK;
            bp_player_damage(gmd->damage_to_player * PLAYER_HAZARD_TICK, g_player.pos, 0.0f);
            bp_fx_particle(PTX_EMBER, vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                           vec3(bp_rng_range(&g_engine.rng, -0.6f, 0.6f), 1.4f,
                                bp_rng_range(&g_engine.rng, -0.6f, 0.6f)),
                           0.4f, 0.09f, 0.0f, g_assets.pal_danger, (Color){ 232, 52, 60, 0 });
        }
    }
    else g_player.hazard_time = 0.0f;

    /* ---- 低血量警示 ---- */
    if (g_player.health > 0.0f && g_player.health < PLAYER_LOW_HP_THRESH)
    {
        g_player.low_health_beep -= dt;
        if (g_player.low_health_beep <= 0.0f)
        {
            /* 血量越低叫得越急：純聽覺的緊迫感爬升，就算玩家沒在看 HUD 也有效。 */
            g_player.low_health_beep = PLAYER_LOW_HP_INTERVAL * BP_CLAMP(g_player.health / PLAYER_LOW_HP_THRESH, 0.35f, 1.0f);
            bp_audio_play(SFX_LOW_HEALTH, 0.55f);
        }
    }
    else g_player.low_health_beep = 0.0f;

    /* ---- 與世界互動 ---- */
    update_triggers();
    bp_pickup_update(dt);

    /* ---- 攝影機 ----
     * damageRoll 會把視角推離最後一次受擊的方向。2 度以內、100 毫秒以下：
     * 感覺得到，卻永遠不至於干擾瞄準。 */
    float damageRoll = g_player.damage_flash * 1.6f * (g_player.last_damage_dir > 0.0f ? -1.0f : 1.0f);
    bp_camera_simulate(dt, g_player.pos, bp_player_speed01(), g_player.on_ground,
                       g_player.crouched, damageRoll);
}

/* -------------------------------------------------------------------------------------- */
/* 除錯繪製                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_player_draw_debug(void)
{
    float h = player_height_now();
    bp_aabb body = bp_aabb_make(
        vec3(g_player.pos.x - g_player.radius, g_player.pos.y, g_player.pos.z - g_player.radius),
        vec3(g_player.pos.x + g_player.radius, g_player.pos.y + h, g_player.pos.z + g_player.radius));

    Color boxCol = g_player.on_ground ? (Color){ 92, 226, 196, 200 } : (Color){ 240, 208, 92, 200 };
    bp_gfx_draw_aabb(body.min, body.max, boxCol);

    /* 眼睛位置 + 視線錐 */
    Vector3 eye = bp_player_eye();
    DrawSphere(eye, 0.05f, MAGENTA);
    bp_gfx_draw_ray(eye, Vector3Add(eye, Vector3Scale(bp_camera_forward(), 3.0f)), (Color){ 255, 138, 32, 220 });
    bp_gfx_draw_ray(eye, Vector3Add(eye, Vector3Scale(bp_camera_right(), 1.2f)), (Color){ 92, 226, 196, 180 });

    /* 速度向量，縮放比例為 1 m 代表 1 m/s */
    bp_gfx_draw_ray(vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                    Vector3Add(vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z), g_player.vel),
                    (Color){ 236, 238, 242, 200 });

    /* 拾取判定體積 */
    for (int i = 0; i < MAX_PICKUPS; i++)
    {
        const bp_pickup *p = &g_player.pickups[i];
        if (!p->alive) continue;
        bp_aabb pb = bp_aabb_make(vec3(p->pos.x - PICKUP_RADIUS, p->pos.y, p->pos.z - PICKUP_RADIUS),
                                  vec3(p->pos.x + PICKUP_RADIUS, p->pos.y + PICKUP_HEIGHT_TOL, p->pos.z + PICKUP_RADIUS));
        bp_gfx_draw_aabb(pb.min, pb.max, (Color){ 86, 214, 110, 90 });
    }
}
