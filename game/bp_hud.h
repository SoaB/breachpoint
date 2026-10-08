/******************************************************************************************
 * bp_hud.h -- the in-game heads up display.
 *
 * HUD PHILOSOPHY (the part most projects get wrong)
 *   The HUD is not a dashboard. During a fight the player's eyes are at the centre of the
 *   screen and stay there; everything else is peripheral. So:
 *     - Anything that must be *read* (health, armour, ammo) lives in the bottom corners,
 *       inside the natural peripheral band, and uses SHAPE and COLOUR as the primary
 *       encoding with the number as confirmation. You should be able to tell "I am nearly
 *       dead" from a 6-degree glance at a red, almost-empty segmented bar without ever
 *       resolving a digit.
 *     - Anything that must be *noticed* (wave banner, kill feed, low ammo, secret found)
 *       is transient, animates, and removes itself. Permanent HUD elements train the eye to
 *       ignore them.
 *     - The centre of the screen belongs to the crosshair and the spread indicator and to
 *       nothing else. No minimap, no compass, no objective text over the aim point.
 *   The HUD therefore has exactly zero authority over the simulation. It reads globals and
 *   draws. The one exception is the toast queue, which is a UI-local event buffer -- other
 *   modules push strings into it rather than each drawing their own popup at their own
 *   coordinates, which is how HUDs end up with overlapping text.
 ******************************************************************************************/
#ifndef BP_HUD_H
#define BP_HUD_H

#include "bp_common.h"

#define HUD_MAX_TOASTS    6
#define HUD_MAX_DMGDIRS   5

typedef enum {
    TOAST_INFO = 0,      /* neutral white/amber  */
    TOAST_GOOD,          /* pickups, secrets     */
    TOAST_BAD,           /* low health, warnings */
    TOAST_BIG            /* wave banner          */
} bp_toast_kind;

typedef struct {
    char  text[96];
    bp_toast_kind kind;
    float age;
    float life;
    float y;             /* resolved slot position, animated in */
} bp_toast;

typedef struct {
    float angle;         /* radians, relative to view yaw */
    float age;
    float life;
    float strength;
} bp_dmgdir;

typedef struct {
    /* smoothed values: the HUD never snaps, because a snapping number is unreadable */
    float health;
    float armor;
    float speed;
    int   ammo_mag;
    int   ammo_reserve;

    /* transient */
    float banner_time;
    char  banner[64];
    char  banner_sub[64];
    float hurt_vignette;
    float low_ammo_pulse;
    float intermission_left;

    bp_toast  toasts[HUD_MAX_TOASTS];
    int       toast_count;
    bp_dmgdir dirs[HUD_MAX_DMGDIRS];
    int       dir_count;

    bool      visible;
    bool      minimal;      /* competitive mode: health + ammo + crosshair only */
    bool      show_fps;
    float     time;
} bp_hud;

extern bp_hud g_hud;

void  bp_hud_init(void);
void  bp_hud_reset(void);
void  bp_hud_update(float dt);
void  bp_hud_draw(void);          /* after bp_gfx_end_scene, inside bp_ui frame */

void  bp_hud_toast(const char *text, bp_toast_kind kind, float life);
void  bp_hud_banner(const char *title, const char *subtitle, float life);
void  bp_hud_notify_damage(float angle, float strength);
void  bp_hud_set_visible(bool visible);

#endif /* BP_HUD_H */
