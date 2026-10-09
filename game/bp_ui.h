/******************************************************************************************
 * bp_ui.h -- 即時模式（immediate mode）的 2D UI 原件：像素對齊的文字、面板、量條與版面。
 *
 * 為什麼採用即時模式
 *   保留式（retained）widget 樹是編輯器的正確答案，卻是遊戲 HUD 的錯誤答案。HUD 每個畫面
 *   都從即時模擬狀態從頭重畫，沒有任何值得追蹤的持久身分，而且絕對不能配置記憶體。即時
 *   模式一次給了我們這三項性質，也意味著 HUD 元件不可能「變舊（stale）」—— 根本沒有東西
 *   可以變舊。
 *
 * 虛擬畫布
 *   所有 UI 都以固定的 640x360 虛擬空間（16:9）撰寫，再以「整數」倍率縮放至視窗，並以黑
 *   邊（letterbox）置中。這帶來兩個要點：
 *     - 5x7 點陣字體正好落在像素邊界上，因此文字在任何視窗尺寸下都銳利，而不是被重新取
 *       樣成一團糊。UI 縮放不是整數，正是多數復古風格遊戲看起來模糊的原因。
 *     - 版面與解析度無關。一個寬 200 單位的面板，無論玩家用的是 720p 筆電還是 4K 螢幕都
 *       是寬 200 單位，因此 HUD 永遠不需要依解析度逐一微調。
 *   無法整除的部分一律置中、多出來的留黑 —— 這正是我們想要的：一條硬邊邊框，而不是半像素
 *   的接縫。
 *
 * 繪製順序契約
 *   bp_ui_begin_frame() 必須在 3D 場景解析（resolve）到後緩衝（bp_gfx_end_scene）之後、
 *   EndDrawing() 之前呼叫。UI 絕不畫進場景算圖目標 —— 即使 3D 以 0.5x 解析度渲染，它也
 *   必須在原生解析度下保持銳利，這正是解析度滑桿存在的全部意義。
 ******************************************************************************************/
#ifndef BP_UI_H
#define BP_UI_H

#include "bp_common.h"

#define UI_VW 640
#define UI_VH 360

typedef enum {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER,
    UI_ALIGN_RIGHT
} bp_ui_align;

/* 字級以虛擬像素（virtual pixel）標示。bp_ui 會把每個請求對齊到點陣字體格高（cell
 * height）的最接近整數倍，因此要求 13px 實際會以 14px 渲染。這是刻意的：另一條路就是使用
 * 經過過濾（filtered）的點陣字體。 */
#define UI_FS_TINY     7      /* font_small x1 */
#define UI_FS_SMALL   14      /* font_main  x1 */
#define UI_FS_MED     21      /* font_main x1.5 -> 對齊到 font_big x0.75；見上方 */
#define UI_FS_LARGE   28      /* font_big   x1   */
#define UI_FS_XL      42      /* font_big x1.5 -> 已對齊                   */
#define UI_FS_HUGE    56      /* font_huge  x1   */
#define UI_FS_TITLE  112      /* font_huge  x2   */

typedef struct {
    int   vw, vh;            /* 虛擬畫布尺寸                               */
    int   scale;             /* 縮放至視窗像素的整數倍率                   */
    int   ox, oy;            /* 視窗內的黑邊偏移（letterbox offset）       */
    int   win_w, win_h;

    int   cursor_x, cursor_y;
    bp_ui_align align;

    /* 捲軸由 UI 模組自己擁有，因為面板擁有各自的裁切區。scissor_* 是目前捲動器真正裁切到
     * 的視窗座標矩形，會在呼叫 bp_ui_scroll_begin() 之前由 bp_ui_scroll_region() 設定。 */
    float scroll_target;
    float scroll_current;
    int   scissor_x, scissor_y, scissor_w, scissor_h;

    bool  in_frame;
} bp_ui;

extern bp_ui g_ui;

void  bp_ui_init(void);
void  bp_ui_resize(void);
void  bp_ui_begin_frame(void);
void  bp_ui_end_frame(void);

/* 座標轉換輔助函式 */
float bp_ui_v2sx(float vx);          /* 虛擬 -> 螢幕 x                     */
float bp_ui_v2sy(float vy);
float bp_ui_s2vx(float sx);          /* 螢幕 -> 虛擬（滑鼠座標）           */
float bp_ui_s2vy(float sy);
Vector2 bp_ui_mouse_virtual(void);
int   bp_ui_scale(void);

/* 文字 */
void  bp_ui_text(float x, float y, float size, Color c, const char *fmt, ...);
void  bp_ui_text_align(float x, float y, float size, bp_ui_align a, Color c, const char *fmt, ...);
void  bp_ui_text_shadow(float x, float y, float size, bp_ui_align a, Color c, Color shadow, const char *fmt, ...);
float bp_ui_text_width(const char *text, float size);
float bp_ui_text_height(float size);
void  bp_ui_text_wrapped(float x, float y, float maxWidth, float size, Color c, const char *text);

/* 圖形 —— 座標皆為虛擬座標，填色一律對齊像素 */
void  bp_ui_rect(float x, float y, float w, float h, Color c);
void  bp_ui_rect_outline(float x, float y, float w, float h, float t, Color c);
void  bp_ui_panel(float x, float y, float w, float h, Color fill, Color border);
void  bp_ui_panel_notch(float x, float y, float w, float h, float notch, Color fill, Color border);
void  bp_ui_line(float x0, float y0, float x1, float y1, float t, Color c);
void  bp_ui_gradient(float x, float y, float w, float h, Color left, Color right);
void  bp_ui_vgradient(float x, float y, float w, float h, Color top, Color bottom);
void  bp_ui_dim(float alpha);                     /* 全螢幕淡入淡出        */
void  bp_ui_bar(float x, float y, float w, float h, float frac, Color fill, Color back, Color border);
void  bp_ui_bar_segmented(float x, float y, float w, float h, float frac, int segments,
                          Color fill, Color back, Color border);
void  bp_ui_icon_diamond(float x, float y, float r, Color c);
void  bp_ui_crosshair(float x, float y, float gap, float len, float t, Color c, float hitMarker);

/* 捲軸 */
/* 設定 scroll_begin／scroll_end 將使用的裁切矩形（以虛擬座標表示）。 */
void  bp_ui_scroll_region(float x, float y, float w, float h);
void  bp_ui_scroll_begin(float contentHeight, float viewHeight);
void  bp_ui_scroll_end(void);
float bp_ui_scroll_offset(void);

/* 其他 */
void  bp_ui_set_align(bp_ui_align a);
void  bp_ui_cursor(float x, float y);
void  bp_ui_advance(float dy);
Color bp_ui_fade(Color c, float alpha01);
Color bp_ui_mix(Color a, Color b, float t);

#endif /* BP_UI_H */
