/******************************************************************************************
 * bp_hud.h -- 遊戲中的抬頭顯示器（heads-up display）。
 *
 * HUD 哲學（多數專案做錯的那一部分）
 *   HUD 不是儀表板。交戰中玩家的眼睛盯著畫面中央，而且不會移開；其餘一切都落在周邊視野。
 *   所以：
 *     - 必須被「讀到」的東西（血量、護甲、彈藥）放在下方兩角，正好位於自然的周邊視野帶，
 *       並以「形狀與顏色」作為主要編碼、數字只是確認。玩家應該能用 6 度的餘光瞄到一條紅
 *       色、幾乎空掉的分段量條，就明白「我快死了」，完全不需要看清任何一位數字。
 *     - 必須被「注意到」的東西（波次橫幅、擊殺訊息、彈藥不足、發現隱藏要素）都是短暫的、
 *       會做動畫、然後自行消失。永久性的 HUD 元件只會訓練眼睛去忽略它們。
 *     - 畫面中央只屬於準星與散布指示器，此外別無他物。沒有小地圖、沒有指南針、瞄準點上沒
 *       有任務文字。
 *   因此 HUD 對模擬本身擁有「零」權限。它只讀取全域變數然後繪製。唯一的例外是吐司佇列
 *   （toast queue），那是 UI 自己的事件緩衝區 —— 其他模組把字串推進來，而不是各自在自己
 *   的座標畫出自己的彈窗；那正是 HUD 文字會互相重疊的成因。
 ******************************************************************************************/
#ifndef BP_HUD_H
#define BP_HUD_H

#include "bp_common.h"

#define HUD_MAX_TOASTS    6
#define HUD_MAX_DMGDIRS   5

typedef enum {
    TOAST_INFO = 0,      /* 中性的白／琥珀色   */
    TOAST_GOOD,          /* 拾取物與隱藏要素   */
    TOAST_BAD,           /* 低血量與其他警告   */
    TOAST_BIG            /* 波次橫幅           */
} bp_toast_kind;

typedef struct {
    char  text[96];
    bp_toast_kind kind;
    float age;
    float life;
    float y;             /* 算出的欄位位置，以動畫滑入          */
} bp_toast;

typedef struct {
    float angle;         /* 弧度，相對於視角的偏航角       */
    float age;
    float life;
    float strength;
} bp_dmgdir;

typedef struct {
    /* 平滑過的數值：HUD 絕不瞬跳，因為會瞬跳的數字根本無法閱讀 */
    float health;
    float armor;
    float speed;
    int   ammo_mag;
    int   ammo_reserve;

    /* 短暫顯示 */
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
    bool      minimal;      /* 競技模式：只保留血量、彈藥與準星               */
    bool      show_fps;
    float     time;
} bp_hud;

extern bp_hud g_hud;

void  bp_hud_init(void);
void  bp_hud_reset(void);
void  bp_hud_update(float dt);
void  bp_hud_draw(void);          /* 在 bp_gfx_end_scene 之後、bp_ui 幀內呼叫   */

void  bp_hud_toast(const char *text, bp_toast_kind kind, float life);
void  bp_hud_banner(const char *title, const char *subtitle, float life);
void  bp_hud_notify_damage(float angle, float strength);
void  bp_hud_set_visible(bool visible);

#endif /* BP_HUD_H */
