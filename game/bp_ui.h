/******************************************************************************************
 * bp_ui.h -- immediate-mode 2D UI primitives: pixel-snapped text, panels, bars, layout.
 *
 * WHY IMMEDIATE MODE
 *   A retained widget tree is the right answer for an editor and the wrong answer for a
 *   game HUD. The HUD is redrawn from scratch every frame from live simulation state, has
 *   no persistent identity worth tracking, and must never allocate. Immediate mode gives us
 *   all three properties for free, and it means a HUD element cannot get "stale" -- there is
 *   nothing to go stale.
 *
 * THE VIRTUAL CANVAS
 *   All UI is authored in a fixed 640x360 virtual space (16:9) and scaled to the window with
 *   an *integer* factor, letterboxed. Two consequences that matter:
 *     - The 5x7 bitmap font lands exactly on pixel boundaries, so text is crisp at every
 *       window size instead of resampling into mush. Non-integer UI scale is the reason most
 *       retro-styled games still look blurry.
 *     - Layout is resolution independent. A panel that is 200 units wide is 200 units wide
 *       whether the player is on a 720p laptop or a 4K monitor, so the HUD never needs
 *       per-resolution tuning.
 *   Anything that does not divide evenly is centred and the leftover is left black, which is
 *   what you want: a hard-edged border instead of a half-pixel seam.
 *
 * DRAW ORDER CONTRACT
 *   bp_ui_begin_frame() must be called after the 3D scene has been resolved to the
 *   backbuffer (bp_gfx_end_scene) and before EndDrawing(). UI is never drawn into the
 *   scene target -- it must stay crisp at the native resolution even when the 3D is
 *   rendered at 0.5x, which is the entire point of the resolution slider.
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

/* Font sizes are authored in virtual pixels. bp_ui snaps each request to the nearest
 * integer multiple of the bitmap font's cell height, so a requested 13px actually renders
 * at 14px. Deliberate: the alternative is a filtered bitmap font. */
#define UI_FS_TINY     7      /* font_small x1 */
#define UI_FS_SMALL   14      /* font_main  x1 */
#define UI_FS_MED     21      /* font_main  x1.5 -> snapped to font_big x0.75; see notes */
#define UI_FS_LARGE   28      /* font_big   x1   */
#define UI_FS_XL      42      /* font_big   x1.5 -> snapped */
#define UI_FS_HUGE    56      /* font_huge  x1   */
#define UI_FS_TITLE  112      /* font_huge  x2   */

typedef struct {
    int   vw, vh;            /* virtual canvas size                        */
    int   scale;             /* integer scale to window pixels             */
    int   ox, oy;            /* letterbox offset in window pixels          */
    int   win_w, win_h;

    int   cursor_x, cursor_y;
    bp_ui_align align;

    /* scrollbars: the UI module owns them because panels own their clipping.
     * scissor_* is the window-space rectangle the current scroller clips to; it is set by
     * bp_ui_scroll_region() before bp_ui_scroll_begin(). */
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

/* coordinate helpers */
float bp_ui_v2sx(float vx);          /* virtual -> screen x                */
float bp_ui_v2sy(float vy);
float bp_ui_s2vx(float sx);          /* screen -> virtual (mouse)          */
float bp_ui_s2vy(float sy);
Vector2 bp_ui_mouse_virtual(void);
int   bp_ui_scale(void);

/* text */
void  bp_ui_text(float x, float y, float size, Color c, const char *fmt, ...);
void  bp_ui_text_align(float x, float y, float size, bp_ui_align a, Color c, const char *fmt, ...);
void  bp_ui_text_shadow(float x, float y, float size, bp_ui_align a, Color c, Color shadow, const char *fmt, ...);
float bp_ui_text_width(const char *text, float size);
float bp_ui_text_height(float size);
void  bp_ui_text_wrapped(float x, float y, float maxWidth, float size, Color c, const char *text);

/* shapes -- all coordinates virtual, all fills pixel-snapped */
void  bp_ui_rect(float x, float y, float w, float h, Color c);
void  bp_ui_rect_outline(float x, float y, float w, float h, float t, Color c);
void  bp_ui_panel(float x, float y, float w, float h, Color fill, Color border);
void  bp_ui_panel_notch(float x, float y, float w, float h, float notch, Color fill, Color border);
void  bp_ui_line(float x0, float y0, float x1, float y1, float t, Color c);
void  bp_ui_gradient(float x, float y, float w, float h, Color left, Color right);
void  bp_ui_vgradient(float x, float y, float w, float h, Color top, Color bottom);
void  bp_ui_dim(float alpha);                     /* full-screen fade      */
void  bp_ui_bar(float x, float y, float w, float h, float frac, Color fill, Color back, Color border);
void  bp_ui_bar_segmented(float x, float y, float w, float h, float frac, int segments,
                          Color fill, Color back, Color border);
void  bp_ui_icon_diamond(float x, float y, float r, Color c);
void  bp_ui_crosshair(float x, float y, float gap, float len, float t, Color c, float hitMarker);

/* scrollbars */
/* Sets the clip rectangle (in virtual coordinates) that scroll_begin/end will use. */
void  bp_ui_scroll_region(float x, float y, float w, float h);
void  bp_ui_scroll_begin(float contentHeight, float viewHeight);
void  bp_ui_scroll_end(void);
float bp_ui_scroll_offset(void);

/* misc */
void  bp_ui_set_align(bp_ui_align a);
void  bp_ui_cursor(float x, float y);
void  bp_ui_advance(float dy);
Color bp_ui_fade(Color c, float alpha01);
Color bp_ui_mix(Color a, Color b, float t);

#endif /* BP_UI_H */
