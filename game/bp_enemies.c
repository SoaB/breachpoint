/******************************************************************************************
 * bp_enemies.c -- 敵人定義、池位管理、AI 感知／導航／攻擊，以及程序化骨骼繪製。
 *
 * 為什麼是程序化模型
 *   四種原型的身體都由零件（腿、軀幹、手臂、頭）在執行期組裝，而不是載入美術資產。這讓
 *   動作能直接由模擬狀態驅動 —— 步態相位跟著地面速度走、攻擊前搖對應 attack_wind、死亡
 *   傾倒對應 die_timer —— 也因此換皮只需要改顏色與 scale，不必重做動畫。
 *
 * 為什麼敵人用 handle（世代索引）而不是裸指標
 *   敵人會在池中頻繁生滅，任何持有者都可能握著一個已被回收的位置。handle 帶著世代
 *   （generation），過期的 handle 會解析成 NULL，而不是悄悄改到另一個新敵人身上。
 *
 * AI 的取捨
 *   感知、導航與攻擊都是刻意的近似：視野錐 + 射線可視性、三根鬚毛（whisker）的避障、以
 *   分離向量避免擠成一團。沒有尋路（pathfinding），因為競技場的可走區域本來就大而連通；
 *   換來的是每一隻敵人的成本低到可以同屏塞二十隻。
 ******************************************************************************************/
#include "bp_enemies.h"
#include "bp_world.h"
#include "bp_player.h"
#include "bp_fx.h"
#include "bp_hitdetect.h"
#include "bp_weapons.h"
#include "bp_waves.h"
#include "../engine/bp_config.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "../engine/bp_gfx.h"

bp_enemy_sys g_enemies = { 0 };

/* ====================================================================================== */
/* 1. 編譯期預設值（assets/cfg/enemies.cfg 缺失時的回退值）                                  */
/* ====================================================================================== */
static bp_enemy_def s_defaults[ET_COUNT] = {
    {
        "grunt", "SCRAPHOUND", 62.0f, 0.0f, 3.4f, 6.6f, 0.46f, 1.82f, 1.00f,
        62.0f, 26.0f, 2.30f, 13.0f, 0.85f, SFX_GRUNT_SIGHT, 0.55f, 100,
        0.18f, 0.42f, 0.05f, { 168, 92, 76, 255 }, { 118, 58, 50, 255 }, { 255, 207, 58, 255 },
        0.35f, 0.80f, false, 0, 0, 0, false, 1.0f, 0,
        SFX_GRUNT_SIGHT, SFX_GRUNT_PAIN, SFX_GRUNT_DIE
    },
    {
        "hound", "RIPPER", 38.0f, 0.0f, 5.2f, 11.4f, 0.40f, 1.05f, 1.00f,
        70.0f, 34.0f, 1.95f, 9.0f, 0.52f, SFX_HOUND_ATTACK, 0.30f, 75,
        0.10f, 0.30f, 0.02f, { 92, 104, 70, 255 }, { 58, 66, 48, 255 }, { 255, 74, 58, 255 },
        0.75f, 1.00f, false, 0, 0, 0, false, 1.0f, 1,
        SFX_HOUND_SIGHT, SFX_HOUND_SIGHT, SFX_HOUND_DIE
    },
    {
        "brute", "SIEGEBREAKER", 340.0f, 60.0f, 2.1f, 4.3f, 0.92f, 2.85f, 1.85f,
        75.0f, 40.0f, 3.40f, 26.0f, 1.35f, SFX_BRUTE_ROAR, 0.12f, 500,
        0.55f, 0.85f, 0.35f, { 122, 68, 62, 255 }, { 74, 42, 40, 255 }, { 255, 42, 42, 255 },
        0.15f, 0.95f, false, 0, 0, 0, false, 0.22f, 2,
        SFX_BRUTE_ROAR, SFX_BRUTE_ROAR, SFX_BRUTE_DIE
    },
    {
        "turret", "SENTRY NODE", 120.0f, 45.0f, 0.0f, 0.0f, 0.52f, 1.55f, 1.00f,
        85.0f, 0.0f, 55.0f, 8.0f, 0.26f, SFX_TURRET_FIRE, 0.0f, 250,
        0.25f, 0.70f, 0.15f, { 128, 134, 142, 255 }, { 58, 60, 68, 255 }, { 255, 58, 42, 255 },
        0.0f, 1.00f, true, 30.0f, 0.11f, 8.0f, true, 1.0f, 3,
        SFX_TURRET_FIRE, SFX_TURRET_FIRE, SFX_TURRET_DIE
    },
};

const bp_enemy_def *g_enemy_table = s_defaults;

bp_enemy_type bp_enemy_type_from_cfg(const char *id)
{
    for (int i = 0; i < ET_COUNT; i++) if (!strcmp(s_defaults[i].cfg_id, id)) return (bp_enemy_type)i;
    return ET_GRUNT;
}

bool bp_enemies_load_config(const char *path)
{
    bp_config cfg;
    if (!bp_config_load(&cfg, path))
    {
        BP_LOGW("enemies: '%s' missing -- using compiled-in defaults", path);
        return false;
    }

    memcpy(g_enemies.def, s_defaults, sizeof(s_defaults));
    int patched = 0;

    while (bp_config_next_section(&cfg))
    {
        if (strcmp(bp_config_section_type(&cfg), "enemy") != 0) continue;
        bp_enemy_type t = bp_enemy_type_from_cfg(bp_config_section_id(&cfg));
        bp_enemy_def *d = &g_enemies.def[t];

        while (bp_config_next_key(&cfg))
        {
            const char *k = cfg.key;
            if      (!strcmp(k, "display_name"))    snprintf(d->display_name, sizeof(d->display_name), "%s", bp_cfg_str(&cfg, d->display_name));
            else if (!strcmp(k, "health"))          d->health = bp_cfg_float(&cfg, d->health);
            else if (!strcmp(k, "armor"))           d->armor = bp_cfg_float(&cfg, d->armor);
            else if (!strcmp(k, "speed_walk"))      d->speed_walk = bp_cfg_float(&cfg, d->speed_walk);
            else if (!strcmp(k, "speed_run"))       d->speed_run = bp_cfg_float(&cfg, d->speed_run);
            else if (!strcmp(k, "radius"))          d->radius = bp_cfg_float(&cfg, d->radius);
            else if (!strcmp(k, "height"))          d->height = bp_cfg_float(&cfg, d->height);
            else if (!strcmp(k, "scale"))           d->scale = bp_cfg_float(&cfg, d->scale);
            else if (!strcmp(k, "sight_range"))     d->sight_range = bp_cfg_float(&cfg, d->sight_range);
            else if (!strcmp(k, "hear_range"))      d->hear_range = bp_cfg_float(&cfg, d->hear_range);
            else if (!strcmp(k, "attack_range"))    d->attack_range = bp_cfg_float(&cfg, d->attack_range);
            else if (!strcmp(k, "attack_damage"))   d->attack_damage = bp_cfg_float(&cfg, d->attack_damage);
            else if (!strcmp(k, "attack_cooldown")) d->attack_cooldown = bp_cfg_float(&cfg, d->attack_cooldown);
            else if (!strcmp(k, "attack_sfx"))      d->attack_sfx = bp_cfg_int(&cfg, d->attack_sfx);
            else if (!strcmp(k, "pain_chance"))     d->pain_chance = bp_cfg_float(&cfg, d->pain_chance);
            else if (!strcmp(k, "score"))           d->score = bp_cfg_int(&cfg, d->score);
            else if (!strcmp(k, "drop_health"))     d->drop_health = bp_cfg_float(&cfg, d->drop_health);
            else if (!strcmp(k, "drop_ammo"))       d->drop_ammo = bp_cfg_float(&cfg, d->drop_ammo);
            else if (!strcmp(k, "drop_armor"))      d->drop_armor = bp_cfg_float(&cfg, d->drop_armor);
            else if (!strcmp(k, "color_body"))      d->color_body = bp_cfg_color(&cfg, d->color_body);
            else if (!strcmp(k, "color_trim"))      d->color_trim = bp_cfg_color(&cfg, d->color_trim);
            else if (!strcmp(k, "eye_color"))       d->eye_color = bp_cfg_color(&cfg, d->eye_color);
            else if (!strcmp(k, "strafe"))          d->strafe = bp_cfg_float(&cfg, d->strafe);
            else if (!strcmp(k, "aggression"))      d->aggression = bp_cfg_float(&cfg, d->aggression);
            else if (!strcmp(k, "projectile"))      d->projectile = bp_cfg_bool(&cfg, d->projectile);
            else if (!strcmp(k, "proj_speed"))      d->proj_speed = bp_cfg_float(&cfg, d->proj_speed);
            else if (!strcmp(k, "proj_radius"))     d->proj_radius = bp_cfg_float(&cfg, d->proj_radius);
            else if (!strcmp(k, "proj_damage"))     d->proj_damage = bp_cfg_float(&cfg, d->proj_damage);
            else if (!strcmp(k, "static"))          d->is_static = bp_cfg_bool(&cfg, d->is_static);
            else if (!strcmp(k, "knockback_scale")) d->knockback_scale = bp_cfg_float(&cfg, d->knockback_scale);
            else if (!strcmp(k, "body"))
            {
                const char *b = bp_cfg_str(&cfg, "torso");
                d->body_set = !strcmp(b, "hound") ? 1 : !strcmp(b, "brute") ? 2
                            : !strcmp(b, "turret") ? 3 : 0;
            }
            else BP_LOGD("enemies: ignoring unknown key '%s'", k);
        }
        patched++;
    }

    g_enemies.def_loaded = true;
    g_enemy_table = g_enemies.def;
    BP_LOGI("enemies: %d/%d archetypes patched from config", patched, ET_COUNT);
    return patched > 0;
}

/* ====================================================================================== */
/* 2. 生命週期                                                                               */
/* ====================================================================================== */
void bp_enemies_init(void)
{
    memcpy(g_enemies.def, s_defaults, sizeof(s_defaults));
    g_enemy_table = g_enemies.def;
    bp_enemies_load_config("assets/cfg/enemies.cfg");

    bp_pool_init(&g_enemies.pool, "enemy", g_enemies.storage, g_enemies.generations,
                 g_enemies.alive_flags, g_enemies.free_stack, g_enemies.iter_list,
                 MAX_ENEMIES, sizeof(bp_enemy));
    g_enemies.initialised = true;
    bp_enemies_clear();
    BP_LOGI("enemies: system online (pool %d)", MAX_ENEMIES);
}

void bp_enemies_clear(void)
{
    bp_pool_reset(&g_enemies.pool);
    g_enemies.alive_count = 0;
    g_enemies.killed_total = 0;
    g_enemies.spawned_total = 0;
    g_enemies.damage_received_total = 0.0f;
}

bp_enemy *bp_enemy_get(bp_handle h) { return (bp_enemy *)bp_pool_get(&g_enemies.pool, h); }

int bp_enemy_alive_count(void)
{
    int n = 0;
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (e && e->state != ES_DYING && e->state != ES_DEAD) n++;
    }
    return n;
}

int bp_enemy_count_type(bp_enemy_type t)
{
    int n = 0;
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (e && e->type == t && e->state != ES_DYING && e->state != ES_DEAD) n++;
    }
    return n;
}

bp_handle bp_enemy_spawn(bp_enemy_type type, Vector3 pos, float yaw, bool dormant)
{
    if (type < 0 || type >= ET_COUNT) type = ET_GRUNT;
    const bp_enemy_def *d = &g_enemies.def[type];

    bp_handle h = bp_pool_alloc(&g_enemies.pool);
    bp_enemy *e = (bp_enemy *)bp_pool_get(&g_enemies.pool, h);
    if (!e) { BP_LOGW("enemies: spawn failed, pool full"); return BP_NULL_HANDLE; }

    memset(e, 0, sizeof(*e));
    e->type = type;
    e->state = dormant ? ES_DORMANT : ES_IDLE;
    e->pos = pos;
    e->yaw = yaw;
    /* 難度只在生成時套用一次，而不是在每次傷害事件時套用。原因有二：AI 拿來推理的血條
     * （「我是否已低於痛苦閾值」）會與實際儲存的數值保持一致；而且中途改難度也絕不會
     * 留下半套用的敵人。護甲刻意「不」縮放 —— 護甲是一種傷害「型別」機制（吸收 55%），
     * 縮放它會無聲地改變每把武器對重甲敵人的表現。 */
    e->health = d->health * g_waves.diff_health;
    e->armor = d->armor;
    e->max_health = e->health;
    /* 最高難度下敵人不會畏縮：痛苦硬直（pain stagger）是難度給予的獎勵，而不是原型的
     * 固有屬性。 */
    e->pain_scale = (g_waves.difficulty >= 3) ? 0.0f : 1.0f;
    e->attack_timer = bp_rng_range(&g_engine.rng, 0.0f, d->attack_cooldown);
    e->strafe_dir = bp_rng_range(&g_engine.rng, 0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
    e->strafe_timer = bp_rng_range(&g_engine.rng, 0.4f, 1.4f);
    e->wander_yaw = yaw;
    e->last_known_pos = pos;
    e->activation_delay = dormant ? bp_rng_range(&g_engine.rng, 0.0f, 0.5f) : 0.0f;
    e->anim_time = bp_rng_range(&g_engine.rng, 0.0f, 10.0f);
    e->counted_for_wave = !dormant;

    g_enemies.spawned_total++;
    g_enemies.alive_count++;

    /* 生成回饋：一圈衝擊環加一小團火星，確保新的進場永遠不會被漏看。漏掉一次生成，是
     * 競技場射擊遊戲中「這感覺不公平」的頭號原因。 */
    bp_fx_particle(PTX_RING, Vector3Add(pos, (Vector3){ 0, 0.4f, 0 }), (Vector3){ 0, 0, 0 },
                   0.36f, d->radius * 1.2f, d->radius * 5.0f,
                   (Color){ 255, 120, 60, 200 }, (Color){ 255, 40, 20, 0 });
    for (int i = 0; i < 8; i++)
        bp_fx_particle(PTX_EMBER, Vector3Add(pos, (Vector3){ 0, 0.3f, 0 }),
                       (Vector3){ bp_rng_range(&g_engine.rng, -2.0f, 2.0f),
                                  bp_rng_range(&g_engine.rng, 1.0f, 4.0f),
                                  bp_rng_range(&g_engine.rng, -2.0f, 2.0f) },
                       0.45f, 0.10f, 0.005f, (Color){ 255, 180, 80, 230 }, (Color){ 200, 50, 10, 0 });

    return h;
}

/* ====================================================================================== */
/* 3.命中框（hitbox）                                                                        */
/* ====================================================================================== */
bool bp_enemy_body_box(bp_enemy *e, bp_aabb *out)
{
    if (!e || !out) return false;
    const bp_enemy_def *d = &g_enemies.def[e->type];
    float r = d->radius;
    float h = d->height;
    out->min = (Vector3){ e->pos.x - r, e->pos.y, e->pos.z - r };
    out->max = (Vector3){ e->pos.x + r, e->pos.y + h, e->pos.z + r };
    return true;
}

bool bp_enemy_head_box(bp_enemy *e, bp_aabb *out)
{
    if (!e || !out) return false;
    const bp_enemy_def *d = &g_enemies.def[e->type];
    if (d->body_set == 1) return false;          /* hound 沒有獨立的頭部 */
    if (d->body_set == 3) return false;          /* 砲塔：不提供爆頭加成 */

    float r = d->radius * 0.62f;
    float headH = d->height * 0.20f;
    float y0 = e->pos.y + d->height - headH;
    out->min = (Vector3){ e->pos.x - r, y0, e->pos.z - r };
    out->max = (Vector3){ e->pos.x + r, e->pos.y + d->height + headH * 0.25f, e->pos.z + r };
    return true;
}

Vector3 bp_enemy_center(bp_enemy *e)
{
    const bp_enemy_def *d = &g_enemies.def[e->type];
    return (Vector3){ e->pos.x, e->pos.y + d->height * 0.55f, e->pos.z };
}

float bp_enemy_nearest_distance(Vector3 from, bp_handle *out)
{
    float best = 1e9f;
    bp_handle bestH = BP_NULL_HANDLE;
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        uint32_t idx = g_enemies.pool.iter_list[i];
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, idx);
        if (!e || e->state == ES_DYING || e->state == ES_DEAD) continue;
        float d = Vector3Distance(bp_enemy_center(e), from);
        if (d < best) { best = d; bestH = bp_pool_handle_at(&g_enemies.pool, idx); }
    }
    if (out) *out = bestH;
    return best;
}

void bp_enemy_alert_nearby(Vector3 pos, float radius)
{
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (!e || e->state == ES_DYING || e->state == ES_DEAD) continue;
        if (Vector3Distance(e->pos, pos) > radius) continue;
        if (e->state == ES_DORMANT || e->state == ES_IDLE)
        {
            e->state = ES_PURSUE;
            e->state_time = 0.0f;
            e->last_known_pos = pos;
            e->memory_time = 4.0f;
        }
    }
}

/* ====================================================================================== */
/* 4. AI輔助函式                                                                             */
/* ====================================================================================== */
static void set_state(bp_enemy *e, bp_enemy_state s)
{
    if (e->state == s) return;
    e->state = s;
    e->state_time = 0.0f;
}

/* 感知：視覺是一條從敵人眼部到玩家胸口的射線（LOS），並受距離限制。聽覺則依賴「最後
 * 已知位置」的記憶，因此失去視線的敵人會繼續搜尋，而不是立刻忘記 —— 這是「笨」與
 * 「在追獵」之間的差別。 */
static void perceive(bp_enemy *e, const bp_enemy_def *d, Vector3 playerPos, Vector3 playerEye,
                     bool playerAlive)
{
    if (!playerAlive)
    {
        e->can_see = false;
        e->memory_time = BP_MAX(0.0f, e->memory_time - GetFrameTime());
        return;
    }

    Vector3 eye = (Vector3){ e->pos.x, e->pos.y + d->height * 0.82f, e->pos.z };
    Vector3 chest = (Vector3){ playerPos.x, playerPos.y + PLAYER_HEIGHT * 0.55f, playerPos.z };
    float dist = Vector3Distance(eye, chest);

    bool sees = false;
    if (dist < d->sight_range)
    {
        /* 140 度的視野錐（view cone）：站在你背後的敵人不該立刻轉身面對你，那是射擊遊戲
         * 中最破壞沉浸感的 AI 行為。 */
        Vector3 fwd = { sinf(e->yaw * BP_DEG2RAD), 0.0f, cosf(e->yaw * BP_DEG2RAD) };
        Vector3 to = Vector3Normalize(Vector3Subtract(chest, eye));
        float dot = Vector3DotProduct(fwd, to);
        bool inCone = (dot > 0.34f) || (dist < 5.0f);   /* 近距離 = 完全感知 */
        if (inCone && bp_world_line_of_sight(eye, chest)) sees = true;
    }

    e->can_see = sees;
    if (sees)
    {
        e->last_known_pos = playerPos;
        e->last_known_dist = dist;
        e->memory_time = 3.5f;

        if (e->state == ES_DORMANT || e->state == ES_IDLE)
        {
            set_state(e, ES_PURSUE);
            /* 首次目視的叫聲，由 sfx 冷卻表節流 */
            bp_audio_play_at((bp_sfx_id)d->sfx_sight, e->pos, playerEye, bp_camera_forward(), 1.0f);
            bp_enemy_alert_nearby(e->pos, d->hear_range * 0.8f);
        }
    }
    else
    {
        e->memory_time = BP_MAX(0.0f, e->memory_time - GetFrameTime());
    }
    BP_UNUSED(playerEye);
}

/* 導向（steering）：追擊 + 分離 + 障礙物閃避 + 橫移。全部只在水平面上運作。 */
static Vector3 steer(bp_enemy *e, const bp_enemy_def *d, Vector3 target, float desiredSpeed,
                     bool pursue)
{
    Vector3 flat = { target.x - e->pos.x, 0.0f, target.z - e->pos.z };
    float dist = Vector3Length(flat);
    Vector3 seek = { 0, 0, 0 };
    if (dist > 0.001f) seek = Vector3Scale(flat, 1.0f / dist);

    Vector3 right = { seek.z, 0.0f, -seek.x };

    /* ---- 橫移：會繞著你打轉，才讓競技場裡的敵人有威脅感，而不只是一隻走進你準心的
     *      血袋 ---- */
    e->strafe_timer -= GetFrameTime();
    if (e->strafe_timer <= 0.0f)
    {
        e->strafe_timer = bp_rng_range(&g_engine.rng, 0.55f, 1.60f);
        float r = bp_rng_range(&g_engine.rng, 0.0f, 1.0f);
        e->strafe_dir = (r < d->strafe * 0.5f) ? 1.0f : (r < d->strafe ? -1.0f : e->strafe_dir);
    }

    float strafeAmt = 0.0f;
    if (pursue && dist < 22.0f && dist > d->attack_range * 0.7f) strafeAmt = e->strafe_dir * d->strafe;

    /* ---- 分離：敵人絕不能疊成同一團 ---- */
    Vector3 sep = { 0, 0, 0 };
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *o = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (!o || o == e || o->state == ES_DYING || o->state == ES_DEAD) continue;
        Vector3 delta = { e->pos.x - o->pos.x, 0.0f, e->pos.z - o->pos.z };
        float dd = Vector3Length(delta);
        float minD = (d->radius + g_enemies.def[o->type].radius) * 1.35f;
        if (dd < minD && dd > 0.001f)
            sep = Vector3Add(sep, Vector3Scale(delta, (minD - dd) / (minD * dd) * 2.6f));
    }

    /* ---- 障礙物閃避：一根向前的鬚毛（whisker）加兩根斜向的。最先撞到的那根會沿著表面
     *      法線的垂直方向推出去，於是在完全沒有尋路的情況下做出貼牆行走。 ---- */
    Vector3 avoid = { 0, 0, 0 };
    if (desiredSpeed > 0.1f)
    {
        float whiskerLen = 1.0f + desiredSpeed * 0.24f;
        Vector3 probeY = { e->pos.x, e->pos.y + d->height * 0.4f, e->pos.z };

        static const float whiskerAngles[3] = { 0.0f, 0.62f, -0.62f };
        for (int w = 0; w < 3; w++)
        {
            float ca = cosf(whiskerAngles[w]), sa = sinf(whiskerAngles[w]);
            Vector3 dir = { seek.x * ca - seek.z * sa, 0.0f, seek.x * sa + seek.z * ca };
            if (w > 0)
            {
                dir.x = seek.x * ca + right.x * sa * 0.0f;
                dir = Vector3Normalize(Vector3Add(Vector3Scale(seek, ca), Vector3Scale(right, sa)));
            }

            float t; Vector3 n; const bp_solid *s;
            if (bp_world_sweep(probeY, Vector3Add(probeY, Vector3Scale(dir, whiskerLen)),
                               d->radius * 0.9f, &t, &n, &s))
            {
                float weight = (w == 0) ? 2.2f : 1.4f;
                weight *= 1.0f - BP_CLAMP(t / whiskerLen, 0.0f, 1.0f);
                Vector3 lat = { -n.z, 0.0f, n.x };
                if (Vector3Length(lat) < 0.001f) lat = right;
                /* 選一個與目前前進方向一致的側向，好讓敵人認定一側走到底，而不是左右
                 * 振盪 */
                if (Vector3DotProduct(lat, e->vel) < 0.0f && w == 0) lat = Vector3Scale(lat, -1.0f);
                else if (w > 0 && Vector3DotProduct(lat, seek) < 0.0f) lat = Vector3Scale(lat, -1.0f);
                avoid = Vector3Add(avoid, Vector3Scale(lat, weight));
                avoid = Vector3Add(avoid, Vector3Scale(n, weight * 0.55f));
            }
        }
    }

    Vector3 out = Vector3Add(Vector3Scale(seek, 1.0f), Vector3Scale(right, strafeAmt));
    out = Vector3Add(out, Vector3Scale(sep, 1.0f));
    out = Vector3Add(out, avoid);

    float l = Vector3Length(out);
    if (l > 0.0001f) out = Vector3Scale(out, desiredSpeed / l);
    return out;
}

/* 原型的參考速度，用來正規化加速度反應 */
static float desiredSpeedGuard(bp_enemy *e)
{
    const bp_enemy_def *d = &g_enemies.def[e->type];
    return BP_MAX(d->speed_run, 0.5f);
}

static void move_enemy(bp_enemy *e, const bp_enemy_def *d, Vector3 desired, float dt)
{
    /* 朝期望速度「加速」，而不是瞬間跳過去：能立刻改變方向的敵人讀起來像機器人，而且用
     * 投射物武器預判它的落點會困難得多。 */
    float accel = e->on_ground ? 22.0f : 6.0f;
    e->vel.x = bp_damp(e->vel.x, desired.x, accel / BP_MAX(desiredSpeedGuard(e), 0.001f) * 0.1f, dt);
    e->vel.y += GRAVITY * dt;
    e->vel.z = bp_damp(e->vel.z, desired.z, accel / BP_MAX(desiredSpeedGuard(e), 0.001f) * 0.1f, dt);

    /* 夾住水平速度，讓「橫移 + 追擊」的向量和永遠不會超過原型的奔跑速度 */
    Vector3 horiz = { e->vel.x, 0.0f, e->vel.z };
    float hs = Vector3Length(horiz);
    float maxS = d->speed_run * 1.12f;
    if (hs > maxS) { horiz = Vector3Scale(horiz, maxS / hs); e->vel.x = horiz.x; e->vel.z = horiz.z; }

    e->pos = Vector3Add(e->pos, Vector3Scale(e->vel, dt));

    int mat = MAT_CONCRETE;
    bp_world_resolve_body(&e->pos, d->radius, d->height, &e->vel, &e->on_ground, &mat);

    if (e->pos.y < g_world.bounds_min.y + 1.0f)
    {
        e->pos.y = g_world.bounds_min.y + 1.0f;
        e->vel.y = 0.0f;
        e->on_ground = true;
    }

    /* 面向移動方向（攻擊時則面向目標） */
    if (hs > 0.35f)
    {
        float targetYaw = atan2f(e->vel.x, e->vel.z) * BP_RAD2DEG;
        e->yaw = bp_lerp_angle(e->yaw, targetYaw, BP_CLAMP(dt * 9.0f, 0.0f, 1.0f));
    }
}

/* ====================================================================================== */
/* 5. 戰鬥                                                                                   */
/* ====================================================================================== */
static void enemy_attack(bp_enemy *e, const bp_enemy_def *d, Vector3 playerPos, Vector3 playerEye)
{
    if (e->attack_timer > 0.0f) return;

    Vector3 eye = { e->pos.x, e->pos.y + d->height * 0.75f, e->pos.z };
    Vector3 chest = { playerPos.x, playerPos.y + PLAYER_HEIGHT * 0.5f, playerPos.z };
    float dist = Vector3Distance(e->pos, playerPos);

    if (d->projectile)
    {
        /* 砲塔會對目標做少量預判，因此站在空地不動會被懲罰，而持續橫移的玩家躲得開 ——
         * 這句話的兩半都必須成立，遭遇戰才算公平。 */
        Vector3 to = Vector3Subtract(chest, eye);
        float len = Vector3Length(to);
        if (len < 0.01f) return;
        to = Vector3Scale(to, 1.0f / len);
        Vector3 lead = Vector3Scale(bp_player_velocity(), BP_CLAMP(len / BP_MAX(d->proj_speed, 1.0f), 0.0f, 0.45f));
        Vector3 aim = Vector3Normalize(Vector3Add(to, Vector3Scale(lead, 0.6f / BP_MAX(len, 1.0f))));

        bp_projectile_spawn(Vector3Add(eye, Vector3Scale(aim, 0.5f)), aim, d->proj_speed,
                            d->proj_damage * g_waves.diff_damage, d->proj_radius, 0.0f, false,
                            (Color){ 255, 90, 60, 255 }, MDL_PROJECTILE_PLASMA, BP_NULL_HANDLE);
        bp_audio_play_at((bp_sfx_id)d->attack_sfx, e->pos, playerEye, bp_camera_forward(), 1.0f);
        bp_fx_muzzle_flash(Vector3Add(eye, Vector3Scale(aim, 0.6f)), aim, 0.5f);
        e->attack_timer = d->attack_cooldown * bp_rng_range(&g_engine.rng, 0.85f, 1.2f);
        e->attack_wind = 0.0f;
        return;
    }

    /* 近戰 */
    if (dist <= d->attack_range + 0.35f)
    {
        e->attack_wind = 1.0f;
        e->lunge = 1.0f;
        bp_audio_play_at((bp_sfx_id)d->attack_sfx, e->pos, playerEye, bp_camera_forward(), 1.0f);

        Vector3 dir = Vector3Normalize(Vector3Subtract(chest, eye));
        float dmg = d->attack_damage * g_waves.diff_damage;
        bp_player_damage(dmg, e->pos, 0.45f);
        /* 擊退依「未經難度修正」的傷害縮放：更高難度應該是更痛，而不是把玩家甩過整個
         * 競技場。那是兩種不同的「難」。 */
        bp_player_impulse(Vector3Scale(dir, d->attack_damage * 0.16f));
        bp_fx_shake(0.20f, 0.2f);
        e->damage_dealt_to_player += dmg;
        e->attack_timer = d->attack_cooldown * bp_rng_range(&g_engine.rng, 0.9f, 1.15f);
    }
    else e->attack_timer = 0.12f;      /* 很快再檢查一次 */
}

/* ====================================================================================== */
/* 6. 每畫面更新                                                                             */
/* ====================================================================================== */
void bp_enemy_update(float dt, Vector3 playerPos, Vector3 playerEye, bool playerAlive)
{
    bp_pool_compact(&g_enemies.pool);
    int alive = 0;

    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        uint32_t idx = g_enemies.pool.iter_list[i];
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, idx);
        if (!e) continue;
        const bp_enemy_def *d = &g_enemies.def[e->type];

        e->state_time += dt;
        e->anim_time += dt;
        e->pain_flash = BP_MAX(0.0f, e->pain_flash - dt * 5.5f);
        e->attack_wind = BP_MAX(0.0f, e->attack_wind - dt * 4.0f);
        e->lunge = BP_MAX(0.0f, e->lunge - dt * 3.2f);
        if (e->attack_timer > 0.0f) e->attack_timer -= dt;

        switch (e->state)
        {
            case ES_DORMANT:
            {
                e->activation_delay -= dt;
                float dist = Vector3Distance(e->pos, playerPos);
                if (e->activation_delay <= 0.0f && (dist < 26.0f || playerAlive))
                {
                    if (dist < d->sight_range && bp_world_line_of_sight(e->pos, playerPos))
                        set_state(e, ES_PURSUE);
                    else if (dist < 22.0f) set_state(e, ES_IDLE);
                }
                break;
            }
            case ES_DYING:
            {
                e->die_timer += dt;
                e->vel.y += GRAVITY * dt;
                e->vel.x *= (1.0f - 4.0f * dt);
                e->vel.z *= (1.0f - 4.0f * dt);
                e->pos = Vector3Add(e->pos, Vector3Scale(e->vel, dt));
                bool og = false; int mat = MAT_CONCRETE;
                bp_world_resolve_body(&e->pos, d->radius * 0.9f, d->height * 0.4f, &e->vel, &og, &mat);

                /* 牠還帶有動量時拖出的血跡 */
                if (e->die_timer < 0.45f && bp_rng_range(&g_engine.rng, 0.0f, 1.0f) < 0.6f)
                    bp_fx_particle(PTX_BLOOD, bp_enemy_center(e),
                                   (Vector3){ bp_rng_range(&g_engine.rng, -1.0f, 1.0f),
                                              bp_rng_range(&g_engine.rng, 0.0f, 1.5f),
                                              bp_rng_range(&g_engine.rng, -1.0f, 1.0f) },
                                   0.5f, 0.16f, 0.01f,
                                   (Color){ 170, 16, 26, 220 }, (Color){ 90, 6, 12, 0 });
                if (e->die_timer > 1.55f)
                {
                    e->state = ES_DEAD;
                    bp_pool_free(&g_enemies.pool, bp_pool_handle_at(&g_enemies.pool, idx));
                    g_enemies.alive_count = BP_MAX(0, g_enemies.alive_count - 1);
                }
                break;
            }
            case ES_DEAD:
                bp_pool_free(&g_enemies.pool, bp_pool_handle_at(&g_enemies.pool, idx));
                break;

            default:
            {
                if (e->state != ES_DYING && e->state != ES_DEAD) alive++;

                perceive(e, d, playerPos, playerEye, playerAlive);

                /* 硬直有一個最短停留時間，確保受擊反應永遠讀得清楚 */
                if (e->state == ES_STAGGER)
                {
                    e->stagger_timer -= dt;
                    move_enemy(e, d, Vector3Scale(e->vel, 0.0f), dt);
                    if (e->stagger_timer <= 0.0f)
                        set_state(e, e->can_see ? ES_PURSUE : (e->memory_time > 0.0f ? ES_PURSUE : ES_IDLE));
                    break;
                }

                float distToPlayer = Vector3Distance(e->pos, playerPos);
                float distToMemory = Vector3Distance(e->pos, e->last_known_pos);

                /* ---- 狀態選擇 ---- */
                bool inAttackRange = distToPlayer <= d->attack_range + 0.25f;
                bool rangedReady = d->projectile && e->can_see && distToPlayer <= d->attack_range;

                if (playerAlive && (inAttackRange || rangedReady)) set_state(e, ES_ATTACK);
                else if (e->can_see || e->memory_time > 0.0f) set_state(e, ES_PURSUE);
                else set_state(e, ES_IDLE);

                /* ---- 移動 ---- */
                float desiredSpeed;
                Vector3 target;
                bool pursue = false;

                switch (e->state)
                {
                    case ES_ATTACK:
                        desiredSpeed = d->projectile ? 0.0f : d->speed_walk * 0.55f;
                        target = playerPos;
                        if (!d->projectile)
                        {
                            /* 攻擊時仍要持續逼近：近戰敵人在最大射程處停住不動，讀起來
                             * 就像 AI 壞掉了 */
                            desiredSpeed = (distToPlayer > d->attack_range * 0.75f)
                                         ? d->speed_run * d->aggression : d->speed_walk * 0.4f;
                        }
                        enemy_attack(e, d, playerPos, playerEye);
                        break;
                    case ES_PURSUE:
                        target = e->can_see ? playerPos : e->last_known_pos;
                        desiredSpeed = d->speed_run * d->aggression;
                        pursue = true;
                        /* 接近「記憶中的位置」時放慢成步行，免得敵人一頭衝向它已經看不
                         * 穿的牆 */
                        if (!e->can_see && distToMemory < 4.0f) desiredSpeed = d->speed_walk;
                        break;
                    default: /* ES_IDLE */
                        e->wander_yaw += bp_rng_range(&g_engine.rng, -40.0f, 40.0f) * dt;
                        target = Vector3Add(e->pos, (Vector3){ sinf(e->wander_yaw * BP_DEG2RAD) * 6.0f,
                                                               0.0f,
                                                               cosf(e->wander_yaw * BP_DEG2RAD) * 6.0f });
                        desiredSpeed = d->speed_walk * 0.45f;
                        break;
                }

                if (d->is_static) desiredSpeed = 0.0f;

                Vector3 want = steer(e, d, target, desiredSpeed, pursue);
                move_enemy(e, d, want, dt);

                /* 攻擊時（或看得見玩家時）面向玩家，前搖才讀得正確 */
                if (e->state == ES_ATTACK || e->can_see)
                {
                    float ty = atan2f(playerPos.x - e->pos.x, playerPos.z - e->pos.z) * BP_RAD2DEG;
                    e->yaw = bp_lerp_angle(e->yaw, ty, BP_CLAMP(dt * (e->state == ES_ATTACK ? 12.0f : 6.0f), 0.0f, 1.0f));
                }

                /* 步態動畫的相位隨著實際地面速度前進 */
                float hs = sqrtf(e->vel.x * e->vel.x + e->vel.z * e->vel.z);
                e->leg_phase += dt * (2.2f + hs * 1.5f);
                e->bob = sinf(e->leg_phase * 2.0f) * 0.035f * BP_CLAMP(hs / 4.0f, 0.0f, 1.0f);

                /* 從未見過玩家的閒置敵人最終會放棄、回去閒晃，競技場才不會永遠處於
                 * 「警戒」狀態 */
                if (e->state == ES_IDLE && e->state_time > 6.0f) e->wander_yaw += 25.0f * dt;
                break;
            }
        }
    }
    BP_UNUSED(alive);
}

/* ====================================================================================== */
/* 7. 傷害 / 死亡                                                                            */
/* ====================================================================================== */
float bp_enemy_take_damage(bp_handle h, float amount, Vector3 point, Vector3 dir,
                           bool headshot, float knockback, bool *out_killed)
{
    if (out_killed) *out_killed = false;
    bp_enemy *e = bp_enemy_get(h);
    if (!e || e->state == ES_DYING || e->state == ES_DEAD) return 0.0f;

    const bp_enemy_def *d = &g_enemies.def[e->type];
    float dmg = amount;

    /* 護甲先行吸收並被消耗，規則與玩家完全一樣。對稱的規則意味著玩家可以用自己已經熟悉的
     * 那套心智模型，去推理一個重甲敵人。 */
    if (e->armor > 0.0f)
    {
        float absorbed = BP_MIN(e->armor, dmg * 0.55f);
        e->armor -= absorbed;
        dmg -= absorbed;
    }

    e->health -= dmg;
    e->hits_taken++;
    e->pain_flash = 1.0f;
    g_enemies.damage_received_total += amount;

    /* 挨打就醒，即使子彈是從背後來的 */
    if (e->state == ES_DORMANT || e->state == ES_IDLE)
    {
        set_state(e, ES_PURSUE);
        e->last_known_pos = bp_player_position();
        e->memory_time = 4.0f;
    }

    /* 擊退依原型縮放，如此 brute 才不會被連續挑在空中打 */
    if (knockback > 0.0f && !d->is_static)
    {
        float k = knockback * d->knockback_scale * 0.55f;
        e->vel.x += dir.x * k;
        e->vel.z += dir.z * k;
        if (dir.y > 0.1f) e->vel.y += dir.y * k * 0.5f;
    }

    /* 痛苦硬直：只有原型允許、而且這一擊確實有意義時才觸發。讓手槍每發都能硬直一隻
     * brute，會把整場戰鬥變得太廉價。
     * 比較的是 e->max_health 而不是 d->health：原型值是難度修正前的數字，拿它來比會讓
     * 高難度下根本觸發不了硬直。 */
    if (!d->is_static && e->pain_scale > 0.0f && dmg > e->max_health * 0.06f &&
        bp_rng_range(&g_engine.rng, 0.0f, 1.0f) < d->pain_chance * e->pain_scale)
    {
        set_state(e, ES_STAGGER);
        e->stagger_timer = 0.22f;
        bp_audio_play_at((bp_sfx_id)d->sfx_pain, e->pos, bp_camera_eye(), bp_camera_forward(), 1.0f);
    }

    if (e->health <= 0.0f)
    {
        e->health = 0.0f;
        set_state(e, ES_DYING);
        e->die_timer = 0.0f;
        e->vel = Vector3Add(Vector3Scale(dir, 2.6f * d->knockback_scale), (Vector3){ 0, 1.6f, 0 });
        g_enemies.killed_total++;

        bp_audio_play_at((bp_sfx_id)d->sfx_die, e->pos, bp_camera_eye(), bp_camera_forward(), 1.0f);
        bp_fx_blood_mist(bp_enemy_center(e), dir, 1.2f);
        bp_fx_decal(TEX_BLOOD_SPLAT, (Vector3){ e->pos.x, 0.03f, e->pos.z }, (Vector3){ 0, 1, 0 },
                    d->radius * 2.6f, 26.0f, (Color){ 255, 255, 255, 200 });
        bp_fx_shake(d->body_set == 2 ? 0.42f : 0.12f, 0.25f);

        /* 掉落物 */
        if (!e->dropped_pickup)
        {
            e->dropped_pickup = true;
            float r = bp_rng_range(&g_engine.rng, 0.0f, 1.0f);
            if (r < d->drop_health)
                bp_pickup_spawn(PICKUP_HEALTH, e->pos, 25.0f, -1);
            else if (r < d->drop_health + d->drop_ammo)
                /* 彈藥掉落以「一組你手上那把武器的補給」為單位，而不是以發數計算：一顆
                 * 霰彈與一個衝鋒彈匣的「發數」天差地遠，而只有武器模組知道這層對應。 */
                bp_pickup_spawn(PICKUP_AMMO, e->pos,
                                (float)g_weapon_table[g_weapons.current].ammo_per_pickup, -1);
            else if (r < d->drop_health + d->drop_ammo + d->drop_armor)
                bp_pickup_spawn(PICKUP_ARMOR, e->pos, 25.0f, -1);
        }

        bp_player_register_kill((int)((float)d->score * (headshot ? 2.0f : 1.0f) * g_waves.diff_score));
        if (out_killed) *out_killed = true;

        /* 波次導演掌控節奏，因此必須在擊殺發生的「當下」（而不是該畫面結束時）就知道，
         * 這樣一波才能與牠最後一隻敵人死在同一拍結束。正是這點讓「清場」感覺是即時的。 */
        bp_waves_notify_kill(e->type);
    }

    return amount;
}

/* ====================================================================================== */
/* 8. 動畫與繪製                                                                             */
/* ====================================================================================== */
static Matrix part_matrix(Vector3 world, float yawDeg, Vector3 local, Vector3 joint,
                          Vector3 axis, float angleDeg, Vector3 scale)
{
    /* 順序（列向量慣例）：局部偏移 -> 繞關節旋轉 -> 偏航 -> 平移到世界座標。繞「關節」
     * 而不是繞零件中心旋轉，才會讓腿從髖部甩出去，而不是在原地自轉。 */
    Matrix m = MatrixTranslate(local.x * scale.x, local.y * scale.y + world.y, local.z * scale.z);
    m = MatrixMultiply(m, MatrixTranslate(joint.x, joint.y, joint.z));
    m = MatrixMultiply(m, MatrixRotate(axis, angleDeg * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixTranslate(-joint.x, -joint.y, -joint.z));
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 0, 1, 0 }, yawDeg * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixTranslate(world.x, 0.0f, world.z));
    BP_UNUSED(scale);
    return m;
}

/*
 * 在任意轉換下繪製單一個程序化身體零件。
 *
 * raylib 的 DrawModelEx 只接受「位置 + 軸／角度 + 各軸縮放」，無法表達我們組合出來的關節
 * 鏈（translate * rotateX * yaw * translate）。因此我們把完整矩陣推進 rlSetMatrixModelview()
 * 並直接呼叫 DrawMesh() —— 這正是 raylib 在 DrawModel() 內部做的事，只是省去了轉換拆解。
 * 著色器與材質狀態已由外層的 BeginMode3D 傳遞設定好。
 */
static void draw_part(bp_model_id id, Matrix m, Color tint)
{
    if (!bp_model_valid(id)) return;
    Model model = bp_model_get(id);
    if (model.meshCount <= 0) return;

    /* 把色調併入材質的 diffuse，如此只會有一條程式碼路徑。 */
    Color saved = model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color;
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = tint;

    rlSetMatrixModelview(m);
    for (int mi = 0; mi < model.meshCount; mi++) {
        if (model.meshes[mi].vertexCount <= 0) continue;
        DrawMesh(model.meshes[mi], model.materials[0], m);
    }
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = saved;
}

static Color pain_tint(Color base, float flash)
{
    if (flash <= 0.01f) return base;
    return (Color){
        (unsigned char)BP_CLAMP(base.r + (int)(255.0f * flash * 0.85f), 0, 255),
        (unsigned char)BP_CLAMP(base.g + (int)(255.0f * flash * 0.85f), 0, 255),
        (unsigned char)BP_CLAMP(base.b + (int)(255.0f * flash * 0.85f), 0, 255),
        255 };
}

static void draw_humanoid(bp_enemy *e, const bp_enemy_def *d, Color body, Color trim)
{
    float s = d->scale;
    float legSwing = sinf(e->leg_phase) * 34.0f * BP_CLAMP(Vector3Length(e->vel) / BP_MAX(d->speed_run, 0.1f), 0.0f, 1.2f);
    float armSwing = -legSwing * 0.72f;
    float deathTilt = (e->state == ES_DYING) ? BP_MIN(e->die_timer * 130.0f, 88.0f) : 0.0f;
    float windUp = e->attack_wind * 46.0f;

    Vector3 w = e->pos;
    Vector3 sc = { s, s, s };

    /* 腿：以髖部為軸 */
    Vector3 hipL = { -0.19f * s, 0.78f * s, 0.0f };
    Vector3 hipR = {  0.19f * s, 0.78f * s, 0.0f };
    draw_part(MDL_ENEMY_LEG, part_matrix(w, e->yaw, hipL, (Vector3){ 0, 0.39f * s, 0 },
                                         (Vector3){ 1, 0, 0 }, legSwing - deathTilt * 0.4f, sc), trim);
    draw_part(MDL_ENEMY_LEG, part_matrix(w, e->yaw, hipR, (Vector3){ 0, 0.39f * s, 0 },
                                         (Vector3){ 1, 0, 0 }, -legSwing - deathTilt * 0.4f, sc), trim);

    /* 身軀 */
    Vector3 torso = { 0.0f, 0.72f * s + e->bob, 0.0f };
    Matrix torsoM = MatrixTranslate(torso.x, w.y + torso.y, torso.z);
    torsoM = MatrixMultiply(torsoM, MatrixRotate((Vector3){ 1, 0, 0 }, -deathTilt * BP_DEG2RAD));
    torsoM = MatrixMultiply(torsoM, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    torsoM = MatrixMultiply(torsoM, MatrixTranslate(w.x, 0.0f, w.z));
    draw_part(MDL_ENEMY_TORSO, torsoM, body);

    /* 手臂：以肩膀為軸，其中一隻手帶動攻擊的前搖 */
    Vector3 shL = { -0.40f * s, 1.34f * s + e->bob, 0.0f };
    Vector3 shR = {  0.40f * s, 1.34f * s + e->bob, 0.0f };
    draw_part(MDL_ENEMY_ARM, part_matrix(w, e->yaw, shL, (Vector3){ 0, 0.31f * s, 0 },
                                         (Vector3){ 1, 0, 0 }, armSwing - windUp - deathTilt * 0.7f, sc), trim);
    draw_part(MDL_ENEMY_ARM, part_matrix(w, e->yaw, shR, (Vector3){ 0, 0.31f * s, 0 },
                                         (Vector3){ 1, 0, 0 }, -armSwing - windUp * 1.35f - deathTilt * 0.7f, sc), trim);

    /* 頭：隨著身體起伏與傾斜 */
    Vector3 head = { 0.0f, 1.58f * s + e->bob * 1.4f, 0.02f * s };
    Matrix headM = MatrixTranslate(head.x, w.y + head.y, head.z);
    headM = MatrixMultiply(headM, MatrixRotate((Vector3){ 1, 0, 0 }, -deathTilt * BP_DEG2RAD));
    headM = MatrixMultiply(headM, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    headM = MatrixMultiply(headM, MatrixTranslate(w.x, 0.0f, w.z));
    draw_part(MDL_ENEMY_HEAD, headM, trim);

    /* 眼睛：兩個加性光點。在沒有光照 pass 的算圖器裡，發光的眼睛是讓敵人在黑暗角落中
     * 讀得出來最便宜、也最有效的辦法。 */
    Vector3 fwd = { sinf(e->yaw * BP_DEG2RAD), 0.0f, cosf(e->yaw * BP_DEG2RAD) };
    Vector3 right = { fwd.z, 0.0f, -fwd.x };
    Vector3 eyeC = Vector3Add(Vector3Add(w, (Vector3){ 0, 1.60f * s + e->bob * 1.4f, 0 }),
                              Vector3Scale(fwd, 0.17f * s));
    Color eye = (Color){ d->eye_color.r, d->eye_color.g, d->eye_color.b,
                         (unsigned char)(e->state == ES_DYING ? 90 : 235) };
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    DrawSphere(Vector3Add(eyeC, Vector3Scale(right, 0.075f * s)), 0.055f * s, eye);
    DrawSphere(Vector3Add(eyeC, Vector3Scale(right, -0.075f * s)), 0.055f * s, eye);
    rlSetBlendMode(RL_BLEND_ALPHA);
}

static void draw_hound(bp_enemy *e, const bp_enemy_def *d, Color body, Color trim)
{
    float s = d->scale;
    Vector3 w = e->pos;
    float legSwing = sinf(e->leg_phase * 1.6f) * 40.0f;
    float deathTilt = (e->state == ES_DYING) ? BP_MIN(e->die_timer * 160.0f, 90.0f) : 0.0f;
    Vector3 sc = { s, s, s };

    /* 四條腿分佈在身體四角 */
    static const Vector3 legPos[4] = {
        { -0.20f, 0.42f, -0.36f }, { 0.20f, 0.42f, -0.36f },
        { -0.20f, 0.42f,  0.36f }, { 0.20f, 0.42f,  0.36f },
    };
    for (int i = 0; i < 4; i++)
    {
        Vector3 lp = Vector3Scale(legPos[i], s);
        float ph = (i == 0 || i == 3) ? legSwing : -legSwing;
        draw_part(MDL_HOUND_LEG, part_matrix(w, e->yaw, lp, (Vector3){ 0, 0.23f * s, 0 },
                                             (Vector3){ 1, 0, 0 }, ph - deathTilt * 0.3f, sc), trim);
    }

    /* 身體 */
    Vector3 torso = { 0.0f, 0.40f * s + e->bob, 0.0f };
    Matrix m = MatrixTranslate(torso.x, w.y + torso.y, torso.z);
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 1, 0, 0 }, -deathTilt * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixTranslate(w.x, 0.0f, w.z));
    draw_part(MDL_HOUND_BODY, m, body);

    /* 頭：攻擊前搖時向前撲出 */
    Vector3 head = { 0.0f, 0.60f * s + e->bob, (0.55f + e->lunge * 0.28f) * s };
    Matrix hm = MatrixTranslate(head.x, w.y + head.y, head.z);
    hm = MatrixMultiply(hm, MatrixRotate((Vector3){ 1, 0, 0 }, (-deathTilt - e->lunge * 22.0f) * BP_DEG2RAD));
    hm = MatrixMultiply(hm, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    hm = MatrixMultiply(hm, MatrixTranslate(w.x, 0.0f, w.z));
    draw_part(MDL_HOUND_HEAD, hm, trim);

    Vector3 fwd = { sinf(e->yaw * BP_DEG2RAD), 0.0f, cosf(e->yaw * BP_DEG2RAD) };
    Vector3 eyeC = Vector3Add(Vector3Add(w, (Vector3){ 0, 0.62f * s, 0 }), Vector3Scale(fwd, 0.72f * s));
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    DrawSphere(eyeC, 0.085f * s, (Color){ d->eye_color.r, d->eye_color.g, d->eye_color.b,
                                          (unsigned char)(e->state == ES_DYING ? 70 : 220) });
    rlSetBlendMode(RL_BLEND_ALPHA);
}

static void draw_brute(bp_enemy *e, const bp_enemy_def *d, Color body, Color trim)
{
    float s = d->scale;
    Vector3 w = e->pos;
    float legSwing = sinf(e->leg_phase * 0.8f) * 26.0f;
    float windUp = e->attack_wind * 62.0f;
    float deathTilt = (e->state == ES_DYING) ? BP_MIN(e->die_timer * 90.0f, 84.0f) : 0.0f;
    Vector3 sc = { s, s, s };

    draw_part(MDL_BRUTE_LEG, part_matrix(w, e->yaw, (Vector3){ -0.42f * s, 1.00f * s, 0 },
                                         (Vector3){ 0, 0.5f * s, 0 }, (Vector3){ 1, 0, 0 },
                                         legSwing - deathTilt * 0.35f, sc), trim);
    draw_part(MDL_BRUTE_LEG, part_matrix(w, e->yaw, (Vector3){ 0.42f * s, 1.00f * s, 0 },
                                         (Vector3){ 0, 0.5f * s, 0 }, (Vector3){ 1, 0, 0 },
                                         -legSwing - deathTilt * 0.35f, sc), trim);

    Vector3 torso = { 0.0f, 0.90f * s + e->bob * 0.6f, 0.0f };
    Matrix m = MatrixTranslate(torso.x, w.y + torso.y, torso.z);
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 1, 0, 0 }, -deathTilt * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixTranslate(w.x, 0.0f, w.z));
    draw_part(MDL_BRUTE_TORSO, m, body);

    /* 極巨大的手臂，攻擊時會高舉過頭揮下 */
    draw_part(MDL_BRUTE_ARM, part_matrix(w, e->yaw, (Vector3){ -0.86f * s, 1.86f * s, 0 },
                                         (Vector3){ 0, 0.52f * s, 0 }, (Vector3){ 1, 0, 0 },
                                         -legSwing * 0.5f - windUp - deathTilt * 0.6f, sc), trim);
    draw_part(MDL_BRUTE_ARM, part_matrix(w, e->yaw, (Vector3){ 0.86f * s, 1.86f * s, 0 },
                                         (Vector3){ 0, 0.52f * s, 0 }, (Vector3){ 1, 0, 0 },
                                         legSwing * 0.5f - windUp * 1.2f - deathTilt * 0.6f, sc), trim);

    draw_part(MDL_BRUTE_HEAD, part_matrix(w, e->yaw, (Vector3){ 0, 2.36f * s, 0.04f * s },
                                          (Vector3){ 0, 0, 0 }, (Vector3){ 1, 0, 0 },
                                          -deathTilt * 0.8f, sc), body);

    Vector3 fwd = { sinf(e->yaw * BP_DEG2RAD), 0.0f, cosf(e->yaw * BP_DEG2RAD) };
    Vector3 eyeC = Vector3Add(Vector3Add(w, (Vector3){ 0, 2.40f * s, 0 }), Vector3Scale(fwd, 0.30f * s));
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    DrawSphere(eyeC, 0.11f * s, (Color){ d->eye_color.r, d->eye_color.g, d->eye_color.b,
                                         (unsigned char)(e->state == ES_DYING ? 60 : 235) });
    rlSetBlendMode(RL_BLEND_ALPHA);
}

static void draw_turret(bp_enemy *e, const bp_enemy_def *d, Color body, Color trim)
{
    Vector3 w = e->pos;
    /* 底座固定不動；槍管隨 e->yaw 偏航，並朝玩家俯仰 */
    draw_part(MDL_TURRET_BASE, MatrixTranslate(w.x, w.y + 0.28f, w.z), body);

    float pitch = 0.0f;
    Vector3 eye = { w.x, w.y + 1.10f, w.z };
    Vector3 chest = { bp_player_position().x, bp_player_position().y + 0.9f, bp_player_position().z };
    Vector3 delta = Vector3Subtract(chest, eye);
    float horiz = sqrtf(delta.x * delta.x + delta.z * delta.z);
    if (horiz > 0.01f) pitch = atan2f(delta.y, horiz) * BP_RAD2DEG;
    pitch = BP_CLAMP(pitch, -45.0f, 45.0f);

    Matrix m = MatrixTranslate(0.0f, 0.0f, 0.42f);
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 1, 0, 0 }, pitch * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixRotate((Vector3){ 0, 1, 0 }, e->yaw * BP_DEG2RAD));
    m = MatrixMultiply(m, MatrixTranslate(w.x, w.y + 1.05f, w.z));
    draw_part(MDL_TURRET_BARREL, m, trim);

    /* 狀態指示燈：閒置時綠色，追蹤中紅色。玩家在競技場另一端瞥一眼就讀得懂的前搖訊號。 */
    Color lamp = e->can_see ? (Color){ 255, 60, 40, 255 } : (Color){ 90, 220, 120, 255 };
    if (e->state == ES_DYING) lamp = (Color){ 60, 60, 60, 255 };
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    DrawSphere((Vector3){ w.x, w.y + 1.42f, w.z }, 0.13f, lamp);
    rlSetBlendMode(RL_BLEND_ALPHA);
    BP_UNUSED(d);
}

void bp_enemy_draw(Camera3D cam, Vector3 viewer)
{
    BP_UNUSED(cam);
    bp_pool_compact(&g_enemies.pool);
    float cull2 = 150.0f * 150.0f;

    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (!e) continue;
        if (Vector3LengthSqr(Vector3Subtract(e->pos, viewer)) > cull2) continue;

        const bp_enemy_def *d = &g_enemies.def[e->type];
        float flash = e->pain_flash;
        Color body = pain_tint(d->color_body, flash);
        Color trim = pain_tint(d->color_trim, flash);

        /* 垂死的敵人會下沉並淡出，而不是憑空消失：屍體是「這一擊確實算數」的回饋，而且
         * 在快節奏戰鬥中依然讀得清楚。 */
        if (e->state == ES_DYING)
        {
            float k = BP_CLAMP(e->die_timer / 1.55f, 0.0f, 1.0f);
            unsigned char a = (unsigned char)(255.0f * (1.0f - k * k));
            body.a = a; trim.a = a;
        }

        switch (d->body_set)
        {
            case 1:  draw_hound(e, d, body, trim); break;
            case 2:  draw_brute(e, d, body, trim); break;
            case 3:  draw_turret(e, d, body, trim); break;
            default: draw_humanoid(e, d, body, trim); break;
        }

        /* 接觸陰影：地板上的一圈暗色圓盤。少了它，敵人看起來會像飄在空中，距離判斷也就
         * 隨之失效。 */
        float fh = bp_world_floor_height(e->pos, d->radius, e->pos.y + 0.2f);
        if (fh > -1e8f && e->state != ES_DYING)
        {
            float h = BP_CLAMP(1.0f - (e->pos.y - fh) * 0.6f, 0.15f, 1.0f);
            rlSetBlendMode(RL_BLEND_ADD_COLORS);
            DrawCylinder((Vector3){ e->pos.x, fh + 0.02f, e->pos.z },
                         d->radius * 1.15f * h, d->radius * 1.15f * h, 0.01f, 12,
                         (Color){ 0, 0, 0, (unsigned char)(120.0f * h) });
            rlSetBlendMode(RL_BLEND_ALPHA);
        }
    }
}

void bp_enemy_draw_debug(void)
{
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (!e) continue;
        const bp_enemy_def *d = &g_enemies.def[e->type];

        bp_aabb b;
        if (bp_enemy_body_box(e, &b)) bp_gfx_draw_aabb(b.min, b.max, LIME);
        if (bp_enemy_head_box(e, &b)) bp_gfx_draw_aabb(b.min, b.max, YELLOW);

        Color sc = RED;
        switch (e->state)
        {
            case ES_DORMANT: sc = DARKGRAY; break;
            case ES_IDLE:    sc = BLUE;     break;
            case ES_PURSUE:  sc = ORANGE;   break;
            case ES_ATTACK:  sc = RED;      break;
            case ES_STAGGER: sc = YELLOW;   break;
            case ES_DYING:   sc = MAROON;   break;
            default: break;
        }
        Vector3 eye = { e->pos.x, e->pos.y + d->height * 0.8f, e->pos.z };
        bp_gfx_draw_ray(eye, e->last_known_pos, e->can_see ? GREEN : sc);
        DrawSphere(eye, 0.08f, sc);
        /* 頭頂的血條：比 3D 文字便宜，而且任何角度都讀得清楚 */
        float frac = BP_CLAMP(e->health / BP_MAX(e->max_health, 1.0f), 0.0f, 1.0f);
        Vector3 barBase = Vector3Add(e->pos, (Vector3){ -d->radius, d->height + 0.34f, 0 });
        DrawCube(Vector3Add(barBase, (Vector3){ 0, 0, 0 }), d->radius * 2.0f, 0.06f, 0.06f,
                 (Color){ 40, 40, 40, 200 });
        DrawCube(Vector3Add(barBase, (Vector3){ -(1.0f - frac) * d->radius, 0, 0 }),
                 d->radius * 2.0f * frac, 0.06f, 0.06f,
                 frac > 0.5f ? LIME : (frac > 0.25f ? YELLOW : RED));
    }
}
