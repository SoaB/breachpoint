/******************************************************************************************
 * bp_hud.c -- heads up display implementation.
 *
 * Everything here is drawn in the 640x360 virtual canvas (see bp_ui.h) so the HUD layout is
 * identical at every resolution. The only element that deliberately breaks that rule is the
 * crosshair, which is pinned to the exact screen centre in device pixels: it must coincide
 * with the camera's centre ray or the gun does not shoot where you look.
 ******************************************************************************************/
#include "bp_hud.h"

#include "bp_ui.h"
#include "bp_player.h"
#include "bp_weapons.h"
#include "bp_enemies.h"
#include "bp_waves.h"
#include "bp_world.h"
#include "bp_fx.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"
#include "../engine/bp_audio.h"

#include <stdio.h>
#include <string.h>

bp_hud g_hud = { 0 };

/* -------------------------------------------------------------------------------------- */
/* layout: 640x360 virtual                                                                 */
/* -------------------------------------------------------------------------------------- */
#define HUD_MARGIN        8.0f
#define HUD_PANEL_H      44.0f
#define HUD_VITALS_W    168.0f
#define HUD_WEAPON_W    150.0f
#define HUD_TOPBAR_H      3.0f
#define HUD_TOAST_Y      96.0f
#define HUD_BANNER_Y    128.0f
#define HUD_DIR_RADIUS   52.0f
#define HUD_HEALTH_SEG     10

/* -------------------------------------------------------------------------------------- */
/* helpers                                                                                 */
/* -------------------------------------------------------------------------------------- */
static inline Color hud_col(float r, float g, float b, float a)
{
    return (Color){ (unsigned char)BP_CLAMP((int)r, 0, 255), (unsigned char)BP_CLAMP((int)g, 0, 255),
                    (unsigned char)BP_CLAMP((int)b, 0, 255), (unsigned char)BP_CLAMP((int)a, 0, 255) };
}

static Color health_color(float frac)
{
    /* Green -> amber -> red. The colour is the primary readout and the number is the
     * confirmation, so the ramp has to be distinguishable in peripheral vision: that means
     * changing HUE (not just brightness) at every step. */
    if (frac > 0.60f) return g_assets.pal_health;
    if (frac > 0.30f) return g_assets.pal_ammo;
    return g_assets.pal_danger;
}

static void push_toast(const char *text, bp_toast_kind kind, float life)
{
    /* Oldest toast is overwritten when the queue is full. Dropping the *new* message would
     * hide the most relevant information exactly when the screen is busiest. */
    bp_toast *t;
    if (g_hud.toast_count < HUD_MAX_TOASTS)
    {
        t = &g_hud.toasts[g_hud.toast_count++];
        t->age = 0.0f;
        t->y = 0.0f;
    }
    else
    {
        t = &g_hud.toasts[0];
        float oldest = -1.0f;
        for (int i = 0; i < HUD_MAX_TOASTS; i++)
            if (g_hud.toasts[i].age > oldest) { oldest = g_hud.toasts[i].age; t = &g_hud.toasts[i]; }
    }
    snprintf(t->text, sizeof(t->text), "%s", text);
    t->kind = kind;
    t->life = life > 0.1f ? life : 2.2f;
    t->age = 0.0f;
    t->y = 0.0f;
}

/* -------------------------------------------------------------------------------------- */
/* public events                                                                           */
/* -------------------------------------------------------------------------------------- */
void bp_hud_toast(const char *text, bp_toast_kind kind, float life)
{
    if (!text) return;
    push_toast(text, kind, life);
}

void bp_hud_banner(const char *title, const char *subtitle, float life)
{
    snprintf(g_hud.banner, sizeof(g_hud.banner), "%s", title ? title : "");
    snprintf(g_hud.banner_sub, sizeof(g_hud.banner_sub), "%s", subtitle ? subtitle : "");
    g_hud.banner_time = life > 0.2f ? life : 2.6f;
}

void bp_hud_notify_damage(float angle, float strength)
{
    if (g_hud.dir_count >= HUD_MAX_DMGDIRS)
    {
        /* Replace the oldest indicator rather than refusing the new one: the most recent hit
         * is the one worth pointing at. */
        int oldest = 0;
        for (int i = 1; i < HUD_MAX_DMGDIRS; i++)
            if (g_hud.dirs[i].age > g_hud.dirs[oldest].age) oldest = i;
        bp_dmgdir *d = &g_hud.dirs[oldest];
        d->angle = angle; d->age = 0.0f; d->life = 1.1f; d->strength = strength;
        return;
    }
    bp_dmgdir *d = &g_hud.dirs[g_hud.dir_count++];
    d->angle = angle; d->age = 0.0f; d->life = 1.1f; d->strength = strength;
}

void bp_hud_set_visible(bool visible) { g_hud.visible = visible; }

/* -------------------------------------------------------------------------------------- */
/* init / update                                                                           */
/* -------------------------------------------------------------------------------------- */
void bp_hud_init(void)
{
    memset(&g_hud, 0, sizeof(g_hud));
    g_hud.visible = true;
    g_hud.health = PLAYER_MAX_HEALTH;
    g_hud.armor = 0.0f;
    g_hud.show_fps = false;
}

void bp_hud_reset(void)
{
    g_hud.toast_count = 0;
    g_hud.dir_count = 0;
    g_hud.banner_time = 0.0f;
    g_hud.banner[0] = '\0';
    g_hud.banner_sub[0] = '\0';
    g_hud.hurt_vignette = 0.0f;
    g_hud.health = g_player.health;
    g_hud.armor = g_player.armor;
    g_hud.speed = 0.0f;
}

void bp_hud_update(float dt)
{
    g_hud.time += dt;

    /* Smoothing. Every readable value is damped rather than assigned: a health number that
     * jumps 34 -> 21 in one frame is unreadable, and the eye tracks a moving bar far better
     * than a teleporting one. lambda is chosen so the value settles in ~0.25 s. */
    g_hud.health = bp_damp(g_hud.health, g_player.health, 12.0f, dt);
    g_hud.armor  = bp_damp(g_hud.armor,  g_player.armor,  12.0f, dt);

    /* Speed is smoothed harder: a readout that twitches with every footstep is noise. */
    float hs = Vector3Length((Vector3){ g_player.vel.x, 0.0f, g_player.vel.z });
    g_hud.speed = bp_damp(g_hud.speed, hs, 8.0f, dt);

    const bp_weapon_def *wd = bp_weapons_current_def();
    g_hud.ammo_mag = wd ? g_weapons.mag[wd->id] : 0;
    g_hud.ammo_reserve = wd ? g_weapons.reserve[wd->id] : 0;

    /* Feed the post shader. The HUD owns this because it owns the visual language of
     * "you are hurt" / "you are fast"; the renderer just executes it. */
    bp_gfx_flash_damage(g_player.damage_flash * dt * 2.2f);
    bp_gfx_flash_heal(g_player.heal_flash * dt * 1.6f);
    g_gfx.speed_lines = BP_CLAMP((g_hud.speed - g_player.speed_ground) /
                                 BP_MAX(g_player.speed_sprint * 1.4f - g_player.speed_ground, 1.0f), 0.0f, 1.0f);

    g_hud.hurt_vignette = BP_MAX(g_player.damage_flash, g_hud.hurt_vignette - dt * 1.8f);

    /* low ammo pulse: only when the magazine is genuinely low relative to this weapon */
    float magFrac = (wd && wd->mag_size > 0) ? (float)g_hud.ammo_mag / (float)wd->mag_size : 1.0f;
    g_hud.low_ammo_pulse = (magFrac <= 0.25f && g_hud.ammo_mag > 0)
                         ? 0.5f + 0.5f * sinf(g_hud.time * 7.0f)
                         : 0.0f;

    g_hud.intermission_left = (g_waves.state == WAVE_INTERMISSION) ? g_waves.intermission_left : 0.0f;

    /* banners + toasts */
    if (g_hud.banner_time > 0.0f) g_hud.banner_time -= dt;
    for (int i = 0; i < g_hud.toast_count; i++)
    {
        bp_toast *t = &g_hud.toasts[i];
        t->age += dt;
        t->y = bp_damp(t->y, (float)i, 16.0f, dt);   /* slots re-flow as toasts expire */
    }
    /* compact out the expired ones, keeping order */
    int w = 0;
    for (int i = 0; i < g_hud.toast_count; i++)
    {
        if (g_hud.toasts[i].age < g_hud.toasts[i].life) g_hud.toasts[w++] = g_hud.toasts[i];
    }
    g_hud.toast_count = w;

    /* damage direction indicators */
    for (int i = 0; i < g_hud.dir_count; i++) g_hud.dirs[i].age += dt;
    w = 0;
    for (int i = 0; i < g_hud.dir_count; i++)
        if (g_hud.dirs[i].age < g_hud.dirs[i].life) g_hud.dirs[w++] = g_hud.dirs[i];
    g_hud.dir_count = w;
}

/* -------------------------------------------------------------------------------------- */
/* pieces                                                                                  */
/* -------------------------------------------------------------------------------------- */
static void draw_vitals(void)
{
    float x = HUD_MARGIN;
    float y = (float)UI_VH - HUD_MARGIN - HUD_PANEL_H;
    float w = HUD_VITALS_W;
    float h = HUD_PANEL_H;

    float hpFrac = BP_CLAMP(g_hud.health / BP_MAX(g_player.max_health, 1.0f), 0.0f, 1.0f);
    float arFrac = BP_CLAMP(g_hud.armor / BP_MAX(g_player.max_armor, 1.0f), 0.0f, 1.0f);
    Color hpCol = health_color(hpFrac);

    /* The plate flashes red on the frame you are hit. A 2px inset flash reads far better
     * than tinting the whole panel, which would obscure the numbers. */
    Color border = hud_col(hpCol.r, hpCol.g, hpCol.b, 90 + g_hud.hurt_vignette * 165.0f);
    bp_ui_panel_notch(x, y, w, h, 6.0f, g_assets.pal_panel, border);

    /* health */
    float barX = x + 8.0f, barW = w - 16.0f;
    bp_ui_text(barX, y + 4.0f, UI_FS_TINY, g_assets.pal_text_dim, "INTEGRITY");
    bp_ui_bar_segmented(barX, y + 12.0f, barW, 9.0f, hpFrac, HUD_HEALTH_SEG,
                        hpCol, (Color){ 10, 11, 14, 220 }, (Color){ 0, 0, 0, 140 });
    bp_ui_text_align(x + w - 8.0f, y + 22.0f, UI_FS_LARGE, UI_ALIGN_RIGHT, hpCol,
                     "%d", (int)(g_hud.health + 0.5f));

    /* armour */
    bp_ui_text(barX, y + 26.0f, UI_FS_TINY, g_assets.pal_text_dim, "PLATING");
    bp_ui_bar(barX, y + 34.0f, barW * 0.62f, 6.0f, arFrac,
              g_assets.pal_armor, (Color){ 10, 11, 14, 220 }, (Color){ 0, 0, 0, 140 });
    if (g_hud.armor > 0.5f)
        bp_ui_text_align(x + w - 8.0f, y + 31.0f, UI_FS_SMALL, UI_ALIGN_RIGHT, g_assets.pal_armor,
                   "%d", (int)(g_hud.armor + 0.5f));
    else
        bp_ui_text_align(x + w - 8.0f, y + 31.0f, UI_FS_SMALL, UI_ALIGN_RIGHT,
                   (Color){ 70, 74, 84, 200 }, "--");
}

static void draw_weapon(void)
{
    const bp_weapon_def *wd = bp_weapons_current_def();
    float w = HUD_WEAPON_W;
    float x = (float)UI_VW - HUD_MARGIN - w;
    float y = (float)UI_VH - HUD_MARGIN - HUD_PANEL_H;

    bp_ui_panel_notch(x, y, w, HUD_PANEL_H, 6.0f, g_assets.pal_panel, (Color){ 255, 138, 32, 70 });

    if (!wd) return;

    bool empty = (g_hud.ammo_mag <= 0);
    Color ammoCol = empty ? g_assets.pal_danger
                  : (g_hud.low_ammo_pulse > 0.5f ? bp_ui_mix(g_assets.pal_ammo, g_assets.pal_danger, 0.6f)
                                                 : g_assets.pal_ammo);

    bp_ui_text_align(x + w - 8.0f, y + 3.0f, UI_FS_TINY, UI_ALIGN_RIGHT, g_assets.pal_text_dim, "%s", wd->display_name);

    /* The magazine count is the largest number on the HUD. That is a deliberate hierarchy:
     * in a fight "how many shots do I have" is asked more often than anything else. */
    bp_ui_text_align(x + w - 8.0f, y + 12.0f, UI_FS_LARGE, UI_ALIGN_RIGHT, ammoCol, "%d", g_hud.ammo_mag);
    float numW = bp_ui_text_width(TextFormat("%d", g_hud.ammo_mag), UI_FS_LARGE);
    bp_ui_text_align(x + w - 12.0f - numW, y + 20.0f, UI_FS_SMALL, UI_ALIGN_RIGHT, g_assets.pal_text_dim,
               "/ %d", g_hud.ammo_reserve);

    /* per-slot pips: which weapons you own, and which one is out */
    float pipX = x + 8.0f, pipY = y + HUD_PANEL_H - 11.0f;
    for (int i = 0; i < WPN_ID_COUNT; i++)
    {
        bool owned = bp_weapons_owns((bp_weapon_id)i);
        bool sel = (i == (int)wd->id);
        Color c = !owned ? (Color){ 58, 62, 72, 190 }
                : sel    ? g_assets.pal_accent
                         : (Color){ 176, 182, 194, 210 };
        if (sel) bp_ui_rect(pipX - 1.0f, pipY - 1.0f, 10.0f, 8.0f, (Color){ 255, 138, 32, 60 });
        bp_ui_rect(pipX, pipY, 8.0f, 6.0f, c);
        bp_ui_text(pipX + 1.0f, pipY - 8.0f, UI_FS_TINY, c, "%d", i + 1);
        pipX += 12.0f;
    }

    /* reload progress: a bar under the panel's top edge, so it is visible without moving the
     * eye off the ammo number. */
    if (g_weapons.state == WSTATE_RELOADING)
    {
        float f = bp_weapons_reload_fraction();
        bp_ui_rect(x, y, w, 2.0f, (Color){ 10, 11, 14, 200 });
        bp_ui_rect(x, y, w * f, 2.0f, g_assets.pal_accent2);
        bp_ui_text(x + 8.0f, y + 4.0f, UI_FS_TINY, g_assets.pal_accent2, "RELOADING");
    }
    else if (empty)
        bp_ui_text(x + 8.0f, y + 4.0f, UI_FS_TINY, g_assets.pal_danger, "PRESS R");
}

static void draw_top_bar(void)
{
    /* Wave progress along the very top edge. One pixel-thick bands at the top of the screen
     * are free peripheral information: they never occlude the fight and they always answer
     * "how much longer". */
    float y = 0.0f;
    bp_ui_rect(0.0f, y, (float)UI_VW, HUD_TOPBAR_H, (Color){ 10, 11, 15, 200 });

    int total = BP_MAX(1, g_waves.wave_total);
    float segW = (float)UI_VW / (float)total;
    for (int i = 0; i < total; i++)
    {
        Color c;
        if (i + 1 < g_waves.wave_index) c = (Color){ 92, 226, 196, 210 };      /* cleared */
        else if (i + 1 == g_waves.wave_index) c = g_waves.state == WAVE_COMPLETE
                                                ? (Color){ 92, 226, 196, 255 }
                                                : g_assets.pal_accent;          /* current */
        else c = (Color){ 62, 66, 78, 200 };                                    /* future  */
        bp_ui_rect((float)i * segW + 1.0f, y, segW - 2.0f, HUD_TOPBAR_H, c);
    }

    /* wave label */
    if (g_waves.state != WAVE_IDLE)
    {
        char label[48];
        if (g_waves.state == WAVE_INTERMISSION)
            snprintf(label, sizeof(label), "WAVE %d/%d  NEXT IN %d", g_waves.wave_index, total,
                     (int)BP_MAX(0.0f, g_hud.intermission_left + 0.999f));
        else if (g_waves.state == WAVE_COMPLETE)
            snprintf(label, sizeof(label), "AREA CLEAR");
        else
            snprintf(label, sizeof(label), "WAVE %d/%d  HOSTILES %d", g_waves.wave_index, total,
                     bp_waves_remaining());

        bp_ui_text_shadow(HUD_MARGIN, HUD_TOPBAR_H + 3.0f, UI_FS_SMALL, UI_ALIGN_LEFT,
                          g_assets.pal_text, (Color){ 0, 0, 0, 200 }, "%s", label);
    }

    /* score, top right */
    bp_ui_text_shadow((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 3.0f, UI_FS_SMALL, UI_ALIGN_RIGHT,
                      g_assets.pal_ammo, (Color){ 0, 0, 0, 200 }, "%d", g_player.score);
    bp_ui_text_align((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 15.0f, UI_FS_TINY, UI_ALIGN_RIGHT,
               g_assets.pal_text_dim, "KILLS %d   ACC %d%%", g_player.kills,
               (int)(bp_weapons_accuracy() * 100.0f + 0.5f));

    /* streak: only shown while one is live, because a permanent "x1" is noise */
    if (g_player.streak >= 2)
    {
        float pulse = 0.7f + 0.3f * sinf(g_hud.time * 8.0f);
        bp_ui_text_align((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 25.0f, UI_FS_SMALL, UI_ALIGN_RIGHT,
                   hud_col(255.0f, 138.0f * pulse, 32.0f, 255.0f), "STREAK x%d", g_player.streak);
    }
}

static void draw_speed_readout(void)
{
    /* Speed in m/s next to the vitals plate. Boomer shooters are played for movement, so the
     * player needs to be able to see whether a strafe-jump actually worked. */
    float x = HUD_MARGIN + HUD_VITALS_W + 8.0f;
    float y = (float)UI_VH - HUD_MARGIN - 12.0f;

    float frac = BP_CLAMP(g_hud.speed / BP_MAX(g_player.speed_sprint, 1.0f), 0.0f, 2.0f);
    Color c = frac > 1.15f ? g_assets.pal_accent2
            : frac > 0.85f ? g_assets.pal_accent
                           : g_assets.pal_text_dim;

    bp_ui_text(x, y, UI_FS_TINY, g_assets.pal_text_dim, "VEL");
    bp_ui_text(x + 22.0f, y - 3.0f, UI_FS_SMALL, c, "%.1f", (double)g_hud.speed);
    bp_ui_text(x + 58.0f, y, UI_FS_TINY, g_assets.pal_text_dim, "M/S");

    /* a thin meter above the number: the fastest legal speed is marked, so anything past the
     * mark is visibly "bonus" speed earned by circle-strafing */
    float barW = 76.0f;
    bp_ui_rect(x, y - 6.0f, barW, 2.0f, (Color){ 10, 11, 15, 200 });
    bp_ui_rect(x, y - 6.0f, barW * BP_MIN(frac, 1.0f), 2.0f, c);
    bp_ui_rect(x + barW - 1.0f, y - 8.0f, 1.0f, 6.0f, (Color){ 255, 255, 255, 90 });
    if (frac > 1.0f) bp_ui_rect(x + barW, y - 6.0f, barW * BP_MIN(frac - 1.0f, 1.0f), 2.0f, g_assets.pal_accent2);
}

static void draw_crosshair(void)
{
    const bp_weapon_def *wd = bp_weapons_current_def();
    if (!wd) return;

    /* Gap is driven by the live spread cone: the crosshair is the accuracy readout. Scale is
     * chosen so a tight pistol (0.15 deg aimed) is a near-dot and a shotgun hip shot (4.6
     * deg, doubled by movement) is an unmistakable wide cross. */
    float gap = 3.0f + g_weapons.spread_current * 3.2f;
    float len = (wd->pellets > 1) ? 3.0f : 5.0f;

    Color c = bp_weapons_can_fire() ? (Color){ 226, 230, 238, 245 } : (Color){ 176, 182, 194, 200 };
    if (g_hud.ammo_mag <= 0) c = g_assets.pal_danger;
    else if (g_weapons.ads > 0.5f) c = g_assets.pal_accent2;

    bp_ui_crosshair(0.0f, 0.0f, gap, len, 1.0f, c, g_fx.hit_marker);

    /* Pellet spread for shotguns: a ring at the cone radius, because nine separate crosses
     * would be unreadable and a single cross would lie about where the pellets go. */
    if (wd->pellets > 1)
    {
        float r = gap + 4.0f + g_weapons.spread_current * 2.0f;
        int cx = GetScreenWidth() / 2, cy = GetScreenHeight() / 2;
        float rs = r * (float)bp_ui_scale();
        DrawCircleLines(cx, cy, rs, hud_col((float)c.r, (float)c.g, (float)c.b, 70.0f));
    }
}

static void draw_damage_dirs(void)
{
    if (g_hud.dir_count <= 0) return;
    float cx = (float)UI_VW * 0.5f;
    float cy = (float)UI_VH * 0.5f;

    for (int i = 0; i < g_hud.dir_count; i++)
    {
        const bp_dmgdir *d = &g_hud.dirs[i];
        float k = 1.0f - BP_CLAMP(d->age / d->life, 0.0f, 1.0f);
        float alpha = k * BP_CLAMP(d->strength, 0.25f, 1.0f);
        if (alpha <= 0.01f) continue;

        /* angle 0 = in front, +90 deg = to the right, matching atan2(right, forward) */
        float a = d->angle;
        float r = HUD_DIR_RADIUS + (1.0f - k) * 10.0f;
        float sx = cx + sinf(a) * r;
        float sy = cy - cosf(a) * r;

        Color c = hud_col(232.0f, 52.0f, 60.0f, 235.0f * alpha);
        /* A short arc segment pointing inward. Arcs beat arrows here: an arrow needs to be
         * read, an arc's position is perceived pre-attentively. */
        float len = 12.0f;
        float ex = cx + sinf(a) * (r - len);
        float ey = cy - cosf(a) * (r - len);
        bp_ui_line(sx, sy, ex, ey, 2.0f, c);

        /* widen perpendicular for a "bracket" look */
        float px = cosf(a) * 4.0f, py = sinf(a) * 4.0f;
        bp_ui_line(sx - px, sy - py, sx + px, sy + py, 2.0f, c);
    }
}

static void draw_hurt_vignette(void)
{
    if (g_hud.hurt_vignette <= 0.01f) return;
    /* Four edge gradients rather than a radial texture: no extra asset, no filter, and the
     * banding is invisible at these alpha levels. */
    float a = BP_CLAMP(g_hud.hurt_vignette, 0.0f, 1.0f);
    Color clear = (Color){ 190, 20, 26, 0 };
    Color hot = hud_col(190.0f, 20.0f, 26.0f, 150.0f * a);
    float t = 78.0f;

    bp_ui_vgradient(0.0f, 0.0f, (float)UI_VW, t, hot, clear);
    bp_ui_vgradient(0.0f, (float)UI_VH - t, (float)UI_VW, t, clear, hot);
    bp_ui_gradient(0.0f, 0.0f, t, (float)UI_VH, hot, clear);
    bp_ui_gradient((float)UI_VW - t, 0.0f, t, (float)UI_VH, clear, hot);
}

static void draw_low_health(void)
{
    float frac = g_player.health / BP_MAX(g_player.max_health, 1.0f);
    if (frac > 0.28f || g_player.health <= 0.0f) return;

    /* Slow red pulse at the screen edge. Deliberately slow (1.6 Hz) and low alpha: a fast
     * bright pulse is fatiguing within thirty seconds, and this can be on screen for minutes. */
    float pulse = 0.5f + 0.5f * sinf(g_hud.time * 1.6f * BP_TAU * 0.5f);
    float a = (1.0f - frac / 0.28f) * (0.25f + 0.45f * pulse);
    Color clear = (Color){ 200, 24, 32, 0 };
    Color hot = hud_col(200.0f, 24.0f, 32.0f, 190.0f * a);
    float t = 64.0f;
    bp_ui_vgradient(0.0f, (float)UI_VH - t, (float)UI_VW, t, clear, hot);
    bp_ui_vgradient(0.0f, 0.0f, (float)UI_VW, t, hot, clear);
}

static void draw_toasts(void)
{
    if (g_hud.toast_count <= 0) return;
    float cx = (float)UI_VW * 0.5f;

    for (int i = 0; i < g_hud.toast_count; i++)
    {
        const bp_toast *t = &g_hud.toasts[i];
        float life = BP_CLAMP(t->age / t->life, 0.0f, 1.0f);
        /* slide in from above, fade out at the end; no pop-in */
        float in = BP_CLAMP(t->age * 8.0f, 0.0f, 1.0f);
        float out = life > 0.72f ? 1.0f - (life - 0.72f) / 0.28f : 1.0f;
        float alpha = in * out;
        if (alpha <= 0.01f) continue;

        float y = HUD_TOAST_Y + t->y * 15.0f - (1.0f - in) * 8.0f;

        Color text, back;
        switch (t->kind)
        {
            case TOAST_GOOD: text = g_assets.pal_health; back = (Color){ 12, 22, 16, 190 }; break;
            case TOAST_BAD:  text = g_assets.pal_danger; back = (Color){ 24, 12, 14, 190 }; break;
            case TOAST_BIG:  text = g_assets.pal_accent; back = (Color){ 20, 16, 10, 200 }; break;
            default:         text = g_assets.pal_text;   back = (Color){ 12, 13, 17, 180 }; break;
        }

        float size = (t->kind == TOAST_BIG) ? UI_FS_SMALL : UI_FS_SMALL;
        float w = bp_ui_text_width(t->text, size) + 16.0f;
        bp_ui_rect(cx - w * 0.5f, y - 2.0f, w, 14.0f, bp_ui_fade(back, alpha));
        bp_ui_rect(cx - w * 0.5f, y - 2.0f, 2.0f, 14.0f, bp_ui_fade(text, alpha));
        bp_ui_text_shadow(cx, y, size, UI_ALIGN_CENTER, bp_ui_fade(text, alpha),
                          (Color){ 0, 0, 0, (unsigned char)(190 * alpha) }, "%s", t->text);
    }
}

static void draw_banner(void)
{
    if (g_hud.banner_time <= 0.0f || !g_hud.banner[0]) return;

    /* Slide-and-settle title. The banner is the one HUD element allowed to occupy the centre
     * of the screen, because it only appears when the player is not being shot at. */
    float t = g_hud.banner_time;
    float in = BP_CLAMP((2.6f - t) * 5.0f, 0.0f, 1.0f);
    float out = BP_CLAMP(t / 0.5f, 0.0f, 1.0f);
    float alpha = in * out;
    float y = HUD_BANNER_Y - (1.0f - in) * 14.0f;

    bp_ui_rect(0.0f, y - 8.0f, (float)UI_VW, 46.0f, (Color){ 8, 9, 12, (unsigned char)(200 * alpha) });
    bp_ui_rect(0.0f, y - 8.0f, (float)UI_VW, 1.0f, hud_col(255.0f, 138.0f, 32.0f, 200.0f * alpha));
    bp_ui_rect(0.0f, y + 37.0f, (float)UI_VW, 1.0f, hud_col(255.0f, 138.0f, 32.0f, 200.0f * alpha));

    bp_ui_text_shadow((float)UI_VW * 0.5f, y, UI_FS_LARGE, UI_ALIGN_CENTER,
                      hud_col(255.0f, 138.0f, 32.0f, 255.0f * alpha), (Color){ 0, 0, 0, (unsigned char)(220 * alpha) },
                      "%s", g_hud.banner);
    if (g_hud.banner_sub[0])
        bp_ui_text_shadow((float)UI_VW * 0.5f, y + 24.0f, UI_FS_SMALL, UI_ALIGN_CENTER,
                          hud_col(190.0f, 196.0f, 206.0f, 230.0f * alpha), (Color){ 0, 0, 0, (unsigned char)(200 * alpha) },
                          "%s", g_hud.banner_sub);
}

static void draw_objective(void)
{
    /* One line, top-left, under the wave label. Never more than one line: if the objective
     * needs two lines the level design is unclear and the HUD cannot fix it. */
    const char *obj;
    if (g_waves.state == WAVE_COMPLETE) obj = g_player.exit_reached ? "EXTRACTION OPEN" : "REACH THE EXIT";
    else if (g_waves.state == WAVE_INTERMISSION) obj = "RESUPPLY -- NEXT WAVE INCOMING";
    else obj = "ELIMINATE ALL HOSTILES";

    bp_ui_text(HUD_MARGIN, HUD_TOPBAR_H + 26.0f, UI_FS_TINY, g_assets.pal_text_dim, "%s", obj);

    /* Exit direction marker once the level is clear: a chevron at the screen edge pointing at
     * the exit trigger. Without it "reach the exit" becomes a scavenger hunt. */
    if (g_waves.state != WAVE_COMPLETE) return;

    Vector3 fwd = bp_camera_forward();
    fwd.y = 0.0f;
    if (Vector3LengthSqr(fwd) < 1e-6f) return;
    fwd = Vector3Normalize(fwd);

    /* find the exit trigger */
    bool found = false;
    Vector3 exitPos = { 0, 0, 0 };
    for (int i = 0; i < g_world.trigger_count; i++)
        if (g_world.triggers[i].kind == TRIG_EXIT) { exitPos = g_world.triggers[i].position; found = true; break; }
    if (!found) return;

    Vector3 to = Vector3Subtract(exitPos, g_player.pos);
    to.y = 0.0f;
    float dist = Vector3Length(to);
    if (dist < 0.5f) return;
    to = Vector3Scale(to, 1.0f / dist);

    /* signed angle from view forward: +right, -left */
    float ang = atan2f(to.x * fwd.z - to.z * fwd.x, to.x * fwd.x + to.z * fwd.z);
    float cx = (float)UI_VW * 0.5f, cy = (float)UI_VH * 0.5f;
    /* clamp the marker to a ring so it stays on screen even when the exit is behind us */
    float r = 120.0f;
    float mx = cx + sinf(ang) * r;
    float my = cy - cosf(ang) * r * 0.55f;      /* squash vertically: the screen is 16:9 */

    Color c = hud_col(92.0f, 226.0f, 196.0f, 220.0f);
    float pulse = 0.6f + 0.4f * sinf(g_hud.time * 4.0f);
    bp_ui_icon_diamond(mx, my, 4.0f * pulse + 2.0f, c);
    bp_ui_text_shadow(mx, my + 7.0f, UI_FS_TINY, UI_ALIGN_CENTER, c, (Color){ 0, 0, 0, 190 },
                      "EXIT %dm", (int)(dist + 0.5f));
}

static void draw_debug_overlay(void)
{
    if (!g_engine.perf.show_overlay) return;

    float x = (float)UI_VW - 178.0f, y = 44.0f, w = 170.0f;
    int lines = 12;
    bp_ui_panel(x, y, w, 12.0f + (float)lines * 9.0f, (Color){ 8, 9, 12, 210 }, (Color){ 92, 226, 196, 90 });

    float ty = y + 4.0f;
    #define ROW(label, fmt, col, ...) do { \
        bp_ui_text(x + 6.0f, ty, UI_FS_TINY, g_assets.pal_text_dim, label); \
        bp_ui_text(x + w - 6.0f, ty, UI_FS_TINY, col, fmt, __VA_ARGS__); \
        ty += 9.0f; } while (0)

    Color good = g_assets.pal_health, warn = g_assets.pal_ammo, bad = g_assets.pal_danger;
    Color fpsCol = g_engine.perf.fps >= 58 ? good : (g_engine.perf.fps >= 30 ? warn : bad);

    ROW("FPS",        "%d",            fpsCol, g_engine.perf.fps);
    ROW("FRAME",      "%.2f ms",       g_engine.perf.frame_ms > 20.0f ? bad : g_assets.pal_text, (double)g_engine.perf.frame_ms);
    ROW("SIM",        "%.2f ms",       g_engine.perf.sim_ms > 8.0f ? warn : g_assets.pal_text, (double)g_engine.perf.sim_ms);
    ROW("DRAW",       "%.2f ms",       g_engine.perf.draw_ms > 10.0f ? warn : g_assets.pal_text, (double)g_engine.perf.draw_ms);
    ROW("SIM STEPS",  "%d",            g_engine.perf.sim_steps > 3 ? warn : g_assets.pal_text, g_engine.perf.sim_steps);
    /* Draw calls are not reported: raylib 5.5 publishes no counter, and an invented number
     * on a debug overlay is worse than a missing one. */
    ROW("PARTICLES",  "%d",            g_fx.particle_live > MAX_PARTICLES * 3 / 4 ? warn : g_assets.pal_text, g_fx.particle_live);
    ROW("DECALS",     "%d",            g_assets.pal_text, g_fx.decal_live);
    ROW("ENEMIES",    "%d / %d",       g_enemies.alive_count > g_waves.max_onscreen ? warn : g_assets.pal_text,
                                       g_enemies.alive_count, g_waves.max_onscreen);
    ROW("PROJECTILES","%d",            g_assets.pal_text, g_weapons.projectile_count);
    ROW("SOLIDS",     "%d",            g_assets.pal_text, g_world.solid_count);
    ROW("WAVE STATE", "%s",            g_assets.pal_accent2, bp_waves_state_name());
    ROW("QUALITY",    "%s",            g_assets.pal_text, bp_gfx_quality_name(g_gfx.quality));
    #undef ROW

    /* pool pressure is the thing that actually breaks the game, so it gets its own line */
    bp_ui_text(x + 6.0f, ty, UI_FS_TINY, g_assets.pal_text_dim, "POOL");
    bp_ui_text(x + w - 6.0f, ty, UI_FS_TINY, g_assets.pal_text, "E%d P%d D%d",
               g_enemies.pool.count, g_fx.particle_live, g_fx.decal_live);
}

/* -------------------------------------------------------------------------------------- */
/* frame                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_hud_draw(void)
{
    if (!g_hud.visible) return;

    draw_hurt_vignette();
    draw_low_health();
    draw_damage_dirs();

    if (!g_hud.minimal)
    {
        draw_top_bar();
        draw_objective();
        draw_speed_readout();
        draw_vitals();
        draw_toasts();
    }
    else
    {
        /* Competitive minimal: health number, ammo number, crosshair. Nothing else. */
        float hpFrac = BP_CLAMP(g_hud.health / BP_MAX(g_player.max_health, 1.0f), 0.0f, 1.0f);
        bp_ui_text_shadow(HUD_MARGIN, (float)UI_VH - HUD_MARGIN - 22.0f, UI_FS_LARGE, UI_ALIGN_LEFT,
                          health_color(hpFrac), (Color){ 0, 0, 0, 200 }, "%d", (int)(g_hud.health + 0.5f));
        bp_ui_text_shadow((float)UI_VW - HUD_MARGIN, (float)UI_VH - HUD_MARGIN - 22.0f, UI_FS_LARGE,
                          UI_ALIGN_RIGHT, g_assets.pal_ammo, (Color){ 0, 0, 0, 200 }, "%d", g_hud.ammo_mag);
    }

    draw_weapon();
    draw_crosshair();
    draw_banner();
    draw_debug_overlay();
}
