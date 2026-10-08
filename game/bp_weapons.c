/******************************************************************************************
 * bp_weapons.c
 ******************************************************************************************/
#include "bp_weapons.h"
#include "bp_world.h"
#include "bp_fx.h"
#include "bp_enemies.h"
#include "bp_player.h"
#include "../engine/bp_config.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"

bp_weapon_sys g_weapons = { 0 };

/* ====================================================================================== */
/* 1. compiled-in default table (the fallback if assets/cfg/weapons.cfg is missing)         */
/* ====================================================================================== */
static bp_weapon_def s_defaults[WPN_ID_COUNT] = {
    {
        WPN_ID_PISTOL, "pistol", "M9 SIDEARM", 0, WCATEGORY_HITSCAN,
        21.0f, 1, 0.70f, 0.15f, 1.90f, 5.20f, false, 14, 999, 1.05f, 0.32f,
        1.15f, 0.30f, 0.055f, 0.10f,
        SFX_PISTOL_FIRE, SFX_RELOAD_OUT, 0.020f, 0.055f, 180.0f, 1.2f, 0, 14, 1.60f,
        { 216, 222, 233, 255 },
        0, 0, 0, 0, 0, 0, 0,
        MDL_GUN_PISTOL_BODY, MDL_GUN_PISTOL_SLIDE, MDL_GUN_HAND,
        { 0.17f, -0.15f, -0.42f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.02f, -0.36f }
    },
    {
        WPN_ID_SHOTGUN, "shotgun", "SCATTER-12", 1, WCATEGORY_HITSCAN,
        12.0f, 10, 4.60f, 3.20f, 1.50f, 1.15f, false, 6, 72, 2.05f, 0.46f,
        5.20f, 1.10f, 0.200f, 0.42f,
        SFX_SHOTGUN_FIRE, SFX_RELOAD_OUT, 0.030f, 0.045f, 55.0f, 4.5f, 0, 12, 1.40f,
        { 240, 208, 92, 255 },
        0, 0, 0, 0, 0, 0, 0,
        MDL_GUN_SHOTGUN_BODY, MDL_GUN_SHOTGUN_BARREL, MDL_GUN_SHOTGUN_PUMP,
        { 0.18f, -0.16f, -0.44f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.03f, -0.62f }
    },
    {
        WPN_ID_SMG, "smg", "VK-7 VIPER", 2, WCATEGORY_HITSCAN,
        10.0f, 1, 2.10f, 0.85f, 1.40f, 12.50f, true, 36, 288, 1.55f, 0.36f,
        0.62f, 0.44f, 0.038f, 0.07f,
        SFX_SMG_FIRE, SFX_RELOAD_OUT, 0.018f, 0.045f, 140.0f, 0.7f, 0, 48, 1.50f,
        { 92, 226, 196, 255 },
        0, 0, 0, 0, 0, 0, 0,
        MDL_GUN_SMG_BODY, MDL_GUN_SMG_MAG, MDL_GUN_SMG_STOCK,
        { 0.17f, -0.14f, -0.40f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.03f, -0.46f }
    },
    {
        WPN_ID_RAIL, "rail", "LANCE-9 RAILGUN", 3, WCATEGORY_HITSCAN,
        118.0f, 1, 0.02f, 0.00f, 0.35f, 0.82f, false, 4, 32, 2.40f, 0.58f,
        3.40f, 0.25f, 0.260f, 0.52f,
        SFX_RAIL_FIRE, SFX_RELOAD_OUT, 0.070f, 0.220f, 400.0f, 7.0f, 4, 6, 1.20f,
        { 127, 216, 255, 255 },
        0, 0, 0, 0, 0, 0, 0,
        MDL_GUN_RAIL_BODY, MDL_GUN_RAIL_COIL, MDL_GUN_RAIL_CORE,
        { 0.18f, -0.15f, -0.48f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.02f, -0.78f }
    },
    {
        WPN_ID_ROCKET, "rocket", "ANVIL RPG", 4, WCATEGORY_PROJECTILE,
        96.0f, 1, 0.25f, 0.10f, 0.40f, 0.85f, false, 3, 24, 2.75f, 0.62f,
        4.20f, 0.60f, 0.300f, 0.60f,
        SFX_ROCKET_FIRE, SFX_RELOAD_OUT, 0.0f, 0.0f, 220.0f, 11.0f, 0, 4, 1.00f,
        { 255, 138, 32, 255 },
        34.0f, 0.16f, 0.0f, 5.2f, 128.0f, 0.45f, 12.0f,
        MDL_GUN_ROCKET_TUBE, MDL_GUN_ROCKET_SIGHT, MDL_NONE,
        { 0.19f, -0.16f, -0.46f }, { 1.0f, 1.0f, 1.0f }, { 0.0f, 0.04f, -0.80f }
    },
};

const bp_weapon_def *g_weapon_table = s_defaults;

/* ====================================================================================== */
/* 2. config overlay                                                                       */
/* ====================================================================================== */
static int find_weapon_index(const char *id)
{
    for (int i = 0; i < WPN_ID_COUNT; i++)
        if (!strcmp(s_defaults[i].cfg_id, id)) return i;
    return -1;
}

bool bp_weapons_load_config(const char *path)
{
    bp_config cfg;
    if (!bp_config_load(&cfg, path))
    {
        BP_LOGW("weapons: '%s' missing -- using compiled-in defaults", path);
        return false;
    }

    memcpy(g_weapons.def, s_defaults, sizeof(s_defaults));
    int patched = 0;

    while (bp_config_next_section(&cfg))
    {
        if (strcmp(bp_config_section_type(&cfg), "weapon") != 0) continue;
        int idx = find_weapon_index(bp_config_section_id(&cfg));
        if (idx < 0) { BP_LOGW("weapons: unknown id '%s'", bp_config_section_id(&cfg)); continue; }

        bp_weapon_def *d = &g_weapons.def[idx];
        while (bp_config_next_key(&cfg))
        {
            const char *k = cfg.key;
            if      (!strcmp(k, "display_name"))    snprintf(d->display_name, sizeof(d->display_name), "%s", bp_cfg_str(&cfg, d->display_name));
            else if (!strcmp(k, "slot"))            d->slot = bp_cfg_int(&cfg, d->slot);
            else if (!strcmp(k, "category"))        d->category = !strcmp(bp_cfg_str(&cfg, "projectile"), "projectile") ? WCATEGORY_PROJECTILE : WCATEGORY_HITSCAN;
            else if (!strcmp(k, "damage"))          d->damage = bp_cfg_float(&cfg, d->damage);
            else if (!strcmp(k, "pellets"))         d->pellets = bp_cfg_int(&cfg, d->pellets);
            else if (!strcmp(k, "spread_hip"))      d->spread_hip = bp_cfg_float(&cfg, d->spread_hip);
            else if (!strcmp(k, "spread_ads"))      d->spread_ads = bp_cfg_float(&cfg, d->spread_ads);
            else if (!strcmp(k, "spread_move"))     d->spread_move = bp_cfg_float(&cfg, d->spread_move);
            else if (!strcmp(k, "fire_rate"))       d->fire_rate = bp_cfg_float(&cfg, d->fire_rate);
            else if (!strcmp(k, "auto"))            d->full_auto = bp_cfg_bool(&cfg, d->full_auto);
            else if (!strcmp(k, "mag_size"))        d->mag_size = bp_cfg_int(&cfg, d->mag_size);
            else if (!strcmp(k, "reserve_max"))     d->reserve_max = bp_cfg_int(&cfg, d->reserve_max);
            else if (!strcmp(k, "reload_time"))     d->reload_time = bp_cfg_float(&cfg, d->reload_time);
            else if (!strcmp(k, "swap_time"))       d->swap_time = bp_cfg_float(&cfg, d->swap_time);
            else if (!strcmp(k, "recoil_pitch"))    d->recoil_pitch = bp_cfg_float(&cfg, d->recoil_pitch);
            else if (!strcmp(k, "recoil_yaw"))      d->recoil_yaw = bp_cfg_float(&cfg, d->recoil_yaw);
            else if (!strcmp(k, "recoil_kick"))     d->recoil_kick = bp_cfg_float(&cfg, d->recoil_kick);
            else if (!strcmp(k, "shake"))           d->shake = bp_cfg_float(&cfg, d->shake);
            else if (!strcmp(k, "sfx_fire"))        d->sfx_fire = (bp_sfx_id)bp_cfg_int(&cfg, (int)d->sfx_fire);
            else if (!strcmp(k, "sfx_reload"))      d->sfx_reload = (bp_sfx_id)bp_cfg_int(&cfg, (int)d->sfx_reload);
            else if (!strcmp(k, "tracer_width"))    d->tracer_width = bp_cfg_float(&cfg, d->tracer_width);
            else if (!strcmp(k, "tracer_life"))     d->tracer_life = bp_cfg_float(&cfg, d->tracer_life);
            else if (!strcmp(k, "range"))           d->range = bp_cfg_float(&cfg, d->range);
            else if (!strcmp(k, "knockback"))       d->knockback = bp_cfg_float(&cfg, d->knockback);
            else if (!strcmp(k, "penetration"))     d->penetration = bp_cfg_int(&cfg, d->penetration);
            else if (!strcmp(k, "ammo_per_pickup")) d->ammo_per_pickup = bp_cfg_int(&cfg, d->ammo_per_pickup);
            else if (!strcmp(k, "headshot_mult"))   d->headshot_mult = bp_cfg_float(&cfg, d->headshot_mult);
            else if (!strcmp(k, "color"))           d->color = bp_cfg_color(&cfg, d->color);
            else if (!strcmp(k, "proj_speed"))      d->proj_speed = bp_cfg_float(&cfg, d->proj_speed);
            else if (!strcmp(k, "proj_radius"))     d->proj_radius = bp_cfg_float(&cfg, d->proj_radius);
            else if (!strcmp(k, "proj_gravity"))    d->proj_gravity = bp_cfg_float(&cfg, d->proj_gravity);
            else if (!strcmp(k, "blast_radius"))    d->blast_radius = bp_cfg_float(&cfg, d->blast_radius);
            else if (!strcmp(k, "blast_damage"))    d->blast_damage = bp_cfg_float(&cfg, d->blast_damage);
            else if (!strcmp(k, "blast_self"))      d->blast_self = bp_cfg_float(&cfg, d->blast_self);
            else if (!strcmp(k, "blast_knockback")) d->blast_knockback = bp_cfg_float(&cfg, d->blast_knockback);
            else BP_LOGD("weapons: ignoring unknown key '%s'", k);
        }
        patched++;
    }

    g_weapons.def_loaded = true;
    g_weapon_table = g_weapons.def;
    BP_LOGI("weapons: %d/%d definitions patched from config", patched, WPN_ID_COUNT);
    return patched > 0;
}

/* ====================================================================================== */
/* 3. init / loadout                                                                       */
/* ====================================================================================== */
void bp_weapons_init(void)
{
    memcpy(g_weapons.def, s_defaults, sizeof(s_defaults));
    g_weapon_table = g_weapons.def;
    bp_weapons_load_config("assets/cfg/weapons.cfg");
    bp_weapons_reset_loadout(false);
    g_weapons.initialised = true;
}

void bp_weapons_reset_loadout(bool fullArsenal)
{
    for (int i = 0; i < WPN_ID_COUNT; i++)
    {
        g_weapons.owned[i] = fullArsenal || (i == WPN_ID_PISTOL) || (i == WPN_ID_SHOTGUN);
        g_weapons.mag[i] = g_weapons.def[i].mag_size;
        g_weapons.reserve[i] = fullArsenal ? g_weapons.def[i].reserve_max
                                           : (i == WPN_ID_PISTOL ? 999 : 30);
    }
    g_weapons.current = WPN_ID_SHOTGUN;
    g_weapons.pending = g_weapons.current;
    g_weapons.state = WSTATE_IDLE;
    g_weapons.state_time = 0.0f;
    g_weapons.cooldown = 0.0f;
    g_weapons.reload_progress = 0.0f;
    g_weapons.switch_progress = 0.0f;
    g_weapons.spread_current = 0.0f;
    g_weapons.recoil_viewmodel = 0.0f;
    g_weapons.ads = 0.0f;
    g_weapons.shots_fired = 0;
    g_weapons.shots_hit = 0;
    g_weapons.headshots = 0;
    g_weapons.damage_dealt = 0.0f;

    for (int i = 0; i < MAX_PROJECTILES; i++) g_weapons.projectiles[i].alive = false;
    g_weapons.projectile_count = 0;

    if (!g_weapons.owned[g_weapons.current]) g_weapons.current = WPN_ID_PISTOL;
    BP_LOGI("weapons: loadout reset (%s)", fullArsenal ? "full arsenal" : "starting kit");
}

const bp_weapon_def *bp_weapons_current_def(void)
{
    return &g_weapons.def[BP_CLAMP((int)g_weapons.current, 0, WPN_ID_COUNT - 1)];
}

bool bp_weapons_busy(void)
{
    return g_weapons.state == WSTATE_RELOADING ||
           g_weapons.state == WSTATE_SWITCHING_OUT ||
           g_weapons.state == WSTATE_SWITCHING_IN;
}

float bp_weapons_reload_fraction(void) { return BP_CLAMP(g_weapons.reload_progress, 0.0f, 1.0f); }
bool  bp_weapons_owns(bp_weapon_id id) { return id >= 0 && id < WPN_ID_COUNT && g_weapons.owned[id]; }

bool bp_weapons_can_fire(void)
{
    const bp_weapon_def *d = bp_weapons_current_def();
    return g_weapons.state == WSTATE_IDLE && g_weapons.cooldown <= 0.0f &&
           g_weapons.mag[d->id] > 0 && g_weapons.owned[d->id];
}

int bp_weapons_total_reserve(void)
{
    int total = 0;
    for (int i = 0; i < WPN_ID_COUNT; i++) if (g_weapons.owned[i]) total += g_weapons.reserve[i];
    return total;
}

float bp_weapons_accuracy(void)
{
    return g_weapons.shots_fired > 0
         ? (float)g_weapons.shots_hit / (float)g_weapons.shots_fired
         : 0.0f;
}

void bp_weapons_give(bp_weapon_id id, int ammo, bool switchTo)
{
    if (id < 0 || id >= WPN_ID_COUNT) return;
    bool wasNew = !g_weapons.owned[id];
    g_weapons.owned[id] = true;
    if (g_weapons.mag[id] <= 0) g_weapons.mag[id] = g_weapons.def[id].mag_size;
    bp_weapons_add_ammo(id, ammo);
    if (wasNew) BP_LOGI("weapons: acquired %s", g_weapons.def[id].display_name);
    if (switchTo && wasNew) bp_weapons_try_select(id);
}

void bp_weapons_add_ammo(bp_weapon_id id, int amount)
{
    if (id < 0 || id >= WPN_ID_COUNT) return;
    g_weapons.reserve[id] = BP_MIN(g_weapons.def[id].reserve_max, g_weapons.reserve[id] + amount);
}

void bp_weapons_add_reserve_all(int amount)
{
    for (int i = 0; i < WPN_ID_COUNT; i++)
        if (g_weapons.owned[i]) bp_weapons_add_ammo((bp_weapon_id)i, amount);
}

bool bp_weapons_try_select(bp_weapon_id id)
{
    if (id < 0 || id >= WPN_ID_COUNT) return false;
    if (!g_weapons.owned[id] || id == g_weapons.current) return false;
    if (g_weapons.state == WSTATE_SWITCHING_OUT || g_weapons.state == WSTATE_SWITCHING_IN) return false;

    g_weapons.pending = id;
    g_weapons.state = WSTATE_SWITCHING_OUT;
    g_weapons.state_time = 0.0f;
    g_weapons.switch_progress = 0.0f;
    bp_audio_play(SFX_WEAPON_SWAP, 1.0f);
    return true;
}

void bp_weapons_cycle(int direction)
{
    for (int i = 1; i <= WPN_ID_COUNT; i++)
    {
        int idx = ((int)g_weapons.current + direction * i) % WPN_ID_COUNT;
        if (idx < 0) idx += WPN_ID_COUNT;
        if (g_weapons.owned[idx] && idx != (int)g_weapons.current)
        {
            bp_weapons_try_select((bp_weapon_id)idx);
            return;
        }
    }
}

void bp_weapons_try_reload(void)
{
    const bp_weapon_def *d = bp_weapons_current_def();
    if (g_weapons.state != WSTATE_IDLE) return;
    if (g_weapons.mag[d->id] >= d->mag_size) return;
    if (g_weapons.reserve[d->id] <= 0) return;

    g_weapons.state = WSTATE_RELOADING;
    g_weapons.state_time = 0.0f;
    g_weapons.reload_progress = 0.0f;
    bp_audio_play(SFX_RELOAD_OUT, 1.0f);
}

/* ====================================================================================== */
/* 4. firing                                                                               */
/* ====================================================================================== */
static Vector3 spread_direction(Camera3D cam, float spreadDeg)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    if (spreadDeg <= 0.001f) return fwd;

    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam.up));
    Vector3 up = Vector3CrossProduct(right, fwd);

    float rad = spreadDeg * BP_DEG2RAD;
    /* Sample uniformly inside the cone (sqrt for area-uniform distribution, otherwise the
     * pattern clusters in the centre and the weapon feels artificially accurate). */
    float a = bp_rng_range(&g_engine.rng, 0.0f, 2.0f * BP_PI);
    float r = sqrtf(bp_rng_range(&g_engine.rng, 0.0f, 1.0f)) * tanf(rad);

    return Vector3Normalize(Vector3Add(fwd,
        Vector3Add(Vector3Scale(right, cosf(a) * r), Vector3Scale(up, sinf(a) * r))));
}

static void do_hitscan(const bp_weapon_def *d, Camera3D cam, float spread)
{
    Vector3 origin = cam.position;
    bp_trace_filter filter = bp_filter_default();
    filter.max_distance = d->range;

    int pellets = BP_MAX(1, d->pellets);
    bool anyHit = false;
    bool anyHead = false;
    Vector3 firstEnd = origin;

    for (int p = 0; p < pellets; p++)
    {
        Vector3 dir = spread_direction(cam, spread);
        bp_hit hit = bp_trace_ray(origin, dir, d->range, &filter);

        Vector3 end = Vector3Add(origin, Vector3Scale(dir, hit.kind == HIT_NONE ? d->range : hit.distance));
        if (p == 0) firstEnd = end;

        if (hit.kind == HIT_ENEMY)
        {
            anyHit = true;
            if (hit.headshot) anyHead = true;

            bp_damage_info info = { 0 };
            info.amount = d->damage * (hit.headshot ? d->headshot_mult : 1.0f);
            info.point = hit.point;
            info.direction = dir;
            info.knockback = d->knockback;
            info.from_player = true;
            info.headshot_multiplier = d->headshot_mult;

            bool killed = false;
            float dealt = bp_apply_damage(hit.entity, &info, &killed);
            g_weapons.damage_dealt += dealt;
            bp_fx_hit_marker(killed);
            bp_fx_impact_flesh(hit.point, dir, BP_CLAMP(dealt / 60.0f, 0.2f, 1.6f));
            bp_audio_play_at(hit.headshot ? SFX_HIT_ARMOR : SFX_HIT_FLESH,
                             hit.point, cam.position, dir, 1.0f);
        }
        else if (hit.kind == HIT_BARREL)
        {
            anyHit = true;
            bp_damage_info info = { 0 };
            info.amount = d->damage;
            info.point = hit.point;
            info.direction = dir;
            info.from_player = true;
            bool killed = false;
            bp_apply_damage(hit.entity, &info, &killed);
            bp_fx_impact_metal(hit.point, hit.normal, 0.7f);
        }
        else if (hit.kind == HIT_WORLD)
        {
            const bp_material_def *md = bp_world_material(hit.material);
            bp_fx_impact_wall(hit.point, hit.normal, BP_CLAMP(d->damage / 60.0f, 0.2f, 1.3f));
            bp_audio_play_at((bp_sfx_id)md->impact_sfx, hit.point, cam.position, dir, 0.85f);
            if (bp_rng_range(&g_engine.rng, 0.0f, 1.0f) < 0.25f)
                bp_audio_play_at(SFX_RICOCHET, hit.point, cam.position, dir, 0.5f);
        }
    }

    g_weapons.shots_fired++;
    if (anyHit) g_weapons.shots_hit++;
    if (anyHead) g_weapons.headshots++;

    /* One tracer for the whole shot rather than one per pellet: ten overlapping shotgun
     * tracers read as a white blob and cost ten times the fill rate. */
    if (d->tracer_width > 0.0f && d->tracer_life > 0.0f)
    {
        Vector3 muzzle = Vector3Add(origin, Vector3Scale(bp_camera_forward(), 0.6f));
        bp_fx_tracer(muzzle, firstEnd, d->tracer_width * (pellets > 1 ? 2.2f : 1.0f),
                     d->tracer_life, d->color);
    }
}

static void do_projectile(const bp_weapon_def *d, Camera3D cam, Vector3 origin)
{
    Vector3 dir = spread_direction(cam, d->spread_hip * (1.0f - g_weapons.ads * 0.6f));
    bp_projectile_spawn(origin, dir, d->proj_speed, d->damage, d->proj_radius,
                        d->blast_radius, true, d->color, MDL_PROJECTILE_ROCKET, BP_NULL_HANDLE);

    bp_projectile *p = NULL;
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (g_weapons.projectiles[i].alive) { p = &g_weapons.projectiles[i]; break; }
    if (p)
    {
        p->gravity = d->proj_gravity;
        p->knockback = d->blast_knockback;
        p->self_scale = d->blast_self;
        p->life = d->range / BP_MAX(d->proj_speed, 1.0f);
    }
}

void bp_weapons_try_fire(bool triggerHeld, bool triggerPressed)
{
    const bp_weapon_def *d = bp_weapons_current_def();
    if (!bp_weapons_can_fire())
    {
        /* Dry fire click. Giving audio feedback for an empty chamber is what stops the
         * player from thinking the input was dropped -- a surprisingly common complaint in
         * playtests of shooters that omit it. */
        if (triggerPressed && g_weapons.state == WSTATE_IDLE && g_weapons.mag[d->id] <= 0 &&
            (GetTime() - (double)g_weapons.dry_fire_time) > 0.28)
        {
            g_weapons.dry_fire_time = (float)GetTime();
            bp_audio_play(SFX_PISTOL_DRY, 1.0f);
            bp_weapons_try_reload();
        }
        return;
    }

    bool shouldFire = d->full_auto ? triggerHeld : triggerPressed;
    if (!shouldFire) return;

    g_weapons.mag[d->id]--;
    g_weapons.cooldown = 1.0f / BP_MAX(d->fire_rate, 0.05f);
    g_weapons.state = WSTATE_FIRING;
    g_weapons.state_time = 0.0f;
    g_weapons.recoil_viewmodel = 1.0f;

    float spread = g_weapons.spread_current;
    Camera3D cam = bp_camera_render(1.0f);

    if (d->category == WCATEGORY_PROJECTILE) do_projectile(d, cam, cam.position);
    else                                     do_hitscan(d, cam, spread);

    /* audio + visual feedback */
    bp_audio_play(d->sfx_fire, 1.0f);
    Vector3 fwd = bp_camera_forward();
    Vector3 muzzle = Vector3Add(cam.position, Vector3Scale(fwd, 0.85f));
    bp_fx_muzzle_flash(muzzle, fwd, d->category == WCATEGORY_PROJECTILE ? 1.4f : 1.0f);
    bp_camera_add_recoil(d->recoil_pitch * bp_rng_range(&g_engine.rng, 0.85f, 1.15f),
                         d->recoil_yaw * bp_rng_range(&g_engine.rng, -1.0f, 1.0f));
    bp_fx_shake(d->shake, 0.18f);

    /* shell ejection to the player's right */
    bp_fx_shell_eject(Vector3Add(cam.position, Vector3Scale(fwd, 0.35f)),
                      bp_camera_right(), (Vector3){ 0, 1, 0 });

    /* recoil also drives the speed-line post effect on the heaviest guns */
    if (d->shake > 0.35f) g_gfx.speed_lines = BP_MIN(1.0f, g_gfx.speed_lines + 0.35f);

    if (g_weapons.mag[d->id] <= 0)
    {
        g_weapons.state = WSTATE_EMPTY;
        bp_weapons_try_reload();
    }
}

/* ====================================================================================== */
/* 5. projectiles                                                                          */
/* ====================================================================================== */
void bp_projectile_spawn(Vector3 pos, Vector3 dir, float speed, float damage,
                         float radius, float blastRadius, bool fromPlayer, Color color,
                         bp_model_id model, bp_handle owner)
{
    bp_projectile *p = NULL;
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (!g_weapons.projectiles[i].alive) { p = &g_weapons.projectiles[i]; break; }
    if (!p) { p = &g_weapons.projectiles[0]; }   /* recycle: never refuse to fire */

    memset(p, 0, sizeof(*p));
    p->pos = pos;
    p->vel = Vector3Scale(Vector3Normalize(dir), speed);
    p->damage = damage;
    p->radius = blastRadius;
    p->body_radius = radius;
    p->life = 8.0f;
    p->age = 0.0f;
    p->from_player = fromPlayer;
    p->alive = true;
    p->trail = true;
    p->color = color;
    p->model = model;
    p->owner = owner;
    p->gravity = 0.0f;
    p->knockback = 8.0f;
    p->self_scale = fromPlayer ? 0.45f : 0.0f;
}

void bp_projectile_explode(bp_projectile *p, Vector3 at)
{
    if (!p->alive) return;
    p->alive = false;

    float r = BP_MAX(p->radius, 0.4f);
    bp_fx_explosion(at, r);
    bp_audio_play_at(SFX_EXPLOSION, at, bp_camera_eye(), bp_camera_forward(), 1.0f);
    bp_fx_shake(BP_MIN(1.3f, 0.25f + r * 0.09f), 0.5f);

    float playerDamage = 0.0f;
    int hit = bp_apply_explosion(at, r * 1.35f, p->damage, p->knockback,
                                 p->from_player, bp_player_position(), &playerDamage);

    if (p->from_player && playerDamage > 0.0f)
    {
        /* Rocket jumping must cost something but never be a suicide button: self damage is
         * scaled down and, crucially, the *knockback* is full strength. That asymmetry is
         * the whole reason rocket jumping is a movement technique rather than a hazard. */
        bp_player_damage(playerDamage * p->self_scale, at, 0.0f);
        Vector3 away = Vector3Subtract(bp_player_position(), at);
        float d = Vector3Length(away);
        if (d > 0.01f)
        {
            away = Vector3Scale(away, 1.0f / d);
            float falloff = 1.0f - BP_CLAMP(d / (r * 1.35f), 0.0f, 1.0f);
            bp_player_impulse(Vector3Add(Vector3Scale(away, p->knockback * falloff * 1.25f),
                                         (Vector3){ 0, p->knockback * falloff * 0.85f, 0 }));
        }
    }
    BP_UNUSED(hit);
}

static void update_projectiles(float dt, Camera3D cam)
{
    BP_UNUSED(cam);
    g_weapons.projectile_count = 0;

    for (int i = 0; i < MAX_PROJECTILES; i++)
    {
        bp_projectile *p = &g_weapons.projectiles[i];
        if (!p->alive) continue;
        g_weapons.projectile_count++;

        p->age += dt;
        if (p->age >= p->life) { bp_projectile_explode(p, p->pos); continue; }

        p->vel.y += p->gravity * dt;

        /* Sub-step the movement so a fast rocket cannot tunnel through thin geometry. At
         * 34 m/s and 120 Hz a single step is 28 cm, which is thinner than some wall trim,
         * so we integrate in slices no longer than the projectile's own body radius. */
        float speed = Vector3Length(p->vel);
        int steps = BP_MAX(1, BP_MIN(6, (int)ceilf(speed * dt / BP_MAX(p->body_radius, 0.08f))));
        float sub = dt / (float)steps;

        bp_trace_filter filter = bp_filter_projectile(p->owner);
        filter.max_distance = speed * sub + p->body_radius;

        for (int s = 0; s < steps && p->alive; s++)
        {
            Vector3 dir = (speed > 0.001f) ? Vector3Scale(p->vel, 1.0f / speed) : (Vector3){ 0, -1, 0 };
            float moveLen = speed * sub;
            bp_hit hit = bp_trace_sphere(p->pos, dir, p->body_radius, moveLen, &filter);

            if (hit.kind == HIT_ENEMY)
            {
                bp_damage_info info = { 0 };
                info.amount = p->damage;
                info.point = hit.point;
                info.direction = dir;
                info.knockback = p->knockback;
                info.from_player = p->from_player;
                info.explosive = true;
                bool killed = false;
                float dealt = bp_apply_damage(hit.entity, &info, &killed);
                if (p->from_player) { g_weapons.damage_dealt += dealt; g_weapons.shots_hit++; bp_fx_hit_marker(killed); }
                bp_projectile_explode(p, hit.point);
                break;
            }
            else if (hit.kind == HIT_WORLD || hit.kind == HIT_BARREL)
            {
                if (hit.kind == HIT_BARREL)
                {
                    bp_damage_info info = { 0 };
                    info.amount = p->damage;
                    info.point = hit.point;
                    info.direction = dir;
                    info.from_player = p->from_player;
                    info.explosive = true;
                    bool killed = false;
                    bp_apply_damage(hit.entity, &info, &killed);
                }
                bp_projectile_explode(p, hit.point);
                break;
            }

            p->pos = Vector3Add(p->pos, Vector3Scale(p->vel, sub));

            if (p->trail)
            {
                bp_fx_smoke_trail(p->pos, p->from_player ? 1.0f : 0.5f);
                bp_fx_particle(PTX_EMBER, p->pos,
                               Vector3Scale(p->vel, -0.06f), 0.22f, 0.16f, 0.01f,
                               (Color){ 255, 210, 130, 230 }, (Color){ 255, 90, 20, 0 });
            }
        }
    }
}

void bp_weapons_draw_projectiles(Camera3D cam)
{
    BP_UNUSED(cam);
    for (int i = 0; i < MAX_PROJECTILES; i++)
    {
        bp_projectile *p = &g_weapons.projectiles[i];
        if (!p->alive) continue;

        float speed = Vector3Length(p->vel);
        Vector3 axis = (speed > 0.01f) ? Vector3Scale(p->vel, 1.0f / speed) : (Vector3){ 0, 0, 1 };
        Vector3 yawAxis = Vector3CrossProduct((Vector3){ 0, 1, 0 }, axis);
        float yawLen = Vector3Length(yawAxis);
        float angle = asinf(BP_CLAMP(axis.y, -1.0f, 1.0f)) * BP_RAD2DEG;
        if (yawLen > 0.001f) yawAxis = Vector3Scale(yawAxis, 1.0f / yawLen);
        else yawAxis = (Vector3){ 1, 0, 0 };

        if (bp_model_valid(p->model))
            DrawModelEx(bp_model_get(p->model), p->pos, yawAxis, angle, (Vector3){ 1, 1, 1 }, WHITE);
        else
            DrawSphere(p->pos, p->body_radius * 2.0f, p->color);

        /* additive halo: sells the projectile as a light source in a renderer with none */
        rlSetBlendMode(RL_BLEND_ADDITIVE);
        DrawSphere(p->pos, p->body_radius * 3.4f, (Color){ p->color.r, p->color.g, p->color.b, 60 });
        rlSetBlendMode(RL_BLEND_ALPHA);
    }
}

/* ====================================================================================== */
/* 6. per-frame update                                                                     */
/* ====================================================================================== */
void bp_weapons_update(float dt, Camera3D cam, Vector3 velocity, bool onGround)
{
    const bp_weapon_def *d = bp_weapons_current_def();
    BP_UNUSED(cam);

    if (g_weapons.cooldown > 0.0f) g_weapons.cooldown -= dt;
    g_weapons.state_time += dt;
    g_weapons.recoil_viewmodel = bp_damp(g_weapons.recoil_viewmodel, 0.0f, 11.0f, dt);

    /* ---- spread model ----
     * Aim-down-sights tightens the cone; movement widens it; being airborne widens it most.
     * This is the contract with the player: stand still to be precise, or move and accept
     * the cone. Both are valid, which is what keeps movement and shooting in tension. */
    float speed = Vector3Length(velocity);
    float moveFactor = BP_CLAMP(speed / 9.0f, 0.0f, 1.6f) * d->spread_move;
    float airFactor = onGround ? 0.0f : 1.1f;
    /* ads_held is published by the player controller before this call */
    g_weapons.ads = bp_damp(g_weapons.ads, g_weapons.ads_held ? 1.0f : 0.0f, 13.0f, dt);
    float targetSpread = BP_MAX(0.0f, d->spread_hip + moveFactor + airFactor);
    targetSpread = targetSpread * (1.0f - g_weapons.ads * 0.65f);
    g_weapons.spread_current = bp_damp(g_weapons.spread_current, targetSpread, 12.0f, dt);

    /* ---- state machine ---- */
    switch (g_weapons.state)
    {
        case WSTATE_RELOADING:
        {
            g_weapons.reload_progress = BP_CLAMP(g_weapons.state_time / BP_MAX(d->reload_time, 0.05f), 0.0f, 1.0f);
            /* shell/insert click at the midpoint */
            if (g_weapons.state_time >= d->reload_time * 0.45f && g_weapons.state_time - dt < d->reload_time * 0.45f)
                bp_audio_play(SFX_RELOAD_IN, 1.0f);

            if (g_weapons.state_time >= d->reload_time)
            {
                int need = d->mag_size - g_weapons.mag[d->id];
                int take = BP_MIN(need, g_weapons.reserve[d->id]);
                g_weapons.mag[d->id] += take;
                g_weapons.reserve[d->id] -= take;
                g_weapons.state = WSTATE_IDLE;
                g_weapons.reload_progress = 0.0f;
                bp_audio_play(SFX_RELOAD_DONE, 1.0f);
            }
            break;
        }
        case WSTATE_SWITCHING_OUT:
        {
            float t = g_weapons.state_time / BP_MAX(d->swap_time * 0.5f, 0.05f);
            g_weapons.switch_progress = BP_CLAMP(t, 0.0f, 1.0f);
            if (t >= 1.0f)
            {
                g_weapons.current = g_weapons.pending;
                g_weapons.state = WSTATE_SWITCHING_IN;
                g_weapons.state_time = 0.0f;
                g_weapons.switch_progress = 0.0f;
                g_weapons.cooldown = BP_MAX(g_weapons.cooldown, 0.05f);
            }
            break;
        }
        case WSTATE_SWITCHING_IN:
        {
            const bp_weapon_def *nd = bp_weapons_current_def();
            float t = g_weapons.state_time / BP_MAX(nd->swap_time * 0.5f, 0.05f);
            g_weapons.switch_progress = BP_CLAMP(t, 0.0f, 1.0f);
            if (t >= 1.0f) { g_weapons.state = WSTATE_IDLE; g_weapons.switch_progress = 0.0f; }
            break;
        }
        case WSTATE_FIRING:
            if (g_weapons.state_time > 0.06f) g_weapons.state = WSTATE_IDLE;
            break;
        case WSTATE_EMPTY:
            if (g_weapons.mag[d->id] > 0) g_weapons.state = WSTATE_IDLE;
            else if (g_weapons.state_time > 0.25f && g_weapons.reserve[d->id] > 0) bp_weapons_try_reload();
            break;
        case WSTATE_IDLE:
        default:
            break;
    }

    /* ---- viewmodel sway ----
     * Driven by look delta, not by mouse position: the gun lags the turn and settles back.
     * This is the single cheapest way to make a low-poly viewmodel feel weighted. */
    g_weapons.sway_x = bp_damp(g_weapons.sway_x,
                               BP_CLAMP(-bp_input_look_dx() * 0.02f, -1.0f, 1.0f), 9.0f, dt);
    g_weapons.sway_y = bp_damp(g_weapons.sway_y,
                               BP_CLAMP(-bp_input_look_dy() * 0.02f, -1.0f, 1.0f), 9.0f, dt);

    update_projectiles(dt, cam);
}

/* ====================================================================================== */
/* 7. viewmodel                                                                            */
/* ====================================================================================== */
/*
 * Draw a single viewmodel part in the current orthographic/modelview state.
 * The weapon root transform is already in RLGL.State.modelview, so `local` only has to
 * carry any extra per-part offset (currently identity for all parts).
 */
static void draw_vm_part(bp_model_id id, Vector3 scale, Matrix local, Color tint)
{
    if (!bp_model_valid(id)) return;
    Model m = bp_model_get(id);
    if (m.meshCount <= 0) return;

    Color saved = m.materials[0].maps[MATERIAL_MAP_DIFFUSE].color;
    m.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = tint;
    Matrix xform = MatrixMultiply(local, MatrixScale(scale.x, scale.y, scale.z));
    for (int i = 0; i < m.meshCount; i++)
        if (m.meshes[i].vertexCount > 0) DrawMesh(m.meshes[i], m.materials[0], xform);
    m.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = saved;
}

void bp_weapons_draw_viewmodel(Camera3D cam, float speed01)
{
    BP_UNUSED(cam);
    const bp_weapon_def *d = bp_weapons_current_def();

    /* Lower the gun while it is being holstered, raise it while drawing. Without this the
     * swap reads as a teleport instead of a physical action. */
    float holster = 0.0f;
    if (g_weapons.state == WSTATE_SWITCHING_OUT) holster = g_weapons.switch_progress;
    else if (g_weapons.state == WSTATE_SWITCHING_IN) holster = 1.0f - g_weapons.switch_progress;

    float reloadDip = (g_weapons.state == WSTATE_RELOADING)
                    ? sinf(g_weapons.reload_progress * BP_PI) * 0.34f : 0.0f;

    /* The viewmodel is rendered with its own orthographic projection so it can never clip
     * into world geometry. This is the standard FPS solution and matters a lot in tight
     * corridors, where a perspective viewmodel would intersect every wall the player hugs. */
    Camera3D vm = { 0 };
    vm.position = (Vector3){ 0, 0, 1.0f };
    vm.target = (Vector3){ 0, 0, 0 };
    vm.up = (Vector3){ 0, 1, 0 };
    vm.fovy = 45.0f;
    vm.projection = CAMERA_ORTHOGRAPHIC;

    float halfH = 0.235f;
    float aspect = (float)GetScreenWidth() / (float)BP_MAX(GetScreenHeight(), 1);

    BeginMode3D(vm);
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    rlOrtho(-halfH * aspect, halfH * aspect, -halfH, halfH, 0.01, 12.0);
    rlMatrixMode(RL_MODELVIEW);
    rlPushMatrix();
    rlLoadIdentity();

    Vector3 base = d->vm_offset;
    base.x += g_weapons.sway_x * 0.055f + g_camera.vm_position.x;
    base.y += g_weapons.sway_y * 0.035f + g_camera.vm_position.y - holster * 0.30f - reloadDip * 0.22f;
    base.z += g_camera.vm_position.z - holster * 0.10f;

    /*
     * Full Euler rotation in degrees, built the same way the camera builds its own view
     * matrix: roll about Z, pitch about X, yaw about Y. Composing this once per weapon and
     * pushing it through rlSetMatrixModelview() lets DrawMesh() place the parts exactly,
     * which DrawModelEx's single axis/angle pair cannot express.
     */
    float rotX = g_camera.vm_rotation.x - reloadDip * 42.0f - holster * 18.0f + g_weapons.sway_y * 4.0f;
    float rotY = g_camera.vm_rotation.y + g_weapons.sway_x * 5.0f;
    float rotZ = g_camera.vm_rotation.z - reloadDip * 16.0f + g_weapons.sway_x * 2.5f;

    Matrix vmRot = MatrixMultiply(MatrixMultiply(MatrixRotateZ(rotZ * BP_DEG2RAD),
                                                 MatrixRotateX(rotX * BP_DEG2RAD)),
                                  MatrixRotateY(rotY * BP_DEG2RAD));
    Matrix vmXform = MatrixMultiply(vmRot, MatrixTranslate(base.x, base.y, base.z));
    Matrix ident = MatrixIdentity();
    Color tint = (Color){ 232, 232, 238, 255 };

    /* Draw order: stock/magazine first, body, then the barrel-ish part, then the hand on
     * top. Overlapping convex parts sorted back-to-front avoids needing a depth-correct
     * merged mesh, which we cannot afford to rebuild per frame. */
    rlSetMatrixModelview(vmXform);
    draw_vm_part(d->vm_part_c, d->vm_scale, ident, tint);
    draw_vm_part(d->vm_part_b, d->vm_scale, ident, tint);
    draw_vm_part(d->vm_body,   d->vm_scale, ident, tint);

    /* muzzle flash sprite in viewmodel space, additive */
    if (g_fx.muzzle_flash > 0.02f)
    {
        Vector3 mz = Vector3Add(base, d->vm_muzzle);
        Texture2D soft = bp_tex(TEX_PARTICLE_SOFT);
        float s = 0.16f + g_fx.muzzle_flash * 0.20f;
        rlSetBlendMode(RL_BLEND_ADDITIVE);
        if (soft.id) DrawBillboard(vm, soft, mz, s, (Color){ 255, 236, 190,
                       (unsigned char)(g_fx.muzzle_flash * 235.0f) });
        rlSetBlendMode(RL_BLEND_ALPHA);
    }

    /* reload indicator arc drawn in the same ortho space so it tracks the gun exactly */
    if (g_weapons.state == WSTATE_RELOADING)
    {
        rlPopMatrix();
        rlMatrixMode(RL_PROJECTION);
        rlPopMatrix();
        EndMode3D();

        int w = GetScreenWidth(), h = GetScreenHeight();
        float cx = (float)w * 0.5f + base.x * 900.0f;
        float cy = (float)h * 0.62f;
        float rad = 26.0f;
        DrawRing((Vector2){ cx, cy }, rad - 4.0f, rad, 0.0f, 360.0f, 26, (Color){ 255, 255, 255, 42 });
        DrawRing((Vector2){ cx, cy }, rad - 4.0f, rad, -90.0f,
                 -90.0f + 360.0f * g_weapons.reload_progress, 26, d->color);
        return;
    }

    rlPopMatrix();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    EndMode3D();

    BP_UNUSED(speed01);
    BP_UNUSED(holster);
}
