/******************************************************************************************
 * bp_hud.c -- HUD（heads-up display，抬頭顯示器）的實作。
 *
 * 這裡的所有東西都繪製在 640x360 的虛擬畫布上（見 bp_ui.h），因此 HUD 版面在各種解析
 * 度下都長得一模一樣。唯一刻意違反這條規則的元素是準星：它以裝置像素釘死在螢幕正中央，
 * 因為它必須與相機的中心射線（centre ray）重合，否則槍就不會射向你盯著的地方。
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
/* 版面：640x360 虛擬畫布                                                                         */
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
/* 輔助函式                                                                                    */
/* -------------------------------------------------------------------------------------- */
static inline Color hud_col(float r, float g, float b, float a)
{
    return (Color){ (unsigned char)BP_CLAMP((int)r, 0, 255), (unsigned char)BP_CLAMP((int)g, 0, 255),
                    (unsigned char)BP_CLAMP((int)b, 0, 255), (unsigned char)BP_CLAMP((int)a, 0, 255) };
}

static Color health_color(float frac)
{
    /* 綠 → 琥珀 → 紅。顏色是主要的讀值，數字只是拿來確認的，因此這條漸變必須在周邊視
     * 覺（peripheral vision）中也分辨得出來：也就是說，每一步都必須改變色相（HUE），
     * 而不能只改亮度。 */
    if (frac > 0.60f) return g_assets.pal_health;
    if (frac > 0.30f) return g_assets.pal_ammo;
    return g_assets.pal_danger;
}

static void push_toast(const char *text, bp_toast_kind kind, float life)
{
    /* 佇列滿了就覆蓋掉最舊的 toast。若是丟掉「新」訊息，等於在畫面最忙亂的那一刻，把最
     * 相關的資訊藏起來。 */
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
/* 公開事件                                                                                    */
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
        /* 取代最舊的指示器，而不是拒絕新的這一個：最近的一次受擊，才是值得指出方向的
         * 那一次。 */
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
/* 初始化 / 更新                                                                                */
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

    /* 平滑化。每個供判讀的數值都走阻尼（damp）而不是直接賦值：一幀之內就從 34 跳到 21
     * 的血量數字根本沒法讀，而且眼睛追蹤「移動中的長條」遠比追蹤「瞬間跳位的長條」來
     * 得輕鬆。lambda 的取值讓數值在大約 0.25 秒內安定下來。 */
    g_hud.health = bp_damp(g_hud.health, g_player.health, 12.0f, dt);
    g_hud.armor  = bp_damp(g_hud.armor,  g_player.armor,  12.0f, dt);

    /* 速度的平滑力道更強：一個隨每次落腳而抽動的讀值只是雜訊。 */
    float hs = Vector3Length((Vector3){ g_player.vel.x, 0.0f, g_player.vel.z });
    g_hud.speed = bp_damp(g_hud.speed, hs, 8.0f, dt);

    const bp_weapon_def *wd = bp_weapons_current_def();
    g_hud.ammo_mag = wd ? g_weapons.mag[wd->id] : 0;
    g_hud.ammo_reserve = wd ? g_weapons.reserve[wd->id] : 0;

    /* 餵給後製著色器。這些數值之所以歸 HUD 持有，是因為「你受傷了」「你很快」這套視覺
     * 語言本來就屬於 HUD；算繪器只負責執行。 */
    bp_gfx_flash_damage(g_player.damage_flash * dt * 2.2f);
    bp_gfx_flash_heal(g_player.heal_flash * dt * 1.6f);
    g_gfx.speed_lines = BP_CLAMP((g_hud.speed - g_player.speed_ground) /
                                 BP_MAX(g_player.speed_sprint * 1.4f - g_player.speed_ground, 1.0f), 0.0f, 1.0f);

    g_hud.hurt_vignette = BP_MAX(g_player.damage_flash, g_hud.hurt_vignette - dt * 1.8f);

    /* 低彈藥脈動：只有當彈匣相對「這把武器」確實偏低時才亮起 */
    float magFrac = (wd && wd->mag_size > 0) ? (float)g_hud.ammo_mag / (float)wd->mag_size : 1.0f;
    g_hud.low_ammo_pulse = (magFrac <= 0.25f && g_hud.ammo_mag > 0)
                         ? 0.5f + 0.5f * sinf(g_hud.time * 7.0f)
                         : 0.0f;

    g_hud.intermission_left = (g_waves.state == WAVE_INTERMISSION) ? g_waves.intermission_left : 0.0f;

    /* 橫幅與 toast */
    if (g_hud.banner_time > 0.0f) g_hud.banner_time -= dt;
    for (int i = 0; i < g_hud.toast_count; i++)
    {
        bp_toast *t = &g_hud.toasts[i];
        t->age += dt;
        t->y = bp_damp(t->y, (float)i, 16.0f, dt);   /* toast 到期後，各槽位會重新流動排列 */
    }
    /* 把到期的項目剔除並向前壓縮，順序保持不變 */
    int w = 0;
    for (int i = 0; i < g_hud.toast_count; i++)
    {
        if (g_hud.toasts[i].age < g_hud.toasts[i].life) g_hud.toasts[w++] = g_hud.toasts[i];
    }
    g_hud.toast_count = w;

    /* 受擊方向指示器 */
    for (int i = 0; i < g_hud.dir_count; i++) g_hud.dirs[i].age += dt;
    w = 0;
    for (int i = 0; i < g_hud.dir_count; i++)
        if (g_hud.dirs[i].age < g_hud.dirs[i].life) g_hud.dirs[w++] = g_hud.dirs[i];
    g_hud.dir_count = w;
}

/* -------------------------------------------------------------------------------------- */
/* 部件繪製                                                                                    */
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

    /* 你挨打的那一幀，鈑件會閃紅。往內縮 2px 的閃光遠比把整塊面板染色好讀 —— 染色會
     * 把數字蓋掉。 */
    Color border = hud_col(hpCol.r, hpCol.g, hpCol.b, 90 + g_hud.hurt_vignette * 165.0f);
    bp_ui_panel_notch(x, y, w, h, 6.0f, g_assets.pal_panel, border);

    /* 血量 */
    float barX = x + 8.0f, barW = w - 16.0f;
    bp_ui_text(barX, y + 4.0f, UI_FS_TINY, g_assets.pal_text_dim, "INTEGRITY");
    bp_ui_bar_segmented(barX, y + 12.0f, barW, 9.0f, hpFrac, HUD_HEALTH_SEG,
                        hpCol, (Color){ 10, 11, 14, 220 }, (Color){ 0, 0, 0, 140 });
    bp_ui_text_align(x + w - 8.0f, y + 22.0f, UI_FS_LARGE, UI_ALIGN_RIGHT, hpCol,
                     "%d", (int)(g_hud.health + 0.5f));

    /* 護甲 */
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

    /* 彈匣存量是 HUD 上最大的數字。這是刻意安排的視覺階層：在交火的時候，「我還剩幾
     * 發」這個問題被問到的頻率，高過其他任何一個數字。 */
    bp_ui_text_align(x + w - 8.0f, y + 12.0f, UI_FS_LARGE, UI_ALIGN_RIGHT, ammoCol, "%d", g_hud.ammo_mag);
    float numW = bp_ui_text_width(TextFormat("%d", g_hud.ammo_mag), UI_FS_LARGE);
    bp_ui_text_align(x + w - 12.0f - numW, y + 20.0f, UI_FS_SMALL, UI_ALIGN_RIGHT, g_assets.pal_text_dim,
               "/ %d", g_hud.ammo_reserve);

    /* 各槽位的小標記（pip）：顯示你持有哪幾把武器、目前拔出來的是哪一把 */
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

    /* 填彈進度：畫在面板上緣底下的一條長條，這樣不必把視線移開彈藥數字也看得到。 */
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
    /* 波次進度沿著螢幕的最上緣展開。畫面頂端這些一像素厚的色帶是零成本的周邊資訊：它們
     * 永遠不會擋到戰鬥畫面，而且隨時都在回答「還要多久」。 */
    float y = 0.0f;
    bp_ui_rect(0.0f, y, (float)UI_VW, HUD_TOPBAR_H, (Color){ 10, 11, 15, 200 });

    int total = BP_MAX(1, g_waves.wave_total);
    float segW = (float)UI_VW / (float)total;
    for (int i = 0; i < total; i++)
    {
        Color c;
        if (i + 1 < g_waves.wave_index) c = (Color){ 92, 226, 196, 210 };      /* 已過 */
        else if (i + 1 == g_waves.wave_index) c = g_waves.state == WAVE_COMPLETE
                                                ? (Color){ 92, 226, 196, 255 }
                                                : g_assets.pal_accent;          /* 目前 */
        else c = (Color){ 62, 66, 78, 200 };                                    /* 未來 */
        bp_ui_rect((float)i * segW + 1.0f, y, segW - 2.0f, HUD_TOPBAR_H, c);
    }

    /* 波次標籤 */
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

    /* 分數，右上角 */
    bp_ui_text_shadow((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 3.0f, UI_FS_SMALL, UI_ALIGN_RIGHT,
                      g_assets.pal_ammo, (Color){ 0, 0, 0, 200 }, "%d", g_player.score);
    bp_ui_text_align((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 15.0f, UI_FS_TINY, UI_ALIGN_RIGHT,
               g_assets.pal_text_dim, "KILLS %d   ACC %d%%", g_player.kills,
               (int)(bp_weapons_accuracy() * 100.0f + 0.5f));

    /* 連殺（streak）：只在有進行中的連殺時顯示，因為常駐的「x1」只是雜訊 */
    if (g_player.streak >= 2)
    {
        float pulse = 0.7f + 0.3f * sinf(g_hud.time * 8.0f);
        bp_ui_text_align((float)UI_VW - HUD_MARGIN, HUD_TOPBAR_H + 25.0f, UI_FS_SMALL, UI_ALIGN_RIGHT,
                   hud_col(255.0f, 138.0f * pulse, 32.0f, 255.0f), "STREAK x%d", g_player.streak);
    }
}

static void draw_speed_readout(void)
{
    /* 以 m/s 為單位的速度，顯示在生命鈑件旁。Boomer shooter 玩的就是移動，所以玩家必須
     * 能看得出某次平移跳（strafe-jump）到底有沒有成功。 */
    float x = HUD_MARGIN + HUD_VITALS_W + 8.0f;
    float y = (float)UI_VH - HUD_MARGIN - 12.0f;

    float frac = BP_CLAMP(g_hud.speed / BP_MAX(g_player.speed_sprint, 1.0f), 0.0f, 2.0f);
    Color c = frac > 1.15f ? g_assets.pal_accent2
            : frac > 0.85f ? g_assets.pal_accent
                           : g_assets.pal_text_dim;

    bp_ui_text(x, y, UI_FS_TINY, g_assets.pal_text_dim, "VEL");
    bp_ui_text(x + 22.0f, y - 3.0f, UI_FS_SMALL, c, "%.1f", (double)g_hud.speed);
    bp_ui_text(x + 58.0f, y, UI_FS_TINY, g_assets.pal_text_dim, "M/S");

    /* 數字上方的一條細計量條：標線標出最快的合法速度，因此凡是超過標線的部分，一眼就
     * 看得出是靠繞圈平移（circle-strafe）賺來的「獎勵」速度 */
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

    /* 間隙由即時的散布錐（spread cone）驅動：準星本身就是精準度的讀值。比例是刻意挑的：
     * 一把收得很緊的瞄準手槍（0.15 度）看起來接近一個點，而霰彈槍的腰射（4.6 度，移動
     * 時再翻倍）則是一個不可能認錯的寬十字。 */
    float gap = 3.0f + g_weapons.spread_current * 3.2f;
    float len = (wd->pellets > 1) ? 3.0f : 5.0f;

    Color c = bp_weapons_can_fire() ? (Color){ 226, 230, 238, 245 } : (Color){ 176, 182, 194, 200 };
    if (g_hud.ammo_mag <= 0) c = g_assets.pal_danger;
    else if (g_weapons.ads > 0.5f) c = g_assets.pal_accent2;

    bp_ui_crosshair(0.0f, 0.0f, gap, len, 1.0f, c, g_fx.hit_marker);

    /* 霰彈槍的彈丸散布：在散布錐的半徑上畫一個圈，因為九個各自分開的十字沒法判讀，而
     * 單獨一個十字又會對彈丸的去向說謊。 */
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

        /* 角度 0 = 正前方，+90 度 = 右側，與 atan2(right, forward) 的定義一致 */
        float a = d->angle;
        float r = HUD_DIR_RADIUS + (1.0f - k) * 10.0f;
        float sx = cx + sinf(a) * r;
        float sy = cy - cosf(a) * r;

        Color c = hud_col(232.0f, 52.0f, 60.0f, 235.0f * alpha);
        /* 一小段指向中心的弧。這裡弧線勝過箭頭：箭頭需要被「讀懂」，而弧的位置是在前
         * 注意力（pre-attentive）階段就被感知到的。 */
        float len = 12.0f;
        float ex = cx + sinf(a) * (r - len);
        float ey = cy - cosf(a) * (r - len);
        bp_ui_line(sx, sy, ex, ey, 2.0f, c);

        /* 垂直於半徑方向加寬，做出「括號」般的外觀 */
        float px = cosf(a) * 4.0f, py = sinf(a) * 4.0f;
        bp_ui_line(sx - px, sy - py, sx + px, sy + py, 2.0f, c);
    }
}

static void draw_hurt_vignette(void)
{
    if (g_hud.hurt_vignette <= 0.01f) return;
    /* 用四道邊緣漸層，而不是一張徑向貼圖：不必多一個素材、不必過濾，而且在這個透明度
     * 層級下，色帶（banding）現象根本看不見。 */
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

    /* 螢幕邊緣的緩慢紅色脈動。刻意放慢（1.6 Hz）並壓低透明度：又快又亮的脈動三十秒內
     * 就會讓人疲勞，而這個效果可能一停在畫面上就是好幾分鐘。 */
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
        /* 從上方滑入、結尾淡出；不要突然彈出（pop-in） */
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

    /* 滑入後定位的標題。橫幅是唯一獲准佔據畫面中央的 HUD 元素，因為它只會在玩家沒有
     * 正在挨子彈的時候出現。 */
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
    /* 一行文字，左上角，位於波次標籤下方。永遠不超過一行：如果目標說明需要兩行，那代
     * 表關卡設計本身沒講清楚，而 HUD 修不了這件事。 */
    const char *obj;
    if (g_waves.state == WAVE_COMPLETE) obj = g_player.exit_reached ? "EXTRACTION OPEN" : "REACH THE EXIT";
    else if (g_waves.state == WAVE_INTERMISSION) obj = "RESUPPLY -- NEXT WAVE INCOMING";
    else obj = "ELIMINATE ALL HOSTILES";

    bp_ui_text(HUD_MARGIN, HUD_TOPBAR_H + 26.0f, UI_FS_TINY, g_assets.pal_text_dim, "%s", obj);

    /* 關卡清空之後的出口方向標記：螢幕邊緣一個指向出口觸發器的人字形（chevron）。少了
     * 它，「走到出口」就會淪為一場尋寶遊戲。 */
    if (g_waves.state != WAVE_COMPLETE) return;

    Vector3 fwd = bp_camera_forward();
    fwd.y = 0.0f;
    if (Vector3LengthSqr(fwd) < 1e-6f) return;
    fwd = Vector3Normalize(fwd);

    /* 找出出口觸發器 */
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

    /* 相對視線前方的帶號角（signed angle）：右為正、左為負 */
    float ang = atan2f(to.x * fwd.z - to.z * fwd.x, to.x * fwd.x + to.z * fwd.z);
    float cx = (float)UI_VW * 0.5f, cy = (float)UI_VH * 0.5f;
    /* 把標記夾在一個環上，如此即使出口位於我們背後，標記也留在畫面內 */
    float r = 120.0f;
    float mx = cx + sinf(ang) * r;
    float my = cy - cosf(ang) * r * 0.55f;      /* 垂直方向壓扁：螢幕是 16:9 */

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
    /* 不回報 draw call 數量：raylib 5.5 沒有提供計數器，而在除錯覆蓋層上放一個憑空
     * 捏造的數字，比缺少一個數字更糟。 */
    ROW("PARTICLES",  "%d",            g_fx.particle_live > MAX_PARTICLES * 3 / 4 ? warn : g_assets.pal_text, g_fx.particle_live);
    ROW("DECALS",     "%d",            g_assets.pal_text, g_fx.decal_live);
    ROW("ENEMIES",    "%d / %d",       g_enemies.alive_count > g_waves.max_onscreen ? warn : g_assets.pal_text,
                                       g_enemies.alive_count, g_waves.max_onscreen);
    ROW("PROJECTILES","%d",            g_assets.pal_text, g_weapons.projectile_count);
    ROW("SOLIDS",     "%d",            g_assets.pal_text, g_world.solid_count);
    ROW("WAVE STATE", "%s",            g_assets.pal_accent2, bp_waves_state_name());
    ROW("QUALITY",    "%s",            g_assets.pal_text, bp_gfx_quality_name(g_gfx.quality));
    #undef ROW

    /* 池壓力才是真正會把遊戲弄壞的東西，所以讓它自佔一行 */
    bp_ui_text(x + 6.0f, ty, UI_FS_TINY, g_assets.pal_text_dim, "POOL");
    bp_ui_text(x + w - 6.0f, ty, UI_FS_TINY, g_assets.pal_text, "E%d P%d D%d",
               g_enemies.pool.count, g_fx.particle_live, g_fx.decal_live);
}

/* -------------------------------------------------------------------------------------- */
/* 每幀繪製                                                                                    */
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
        /* 競技用的最小化模式：血量數字、彈藥數字、準星，除此之外一無所有。 */
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
