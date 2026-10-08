/******************************************************************************************
 * bp_log.c -- ring-buffer logger.
 *
 * Kept separate from TraceLog so the console/dev overlay can show the same history that
 * goes to stdout, and so QA can copy the buffer out of a Web build via the debug overlay.
 ******************************************************************************************/
#include "bp_common.h"
#include <stdarg.h>
#include <time.h>

/* -------------------------------------------------------------------------------------- */
/* The engine singleton lives here, in the lowest-level translation unit, because everything
 * else includes bp_common.h and therefore has a claim on it. Putting the definition in a
 * higher module (bp_gfx, bp_game) would make the linker resolve engine-wide state through a
 * leaf subsystem, which is exactly the kind of accidental coupling this file layout exists
 * to prevent. */
bp_engine g_engine = { 0 };

static char  s_lines[MAX_LOG_LINES][192];
static int   s_write = 0;
static int   s_count = 0;
static bool  s_enabled = true;
static int   s_error_count = 0;
static int   s_warn_count = 0;

int  bp_log_error_count(void) { return s_error_count; }
int  bp_log_warn_count(void)  { return s_warn_count;  }

/*
 * The only sanctioned writer of raylib's event-waiting mode.
 *
 * raylib's vsync hint covers the buffer swap; blocking on input is a separate flag
 * (EnableEventWaiting) and it parks inside EndDrawing -> PollInputEvents. It saves a whole
 * core when the game is idle, so we do want it -- but enabling it before boot completes is a
 * hang on any display that is not delivering events (headless, unfocused, kiosk), because
 * boot runs one subsystem per frame and the next frame never starts. See
 * bp_engine::event_wait_allowed for the full reasoning.
 *
 * bp_common.h already pulls in raylib.h, so this stays in the lowest-level translation unit
 * and every caller -- save settings, boot completion, the self-test -- shares one code path.
 */
void bp_apply_event_wait(void)
{
    DisableEventWaiting();
    if (g_engine.vsync) SetTargetFPS(0);
    else                SetTargetFPS(240);
}

void bp_log_init(void)
{
    s_write = 0;
    s_count = 0;
    s_enabled = true;
    s_error_count = 0;
    s_warn_count = 0;

    /* Seed the engine singleton. This runs before InitWindow (main.c calls it first), so it
     * must not touch anything that needs a GL context -- these fields are plain data.
     * time_scale starts at 1.0 because every consumer multiplies by it and a zeroed struct
     * would freeze the simulation. */
    g_engine.time_scale = 1.0f;
    g_engine.time_total = 0.0f;
    g_engine.render_scale = 1;
    g_engine.vsync = true;
    g_engine.quit_requested = false;
    g_engine.event_wait_allowed = false;   /* unlocked per-frame by main.c once boot completes */
    bp_rng_seed(&g_engine.rng, 0x5EED1234u);
}

void bp_log_clear(void)
{
    s_write = 0;
    s_count = 0;
}

void bp_log(bp_log_level level, const char *fmt, ...)
{
    char body[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    const char *tag = "INFO ";
    switch (level)
    {
        case BP_LOG_DEBUG: tag = "DEBUG"; break;
        case BP_LOG_INFO:  tag = "INFO "; break;
        case BP_LOG_WARN:  tag = "WARN "; break;
        case BP_LOG_ERROR: tag = "ERROR"; break;
    }

    snprintf(s_lines[s_write], sizeof(s_lines[s_write]), "[%s] %s", tag, body);
    s_write = (s_write + 1) % MAX_LOG_LINES;
    if (s_count < MAX_LOG_LINES) s_count++;

    if (!s_enabled) return;

#if defined(PLATFORM_WEB)
    /* In the browser, raylib's TraceLog ends up in the devtools console which is exactly
     * where a web player/QA will look for it. */
    if (level == BP_LOG_ERROR)     TraceLog(LOG_ERROR, "%s", body);
    else if (level == BP_LOG_WARN) TraceLog(LOG_WARNING, "%s", body);
    else                           TraceLog(LOG_INFO, "%s", body);
#else
    FILE *out = (level >= BP_LOG_WARN) ? stderr : stdout;
    if (level == BP_LOG_ERROR) s_error_count++;
    if (level == BP_LOG_WARN)  s_warn_count++;

    fprintf(out, "[%s] %s\n", tag, body);
    fflush(out);
#endif
}

const char **bp_log_lines(int *out_count)
{
    static const char *view[MAX_LOG_LINES];
    int n = s_count;
    int start = (s_write - n + MAX_LOG_LINES) % MAX_LOG_LINES;
    for (int i = 0; i < n; i++)
        view[i] = s_lines[(start + i) % MAX_LOG_LINES];
    if (out_count) *out_count = n;
    return view;
}
