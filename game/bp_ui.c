/******************************************************************************************
 * bp_ui.c -- immediate-mode UI implementation.
 *
 * Two invariants every function here upholds:
 *   1. Pixel snapping. Every coordinate goes through snap() before it reaches a draw call,
 *      and every glyph scale is an integer. A UI that is 1 subpixel off is a UI that looks
 *      cheap, and it is invisible in a screenshot but obvious in motion.
 *   2. No allocation. Text goes through a fixed stack buffer (UI_TEXT_BUF). Longer strings
 *      are truncated rather than growing the buffer -- the HUD has no business rendering a
 *      novel, and a truncation is a visible bug while a heap allocation is an invisible one.
 ******************************************************************************************/
#include "bp_ui.h"

#include "../engine/bp_assets.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bp_ui g_ui = { 0 };

#define UI_TEXT_BUF   1024
#define UI_SCROLL_LAMBDA 14.0f

/* -------------------------------------------------------------------------------------- */
/* coordinate mapping                                                                      */
/* -------------------------------------------------------------------------------------- */
static inline float snapf(float v) { return floorf(v + 0.5f); }
static inline int   snapi(float v) { return (int)floorf(v + 0.5f); }

void bp_ui_resize(void)
{
    g_ui.vw = UI_VW;
    g_ui.vh = UI_VH;
    g_ui.win_w = GetScreenWidth();
    g_ui.win_h = GetScreenHeight();

    /* Integer scale, minimum 1. On a 3840x2160 monitor this is exactly 6, and the 5x7 font
     * renders as a 30x42 bitmap -- crisp, because no filtering is involved anywhere. */
    int s = BP_MIN(g_ui.win_w / UI_VW, g_ui.win_h / UI_VH);
    g_ui.scale = BP_MAX(1, s);

    g_ui.ox = (g_ui.win_w - UI_VW * g_ui.scale) / 2;
    g_ui.oy = (g_ui.win_h - UI_VH * g_ui.scale) / 2;
}

float bp_ui_v2sx(float vx) { return (float)g_ui.ox + vx * (float)g_ui.scale; }
float bp_ui_v2sy(float vy) { return (float)g_ui.oy + vy * (float)g_ui.scale; }
float bp_ui_s2vx(float sx) { return (sx - (float)g_ui.ox) / (float)g_ui.scale; }
float bp_ui_s2vy(float sy) { return (sy - (float)g_ui.oy) / (float)g_ui.scale; }
int   bp_ui_scale(void)    { return g_ui.scale; }

Vector2 bp_ui_mouse_virtual(void)
{
    Vector2 m = GetMousePosition();
    return (Vector2){ bp_ui_s2vx(m.x), bp_ui_s2vy(m.y) };
}

/* -------------------------------------------------------------------------------------- */
/* font selection                                                                          */
/* -------------------------------------------------------------------------------------- */
typedef struct { Font *font; int scale; float size; } ui_font_pick;

/*
 * Map a requested virtual pixel size onto (bitmap font, integer scale).
 *
 * The four generated fonts have base sizes 7, 14, 28, 56. Choosing the *largest base that
 * does not exceed the request* and then scaling it by an integer keeps every glyph a whole
 * number of device pixels. Requesting 21px therefore yields font_main (14) x 1 = 14px or
 * font_big (28) x 1 = 28px; we take the smaller and let the caller ask for 28 if they want
 * the bigger step. Sizes are intentionally coarse -- a bitmap font has no business being
 * asked for 17px.
 */
static ui_font_pick pick_font(float size)
{
    ui_font_pick p;
    if (!g_assets.fonts_valid)
    {
        p.font = NULL; p.scale = 1; p.size = BP_MAX(1.0f, floorf(size));
        return p;
    }

    if (size >= 56.0f)
    {
        int k = BP_MAX(1, (int)(size / 56.0f));
        p.font = &g_assets.font_huge; p.scale = k; p.size = 56.0f * (float)k;
    }
    else if (size >= 28.0f)
    {
        int k = BP_MAX(1, (int)(size / 28.0f));
        p.font = &g_assets.font_big; p.scale = k; p.size = 28.0f * (float)k;
    }
    else if (size >= 14.0f)
    {
        int k = BP_MAX(1, (int)(size / 14.0f));
        p.font = &g_assets.font_main; p.scale = k; p.size = 14.0f * (float)k;
    }
    else
    {
        p.font = &g_assets.font_small; p.scale = 1; p.size = 7.0f;
    }
    return p;
}

float bp_ui_text_height(float size) { return pick_font(size).size; }

/*
 * Text width, computed from the font's own advance table.
 *
 * We deliberately do NOT call MeasureTextEx(): it multiplies the advance by a float ratio
 * (fontSize / baseSize) and applies fractional letter spacing, which will not match what
 * DrawTextEx actually rasterises once we have snapped the size. Measuring and drawing must
 * share one code path or centred text drifts a pixel at a time.
 */
float bp_ui_text_width(const char *text, float size)
{
    if (!text || !text[0]) return 0.0f;
    ui_font_pick p = pick_font(size);
    if (!p.font || !p.font->glyphs) return (float)strlen(text) * p.size * 0.6f;

    float spacing = (float)BP_MAX(1, p.scale);
    float w = 0.0f;
    int defaultIndex = GetGlyphIndex(*p.font, '?');

    for (const char *c = text; *c; c++)
    {
        if (*c == '\n') continue;
        int idx = GetGlyphIndex(*p.font, (unsigned char)*c);
        if (idx < 0) idx = defaultIndex;
        int adv = p.font->glyphs[idx].advanceX;
        if (adv == 0) adv = (int)(p.font->recs[idx].width) + p.font->glyphPadding;
        w += (float)adv * (float)p.scale + spacing;
    }
    return BP_MAX(0.0f, w - spacing);
}

static void draw_text_at(float vx, float vy, float size, Color c, const char *text)
{
    if (!text || !text[0]) return;
    ui_font_pick p = pick_font(size);
    float sx = snapf(bp_ui_v2sx(vx));
    float sy = snapf(bp_ui_v2sy(vy));

    if (!p.font || !p.font->texture.id)
    {
        /* Last-resort fallback so a font generation failure is legible rather than silent. */
        DrawText(text, (int)sx, (int)sy, BP_MAX(8, snapi(p.size)), c);
        return;
    }
    DrawTextEx(*p.font, text, (Vector2){ sx, sy }, p.size, (float)BP_MAX(1, p.scale), c);
}

static void vtext(float x, float y, float size, bp_ui_align a, Color c, const char *fmt, va_list args)
{
    char buf[UI_TEXT_BUF];
    vsnprintf(buf, sizeof(buf), fmt, args);
    buf[UI_TEXT_BUF - 1] = '\0';

    if (a != UI_ALIGN_LEFT)
    {
        float w = bp_ui_text_width(buf, size);
        if (a == UI_ALIGN_CENTER) x -= w * 0.5f;
        else x -= w;
    }
    draw_text_at(x, y, size, c, buf);
}

void bp_ui_text(float x, float y, float size, Color c, const char *fmt, ...)
{
    va_list a; va_start(a, fmt); vtext(x, y, size, UI_ALIGN_LEFT, c, fmt, a); va_end(a);
}

void bp_ui_text_align(float x, float y, float size, bp_ui_align a, Color c, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); vtext(x, y, size, a, c, fmt, args); va_end(args);
}

/*
 * Shadowed text: the same string drawn twice, offset by exactly one font pixel.
 *
 * A 1px offset (not 2px, not a blur) is what keeps a bitmap font readable over bright
 * level geometry without making the text look outlined. Blur would require a second pass
 * and a filter, and filters are precisely what this font must not have.
 */
void bp_ui_text_shadow(float x, float y, float size, bp_ui_align a, Color c, Color shadow, const char *fmt, ...)
{
    char buf[UI_TEXT_BUF];
    va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    buf[UI_TEXT_BUF - 1] = '\0';

    if (a != UI_ALIGN_LEFT)
    {
        float w = bp_ui_text_width(buf, size);
        if (a == UI_ALIGN_CENTER) x -= w * 0.5f;
        else x -= w;
    }
    float off = (float)BP_MAX(1, snapi(pick_font(size).size * 0.07f));
    draw_text_at(x + off, y + off, size, shadow, buf);
    draw_text_at(x, y, size, c, buf);
}

void bp_ui_text_wrapped(float x, float y, float maxWidth, float size, Color c, const char *text)
{
    if (!text) return;
    float lineH = bp_ui_text_height(size) + (float)BP_MAX(1, snapi(size * 0.22f));
    char word[128];
    char line[UI_TEXT_BUF];
    float cy = y;

    line[0] = '\0';
    int wi = 0;

    for (const char *p = text;; p++)
    {
        bool boundary = (*p == ' ' || *p == '\n' || *p == '\0');
        if (!boundary && wi < (int)sizeof(word) - 1) word[wi++] = *p;
        if (!boundary && *p != '\0') continue;
        word[wi] = '\0';
        wi = 0;

        char candidate[UI_TEXT_BUF];
        if (line[0]) snprintf(candidate, sizeof(candidate), "%s %s", line, word);
        else snprintf(candidate, sizeof(candidate), "%s", word);

        if (bp_ui_text_width(candidate, size) > maxWidth && line[0])
        {
            draw_text_at(x, cy, size, c, line);
            cy += lineH;
            snprintf(line, sizeof(line), "%s", word);
        }
        else snprintf(line, sizeof(line), "%s", candidate);

        if (*p == '\0') break;
        if (*p == '\n')
        {
            draw_text_at(x, cy, size, c, line);
            cy += lineH;
            line[0] = '\0';
        }
    }
    if (line[0]) draw_text_at(x, cy, size, c, line);
}

/* -------------------------------------------------------------------------------------- */
/* shapes                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_ui_rect(float x, float y, float w, float h, Color c)
{
    if (w <= 0.0f || h <= 0.0f) return;
    int sx = snapi(bp_ui_v2sx(x)), sy = snapi(bp_ui_v2sy(y));
    int ex = snapi(bp_ui_v2sx(x + w)), ey = snapi(bp_ui_v2sy(y + h));
    DrawRectangle(sx, sy, BP_MAX(1, ex - sx), BP_MAX(1, ey - sy), c);
}

void bp_ui_rect_outline(float x, float y, float w, float h, float t, Color c)
{
    if (w <= 0.0f || h <= 0.0f) return;
    bp_ui_rect(x, y, w, t, c);
    bp_ui_rect(x, y + h - t, w, t, c);
    bp_ui_rect(x, y, t, h, c);
    bp_ui_rect(x + w - t, y, t, h, c);
}

void bp_ui_line(float x0, float y0, float x1, float y1, float t, Color c)
{
    Vector2 a = { snapf(bp_ui_v2sx(x0)), snapf(bp_ui_v2sy(y0)) };
    Vector2 b = { snapf(bp_ui_v2sx(x1)), snapf(bp_ui_v2sy(y1)) };
    DrawLineEx(a, b, BP_MAX(1.0f, t * (float)g_ui.scale), c);
}

void bp_ui_gradient(float x, float y, float w, float h, Color left, Color right)
{
    if (w <= 0.0f || h <= 0.0f) return;
    Rectangle r = { snapf(bp_ui_v2sx(x)), snapf(bp_ui_v2sy(y)),
                    snapf(w * (float)g_ui.scale), snapf(h * (float)g_ui.scale) };
    DrawRectangleGradientEx(r, left, left, right, right);
}

void bp_ui_vgradient(float x, float y, float w, float h, Color top, Color bottom)
{
    if (w <= 0.0f || h <= 0.0f) return;
    Rectangle r = { snapf(bp_ui_v2sx(x)), snapf(bp_ui_v2sy(y)),
                    snapf(w * (float)g_ui.scale), snapf(h * (float)g_ui.scale) };
    DrawRectangleGradientEx(r, top, bottom, top, bottom);
}

/*
 * Panel: a dark fill with a 1px border and a subtle top highlight.
 *
 * The highlight is the detail that makes a flat rectangle read as a physical plate rather
 * than a hole in the screen. It costs one extra 1px rect.
 */
void bp_ui_panel(float x, float y, float w, float h, Color fill, Color border)
{
    bp_ui_rect(x, y, w, h, fill);
    if (border.a > 0) bp_ui_rect_outline(x, y, w, h, 1.0f, border);
    bp_ui_rect(x + 1.0f, y + 1.0f, w - 2.0f, 1.0f, (Color){ 255, 255, 255, 18 });
}

/* Panel with a chamfered top-left corner. Used for the HUD plates so the interface has a
 * machined look consistent with the level geometry's bevelled edges. */
void bp_ui_panel_notch(float x, float y, float w, float h, float notch, Color fill, Color border)
{
    bp_ui_rect(x + notch, y, w - notch, h, fill);
    bp_ui_rect(x, y + notch, w, h - notch, fill);
    if (border.a > 0)
    {
        float t = 1.0f;
        bp_ui_rect(x + notch, y, w - notch, t, border);                 /* top */
        bp_ui_rect(x, y + h - t, w, t, border);                         /* bottom */
        bp_ui_rect(x, y + notch, t, h - notch, border);                 /* left */
        bp_ui_rect(x + w - t, y, t, h, border);                         /* right */
        bp_ui_line(x, y + notch, x + notch, y, t, border);              /* chamfer */
    }
    bp_ui_rect(x + notch + 1.0f, y + 1.0f, w - notch - 2.0f, 1.0f, (Color){ 255, 255, 255, 16 });
}

void bp_ui_dim(float alpha01)
{
    int a = BP_CLAMP((int)(alpha01 * 255.0f), 0, 255);
    DrawRectangle(0, 0, g_ui.win_w, g_ui.win_h, (Color){ 5, 6, 9, (unsigned char)a });
}

void bp_ui_bar(float x, float y, float w, float h, float frac, Color fill, Color back, Color border)
{
    frac = BP_CLAMP(frac, 0.0f, 1.0f);
    bp_ui_rect(x, y, w, h, back);
    if (frac > 0.0f) bp_ui_rect(x + 1.0f, y + 1.0f, (w - 2.0f) * frac, h - 2.0f, fill);
    if (border.a > 0) bp_ui_rect_outline(x, y, w, h, 1.0f, border);
    /* gloss line: a 1px lighter strip along the top of the fill. Without it a bar reads as
     * a flat colour block rather than a gauge. */
    if (frac > 0.02f)
        bp_ui_rect(x + 1.0f, y + 1.0f, (w - 2.0f) * frac, 1.0f,
                   (Color){ (unsigned char)BP_MIN(255, fill.r + 60),
                            (unsigned char)BP_MIN(255, fill.g + 60),
                            (unsigned char)BP_MIN(255, fill.b + 60),
                            (unsigned char)BP_MIN(255, fill.a + 20) });
}

/*
 * Segmented bar: the health/armour readout.
 *
 * Segments are drawn as discrete cells rather than as a continuous fill with tick marks on
 * top, so the player can count "how many segments am I on" without reading a number. That
 * is the whole reason a numeric HUD also has a bar.
 */
void bp_ui_bar_segmented(float x, float y, float w, float h, float frac, int segments,
                         Color fill, Color back, Color border)
{
    frac = BP_CLAMP(frac, 0.0f, 1.0f);
    segments = BP_CLAMP(segments, 1, 64);

    bp_ui_rect(x, y, w, h, back);

    float gap = 1.0f;
    float cellW = (w - 2.0f - gap * (float)(segments - 1)) / (float)segments;
    int lit = (int)floorf(frac * (float)segments + 0.0001f);

    for (int i = 0; i < segments; i++)
    {
        float cx = x + 1.0f + (cellW + gap) * (float)i;
        if (i < lit) bp_ui_rect(cx, y + 1.0f, cellW, h - 2.0f, fill);
        else bp_ui_rect(cx, y + 1.0f, cellW, h - 2.0f, (Color){ fill.r, fill.g, fill.b, 34 });
    }
    /* Partial cell: the remainder between lit segments, so the bar still reads smoothly
     * for values that are not an exact multiple of a segment. */
    float partial = frac * (float)segments - (float)lit;
    if (lit < segments && partial > 0.02f)
    {
        float cx = x + 1.0f + (cellW + gap) * (float)lit;
        bp_ui_rect(cx, y + 1.0f, cellW * partial, h - 2.0f, fill);
    }
    if (border.a > 0) bp_ui_rect_outline(x, y, w, h, 1.0f, border);
}

void bp_ui_icon_diamond(float x, float y, float r, Color c)
{
    /* 4-sided polygon = a diamond, snapped to the pixel grid by drawing at the snapped
     * centre. Used for score/secret markers, which must be recognisable at 12px. */
    Vector2 centre = { snapf(bp_ui_v2sx(x)), snapf(bp_ui_v2sy(y)) };
    DrawPoly(centre, 4, BP_MAX(1.0f, r * (float)g_ui.scale), 45.0f, c);
}

void bp_ui_crosshair(float x, float y, float gap, float len, float t, Color c, float hitMarker)
{
    /* The crosshair is the one UI element that must NOT be pixel-snapped relative to the
     * virtual grid: it is snapped to the exact screen centre, which is where the 3D camera's
     * centre ray projects. A crosshair one pixel off from the true centre means the gun does
     * not shoot where you are looking, and players notice that instantly. */
    float cx = (float)g_ui.win_w * 0.5f;
    float cy = (float)g_ui.win_h * 0.5f;
    float s = (float)g_ui.scale;

    float g = gap * s, l = len * s, th = BP_MAX(1.0f, t * s);
    DrawRectangle((int)(cx - g - l), (int)(cy - th * 0.5f), (int)l, (int)th, c);
    DrawRectangle((int)(cx + g),     (int)(cy - th * 0.5f), (int)l, (int)th, c);
    DrawRectangle((int)(cx - th * 0.5f), (int)(cy - g - l), (int)th, (int)l, c);
    DrawRectangle((int)(cx - th * 0.5f), (int)(cy + g),     (int)th, (int)l, c);

    /* Centre dot: visible only when the spread is tight, so the dot itself communicates
     * accuracy without needing a number anywhere. */
    if (gap < 3.2f) DrawRectangle((int)(cx - 1), (int)(cy - 1), 2, 2, c);

    if (hitMarker > 0.01f)
    {
        /* Hit marker: four ticks that close on the centre as the marker fades. Reading the
         * confirmation as motion rather than as a blink is what makes it legible mid-fight. */
        float m = 3.0f + 9.0f * hitMarker;
        float a = hitMarker * 255.0f;
        Color mc = (Color){ 255, 255, 255, (unsigned char)BP_CLAMP((int)a, 0, 255) };
        float ms = m * s;
        DrawLineEx((Vector2){ cx - ms, cy - ms }, (Vector2){ cx - ms * 0.45f, cy - ms * 0.45f }, th, mc);
        DrawLineEx((Vector2){ cx + ms, cy - ms }, (Vector2){ cx + ms * 0.45f, cy - ms * 0.45f }, th, mc);
        DrawLineEx((Vector2){ cx - ms, cy + ms }, (Vector2){ cx - ms * 0.45f, cy + ms * 0.45f }, th, mc);
        DrawLineEx((Vector2){ cx + ms, cy + ms }, (Vector2){ cx + ms * 0.45f, cy + ms * 0.45f }, th, mc);
    }
}

/* -------------------------------------------------------------------------------------- */
/* scrollbars                                                                              */
/* -------------------------------------------------------------------------------------- */
void bp_ui_scroll_region(float x, float y, float w, float h)
{
    g_ui.scissor_x = snapi(bp_ui_v2sx(x));
    g_ui.scissor_y = snapi(bp_ui_v2sy(y));
    g_ui.scissor_w = BP_MAX(1, snapi(w * (float)g_ui.scale));
    g_ui.scissor_h = BP_MAX(1, snapi(h * (float)g_ui.scale));
}

void bp_ui_scroll_begin(float contentHeight, float viewHeight)
{
    float maxScroll = BP_MAX(0.0f, contentHeight - viewHeight);
    g_ui.scroll_target = BP_CLAMP(g_ui.scroll_target - g_input.wheel * 18.0f, 0.0f, maxScroll);

    /* Smoothed so a wheel notch glides instead of jumping: a list that jumps 18px per notch
     * is unreadable, and a list that glides too slowly feels broken. */
    g_ui.scroll_current = bp_damp(g_ui.scroll_current, g_ui.scroll_target, UI_SCROLL_LAMBDA, GetFrameTime());
    if (BP_ABS(g_ui.scroll_current - g_ui.scroll_target) < 0.05f) g_ui.scroll_current = g_ui.scroll_target;

    BeginScissorMode(g_ui.scissor_x, g_ui.scissor_y, g_ui.scissor_w, g_ui.scissor_h);
}

void bp_ui_scroll_end(void)
{
    EndScissorMode();
}

float bp_ui_scroll_offset(void) { return -g_ui.scroll_current; }

/* -------------------------------------------------------------------------------------- */
/* colour helpers                                                                          */
/* -------------------------------------------------------------------------------------- */
Color bp_ui_fade(Color c, float alpha01)
{
    c.a = (unsigned char)BP_CLAMP((int)((float)c.a * alpha01), 0, 255);
    return c;
}

Color bp_ui_mix(Color a, Color b, float t)
{
    t = BP_CLAMP(t, 0.0f, 1.0f);
    return (Color){
        (unsigned char)BP_CLAMP((int)bp_lerp((float)a.r, (float)b.r, t), 0, 255),
        (unsigned char)BP_CLAMP((int)bp_lerp((float)a.g, (float)b.g, t), 0, 255),
        (unsigned char)BP_CLAMP((int)bp_lerp((float)a.b, (float)b.b, t), 0, 255),
        (unsigned char)BP_CLAMP((int)bp_lerp((float)a.a, (float)b.a, t), 0, 255)
    };
}

/* -------------------------------------------------------------------------------------- */
/* frame / cursor                                                                          */
/* -------------------------------------------------------------------------------------- */
void bp_ui_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    bp_ui_resize();
    BP_LOGI("ui: virtual canvas %dx%d at %dx scale (window %dx%d, letterbox %d,%d)",
            UI_VW, UI_VH, g_ui.scale, g_ui.win_w, g_ui.win_h, g_ui.ox, g_ui.oy);
}

void bp_ui_begin_frame(void)
{
    g_ui.in_frame = true;
    g_ui.cursor_x = 0;
    g_ui.cursor_y = 0;
    g_ui.align = UI_ALIGN_LEFT;
}

void bp_ui_end_frame(void)
{
    g_ui.in_frame = false;
    g_input.wheel = 0.0f;      /* consume the wheel so only one scroller sees it */
}

void bp_ui_set_align(bp_ui_align a) { g_ui.align = a; }
void bp_ui_cursor(float x, float y) { g_ui.cursor_x = snapi(x); g_ui.cursor_y = snapi(y); }
void bp_ui_advance(float dy)        { g_ui.cursor_y += snapi(dy); }
