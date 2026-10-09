/******************************************************************************************
 * bp_hitdetect.c
 ******************************************************************************************/
#include "bp_hitdetect.h"
#include "bp_world.h"
#include "bp_enemies.h"
#include "bp_player.h"
#include "bp_fx.h"
#include "bp_weapons.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"

/* ====================================================================================== */
/* 射線過濾器                                                                             */
/* ====================================================================================== */
bp_trace_filter bp_filter_default(void)
{
    bp_trace_filter f;
    memset(&f, 0, sizeof(f));
    f.max_distance = 400.0f;
    f.penetrate_shoot_through = false;
    f.ignore_entity = BP_NULL_HANDLE;
    return f;
}

bp_trace_filter bp_filter_projectile(bp_handle owner)
{
    bp_trace_filter f = bp_filter_default();
    f.ignore_entity = owner;
    return f;
}

/* ====================================================================================== */
/* 敵人命中框                                                                             */
/* ====================================================================================== */
/*
 * 頭部框位於身體圓柱的頂端，而且刻意做得比寫實頭骨更寬（0.62 x 身體半徑）。
 * 理由：在一款節奏快的競技場射擊遊戲裡，差一點點的爆頭被判定成身體命中，
 * 感覺像遊戲對你出老千；而寬鬆的頭部框感覺像自己的技術。每個真的做過這個
 * 實驗的競技型射擊遊戲，最後都落在寬鬆這一邊。
 */
static bool trace_enemy(const bp_enemy *e, Vector3 origin, Vector3 dir, float maxDist,
                        float *outDist, bool *outHead, Vector3 *outPoint, Vector3 *outNormal)
{
    bp_aabb body, head;
    if (!bp_enemy_body_box((bp_enemy *)e, &body)) return false;
    bool hasHead = bp_enemy_head_box((bp_enemy *)e, &head);

    float tBody = maxDist, tHead = maxDist;
    Vector3 nBody = { 0, 0, 0 }, nHead = { 0, 0, 0 };
    bool hitBody = bp_ray_aabb(origin, dir, &body, maxDist, &tBody, &nBody);
    bool hitHead = hasHead && bp_ray_aabb(origin, dir, &head, maxDist, &tHead, &nHead);

    if (!hitBody && !hitHead) return false;

    if (hitHead && (!hitBody || tHead <= tBody))
    {
        *outDist = tHead; *outHead = true; *outNormal = nHead;
        *outPoint = Vector3Add(origin, Vector3Scale(dir, tHead));
    }
    else
    {
        *outDist = tBody; *outHead = false; *outNormal = nBody;
        *outPoint = Vector3Add(origin, Vector3Scale(dir, tBody));
    }
    return true;
}

/* ====================================================================================== */
/* 射線追蹤                                                                               */
/* ====================================================================================== */
static bp_hit trace_core(Vector3 origin, Vector3 dir, float radius, float maxDist,
                         const bp_trace_filter *filter)
{
    bp_hit result;
    memset(&result, 0, sizeof(result));
    result.kind = HIT_NONE;
    result.distance = maxDist;
    result.normal = (Vector3){ 0, 1, 0 };

    if (Vector3LengthSqr(dir) < 1e-8f || maxDist <= 0.0f) return result;

    float best = maxDist;
    bp_trace_filter fallback = bp_filter_default();
    fallback.max_distance = maxDist;
    const bp_trace_filter *f = filter ? filter : &fallback;

    /* ---- 1. 敵人 ---- */
    if (!f->ignore_enemies)
    {
        bp_pool_compact(&g_enemies.pool);
        for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
        {
            uint32_t idx = g_enemies.pool.iter_list[i];
            bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, idx);
            if (!e || e->state == ES_DYING || e->state == ES_DEAD || e->state == ES_DORMANT) continue;

            float d; bool head; Vector3 p, n;
            if (trace_enemy(e, origin, dir, best, &d, &head, &p, &n))
            {
                best = d;
                result.kind = HIT_ENEMY;
                result.distance = d;
                result.point = p;
                result.normal = n;
                result.headshot = head;
                result.entity = bp_pool_handle_at(&g_enemies.pool, idx);
            }
        }
    }

    /* ---- 2. 可破壞物 ---- */
    if (!f->ignore_barrels)
    {
        for (int i = 0; i < g_world.solid_count; i++)
        {
            const bp_solid *s = &g_world.solids[i];
            if (!s->explode || s->destroyed) continue;

            bp_aabb grown = bp_aabb_grow(&s->box, radius);
            float t; Vector3 n;
            if (bp_ray_aabb(origin, dir, &grown, best, &t, &n))
            {
                best = t;
                result.kind = HIT_BARREL;
                result.distance = t;
                result.normal = n;
                result.point = Vector3Add(origin, Vector3Scale(dir, t));
                result.solid_index = i;
                result.material = s->material;
            }
        }
    }

    /* ---- 3. 世界幾何 ---- */
    if (!f->ignore_world)
    {
        float t; Vector3 n; const bp_solid *s = NULL;
        /* bp_world_sweep 本來就會略過可射穿的幾何；radius 會把方框加寬，好讓火箭
         * 不會撞上它視覺上早已通過的門框。 */
        if (bp_world_sweep(origin, Vector3Add(origin, Vector3Scale(dir, best)), radius, &t, &n, &s))
        {
            if (t <= best)
            {
                best = t;
                result.kind = HIT_WORLD;
                result.distance = t;
                result.normal = n;
                result.point = Vector3Add(origin, Vector3Scale(dir, t));
                result.material = s ? s->material : MAT_CONCRETE;
                for (int i = 0; i < g_world.solid_count; i++)
                    if (&g_world.solids[i] == s) { result.solid_index = i; break; }
            }
        }
    }

    if (result.kind == HIT_NONE)
        result.point = Vector3Add(origin, Vector3Scale(dir, maxDist));

    return result;
}

bp_hit bp_trace_ray(Vector3 origin, Vector3 dir, float maxDistance, const bp_trace_filter *filter)
{
    return trace_core(origin, dir, 0.0f, maxDistance, filter);
}

bp_hit bp_trace_sphere(Vector3 origin, Vector3 dir, float radius, float maxDistance,
                       const bp_trace_filter *filter)
{
    return trace_core(origin, dir, radius, maxDistance, filter);
}

bool bp_point_hits_enemy(Vector3 p)
{
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, g_enemies.pool.iter_list[i]);
        if (!e || e->state == ES_DYING || e->state == ES_DEAD) continue;
        bp_aabb b;
        if (bp_enemy_body_box(e, &b) && bp_aabb_contains_point(&b, p)) return true;
    }
    return false;
}

bool bp_sphere_hits_world(Vector3 p, float radius)
{
    bp_aabb probe = bp_aabb_from_center(p, (Vector3){ radius, radius, radius });
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        if (bp_aabb_overlaps(&probe, &s->box)) return true;
    }
    return false;
}

/* ====================================================================================== */
/* 傷害                                                                                   */
/* ====================================================================================== */
/* 對可破壞的固體（油桶）施加傷害。若油桶被摧毀則回傳 true。 */
static bool damage_barrel(int solidIndex, float amount)
{
    if (solidIndex < 0 || solidIndex >= g_world.solid_count) return false;
    bp_solid *s = &g_world.solids[solidIndex];
    if (!s->explode || s->destroyed) return false;

    s->health -= amount;
    Vector3 c = bp_aabb_center(&s->box);
    if (s->health <= 0.0f)
    {
        s->destroyed = true;
        bp_fx_explosion(c, 4.2f);
        bp_audio_play_at(SFX_EXPLOSION, c, bp_camera_eye(), bp_camera_forward(), 1.0f);
        float pd = 0.0f;
        bp_apply_explosion(c, 5.6f, 92.0f, 10.0f, true, bp_player_position(), &pd);
        if (pd > 0.0f) bp_player_damage(pd * 0.6f, c, 0.5f);
        return true;
    }
    bp_fx_impact_metal(c, (Vector3){ 0, 1, 0 }, 0.4f);
    return false;
}

int bp_apply_explosion(Vector3 center, float radius, float maxDamage, float knockback,
                       bool damagesPlayer, Vector3 playerPos, float *playerDamageOut)
{
    if (playerDamageOut) *playerDamageOut = 0.0f;
    int hits = 0;

    /* ---- 敵人 ---- */
    bp_pool_compact(&g_enemies.pool);
    for (uint32_t i = 0; i < g_enemies.pool.iter_count; i++)
    {
        uint32_t idx = g_enemies.pool.iter_list[i];
        bp_enemy *e = (bp_enemy *)bp_pool_at(&g_enemies.pool, idx);
        if (!e || e->state == ES_DYING || e->state == ES_DEAD) continue;

        Vector3 c = bp_enemy_center(e);
        Vector3 delta = Vector3Subtract(c, center);
        float d = Vector3Length(delta);
        if (d > radius) continue;

        /* 從爆炸中心到敵人胸口的視線（line-of-sight）檢查。少了它，潑濺傷害會穿
         * 牆而過，玩家就會學會不信任這把武器。 */
        if (!bp_world_line_of_sight(center, c))
        {
            /* 牆很薄或屬於柵格（grate）時仍放行部分傷害：掃掠只會在實心幾何上
             * 停下，所以會執行到這裡，就代表真的被擋住了。 */
            continue;
        }

        float falloff = 1.0f - BP_CLAMP(d / radius, 0.0f, 1.0f);
        falloff = falloff * falloff * 0.65f + falloff * 0.35f;   /* 軟化遞減曲線 */
        float dmg = maxDamage * falloff;

        bp_handle h = bp_pool_handle_at(&g_enemies.pool, idx);
        bool killed = false;
        Vector3 dir = (d > 0.01f) ? Vector3Scale(delta, 1.0f / d) : (Vector3){ 0, 1, 0 };
        bp_enemy_take_damage(h, dmg, c, dir, false, knockback * falloff, &killed);
        bp_fx_impact_flesh(c, dir, falloff);
        hits++;
    }

    /* ---- 連鎖引爆其他油桶 ---- */
    for (int i = 0; i < g_world.solid_count; i++)
    {
        bp_solid *s = &g_world.solids[i];
        if (!s->explode || s->destroyed) continue;
        Vector3 c = bp_aabb_center(&s->box);
        if (Vector3Distance(c, center) > radius) continue;
        if (damage_barrel(i, maxDamage * 0.7f)) hits++;
    }

    /* ---- 玩家 ---- */
    if (damagesPlayer)
    {
        Vector3 pp = Vector3Add(playerPos, (Vector3){ 0, 0.9f, 0 });
        float d = Vector3Distance(pp, center);
        if (d <= radius && bp_world_line_of_sight(center, pp))
        {
            float falloff = 1.0f - BP_CLAMP(d / radius, 0.0f, 1.0f);
            falloff = falloff * falloff * 0.6f + falloff * 0.4f;
            if (playerDamageOut) *playerDamageOut = maxDamage * falloff;
        }
    }

    BP_UNUSED(hits);
    return hits;
}

/* ====================================================================================== */
/* 傷害分派                                                                               */
/* ====================================================================================== */
float bp_apply_damage(bp_handle target, const bp_damage_info *info, bool *out_killed)
{
    if (out_killed) *out_killed = false;
    if (!info || info->amount <= 0.0f) return 0.0f;

    bp_enemy *e = bp_enemy_get(target);
    if (!e) return 0.0f;

    bool killed = false;
    bool headshot = info->headshot_multiplier > 1.001f;
    float dealt = bp_enemy_take_damage(target, info->amount, info->point, info->direction,
                                       headshot, info->knockback, &killed);
    if (out_killed) *out_killed = killed;
    return dealt;
}

bool bp_barrel_damage(int solidIndex, float amount) { return damage_barrel(solidIndex, amount); }
