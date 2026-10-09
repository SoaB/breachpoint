/******************************************************************************************
 * bp_log.c -- 環形緩衝區日誌記錄器。
 *
 * 刻意與 raylib 的 TraceLog 分開：這樣主控台／開發疊層（dev overlay）才能顯示與 stdout 完全
 * 相同的歷史記錄，QA 也能透過除錯疊層，把 Web 版的緩衝區內容複製出來。
 ******************************************************************************************/
#include "bp_common.h"
#include <stdarg.h>
#include <time.h>

/* -------------------------------------------------------------------------------------- */
/* 引擎單例（singleton）刻意放在這裡 —— 也就是最底層的編譯單元 —— 是因為其他所有東西都會
 * 引入 bp_common.h，因此對它都有一份「主張權」。若把定義放在較高的模組（bp_gfx、bp_game），
 * 等於要求連結器（linker）透過一個葉端子系統來解析全域狀態；而這正是此檔案配置所要預防的
 * 那種「意外耦合（accidental coupling）」。 */
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
 * 唯一核准可以切換 raylib 事件等待（event waiting）模式的地方。
 *
 * raylib 的 vsync 提示只涵蓋緩衝區交換；阻塞等待輸入是另一個旗標（EnableEventWaiting），
 * 它會停在 EndDrawing -> PollInputEvents 裡面。遊戲閒置時這能省下一整個 CPU 核心，所以我們
 * 確實想要它 —— 但若在開機完成之前就啟用，任何不會送事件的顯示環境（無頭、失焦、資訊站）
 * 都會當機，因為開機流程每一幀只推進一個子系統，而下一幀永遠不會開始。完整的推理請見
 * bp_engine::event_wait_allowed。
 *
 * bp_common.h 已經引入 raylib.h，所以這個函式留在最底層的編譯單元裡，而所有呼叫端 —— 存檔
 * 設定、開機完成、自我測試（self-test）—— 共用同一條程式路徑。
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

    /* 初始化引擎單例。這段程式在 InitWindow 之前執行（main.c 會最先呼叫它），所以絕不能碰
     * 任何需要 GL 內容（GL context）的東西 —— 這裡的欄位都只是純資料。time_scale 從 1.0
     * 開始，是因為每個使用者都會拿它來相乘；若整個結構體是零值，模擬就會被凍結。 */
    g_engine.time_scale = 1.0f;
    g_engine.time_total = 0.0f;
    g_engine.render_scale = 1;
    g_engine.vsync = true;
    g_engine.quit_requested = false;
    g_engine.event_wait_allowed = false;   /* 由 main.c 在開機完成後逐幀解鎖                   */
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
    /* 在瀏覽器裡，raylib 的 TraceLog 最終會出現在開發者工具的主控台中，而那正是 Web 玩家
     * 與 QA 會去看的地方。 */
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
