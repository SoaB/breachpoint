/******************************************************************************************
 * bp_common.h -- BREACH POINT :: 共用型別、巨集與引擎層級常數
 * ----------------------------------------------------------------------------------------
 * 每個模組都會最先引入此檔。這裡不做任何配置（allocation），也不依賴任何遊戲邏輯，因此引入
 * 圖（include graph）得以維持嚴格的有向無環圖（DAG）：
 *
 *      bp_common.h  <-  （引擎模組）  <-  （遊戲模組）  <-  main.c
 *
 * 全程式庫一致遵守的設計規則：
 *   1. 開機（boot）之後禁止任何動態配置。每一個池（pool）的容量都由下方的 MAX_* 常數決定，
 *      並在 bp_boot() 中一次性預先配置。這讓每幀的耗時維持平坦，也讓 Web/WASM 版本的線性
 *      記憶體佔用量是可預測的。
 *   2. 除了各標頭檔中宣告的模組單例（singleton）之外，禁止任何全域可變狀態。
 *   3. 固定步進模擬（SIM_DT）搭配繪製插值 -> 在 30 Hz 與 300 Hz 下玩法完全一致，這是競技
 *      射擊遊戲的硬性要求。
 ******************************************************************************************/
#ifndef BP_COMMON_H
#define BP_COMMON_H

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================================== */
/* 建置識別資訊                                                                              */
/* ====================================================================================== */
#define BP_GAME_NAME        "BREACH POINT"
#define BP_GAME_TAGLINE     "A high-velocity arena shooter"
#define BP_VERSION_MAJOR    0
#define BP_VERSION_MINOR    9
#define BP_VERSION_PATCH    0
#define BP_VERSION_STRING   "0.9.0"
#define BP_STUDIO           "Nullstate Interactive"

#if defined(PLATFORM_WEB)
    #define BP_PLATFORM_NAME "WebAssembly"
#elif defined(_WIN32)
    #define BP_PLATFORM_NAME "Windows"
#elif defined(__APPLE__)
    #define BP_PLATFORM_NAME "macOS"
#elif defined(__linux__)
    #define BP_PLATFORM_NAME "Linux"
#else
    #define BP_PLATFORM_NAME "Unknown"
#endif

/* ====================================================================================== */
/* 池容量 —— 遊戲的「預算」。請調整這些數值，執行時期絕不呼叫 malloc。                     */
/* ====================================================================================== */
#define MAX_ENTITIES            1024    /* 一般池化實體（道具、觸發器等）                 */
#define MAX_ENEMIES              128    /* 畫面上存活的敵人                               */
#define MAX_PROJECTILES          256    /* 玩家與敵人的飛射物                             */
#define MAX_HITSCAN_TRACERS       32    /* 可見的光束拖曳痕跡                             */
#define MAX_PARTICLES           4096    /* 撞擊火花、煙霧、血跡、彈殼                     */
#define MAX_DECALS               256    /* 幾何體上的彈孔與焦痕                           */
#define MAX_PICKUPS              128    /* 生命、護甲、彈藥補給                           */
#define MAX_DAMAGE_NUMBERS       128    /* 浮動的傷害數字                                 */
#define MAX_SFX_VOICES            32    /* 同時播放的一次性音效                           */
#define MAX_MAP_CELLS         128 * 128 /* 碰撞網格                                       */
#define MAX_MAP_SPAWNS          64      /* 敵人出生點                                     */
#define MAX_MAP_ARENAS          24      /* 競技場幾何方塊                                 */
#define MAX_WEAPON_DEFS           12    /* 從資料檔載入的武器                             */
#define MAX_ENEMY_DEFS            16    /* 從資料檔載入的敵人類型                         */
#define MAX_LEVELS                 8    /* 關卡表格的項目                                 */
#define MAX_MENU_ITEMS            24    /* 單一選單的列數                                 */
#define MAX_LOG_LINES            256    /* 引擎／遊戲日誌的環形緩衝區                     */
#define MAX_SAVE_SLOTS             4

/* ====================================================================================== */
/* 模擬常數                                                                                  */
/* ====================================================================================== */
#define SIM_HZ                  120.0f  /* 每秒的物理／邏輯更新次數                       */
#define SIM_DT              (1.0f / SIM_HZ)
#define MAX_FRAME_DT            0.1f    /* 夾限：避免卡頓後的「死亡螺旋」                 */
#define MAX_SIM_STEPS_PER_FRAME   8

#define GRAVITY                 -28.0f  /* 單位／秒² —— 比真實重力更「跟手」           */
#define UNIT_SCALE               1.0f   /* 1 個世界單位 == 1 公尺                         */

#define PLAYER_EYE_HEIGHT        1.62f
#define PLAYER_RADIUS            0.42f
#define PLAYER_HEIGHT            1.85f
#define PLAYER_MAX_HEALTH       100.0f
#define PLAYER_MAX_ARMOUR       100.0f

/* ====================================================================================== */
/* 小工具                                                                                    */
/* ====================================================================================== */
#define BP_ARRAY_COUNT(a)   ((int)(sizeof(a) / sizeof((a)[0])))
/*
 * MIN / MAX / CLAMP：極小值、極大值與夾限三個巨集。
 *
 * 注意：這些只是純粹的三元運算式，引數最多會被求值三次。千萬不要傳入帶有副作用的運算式 ——
 * BP_MAX(1, atoi(argv[++i])) 會把 i 遞增兩次，並越過 argv 的結尾，結果是一場毫無診斷訊息的
 * 段錯誤（segfault）。請先指定給區域變數。（C11 沒有乾淨的寫法能把這些寫成可攜的
 * statement-expression —— 在 gcc/clang/emcc 底下、又要在 -Wshadow 之下不產生警告是不可能的，
 * 所以這條規則只能以程式碼審查（review）約束，而不是靠編譯器強制。）
 */
#define BP_MIN(a, b)        ((a) < (b) ? (a) : (b))
#define BP_MAX(a, b)        ((a) > (b) ? (a) : (b))
#define BP_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define BP_ABS(a)           ((a) < 0 ? -(a) : (a))
#define BP_SIGN(a)          ((a) < 0 ? -1.0f : 1.0f)
#define BP_UNUSED(x)        ((void)(x))

#define BP_PI               3.14159265358979323846f
#define BP_DEG2RAD          (BP_PI / 180.0f)
#define BP_RAD2DEG          (180.0f / BP_PI)
#define BP_TAU              (BP_PI * 2.0f)

/* 線性插值做成函式而非巨集。raylib 自帶的 `Lerp` 是一個對 double 運作的巨集，每個呼叫端都會
 * 觸發 -Wdouble-promotion，並悄悄把整個運算式提升成雙精度；這個版本則讓每一個模擬步都維持
 * 單精度（single precision）。 */
static inline float bp_lerp(float a, float b, float t) { return a + (b - a) * t; }
#define BP_LERP(a, b, t)    bp_lerp((a), (b), (t))

/* 與幀率無關的平滑指數逼近（exponential approach）。 */
static inline float bp_damp(float current, float target, float lambda, float dt)
{
    return target + (current - target) * expf(-lambda * dt);
}

/* 最多把 `current` 朝 `target` 移動 `maxDelta` 的量。 */
static inline float bp_approach(float current, float target, float maxDelta)
{
    float d = target - current;
    if (d >  maxDelta) return current + maxDelta;
    if (d < -maxDelta) return current - maxDelta;
    return target;
}

static inline float bp_lerp_angle(float a, float b, float t)
{
    float d = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
    return a + d * t;
}

/* ====================================================================================== */
/* 日誌記錄                                                                                  */
/* ====================================================================================== */
typedef enum {
    BP_LOG_DEBUG = 0,
    BP_LOG_INFO,
    BP_LOG_WARN,
    BP_LOG_ERROR
} bp_log_level;

void bp_log(bp_log_level level, const char *fmt, ...);
const char **bp_log_lines(int *out_count);   /* 環形緩衝區，最舊的排在最前面   */
void bp_log_clear(void);

#define BP_LOGD(...) bp_log(BP_LOG_DEBUG, __VA_ARGS__)
#define BP_LOGI(...) bp_log(BP_LOG_INFO,  __VA_ARGS__)
#define BP_LOGW(...) bp_log(BP_LOG_WARN,  __VA_ARGS__)
#define BP_LOGE(...) bp_log(BP_LOG_ERROR, __VA_ARGS__)

/* ====================================================================================== */
/* 決定性 RNG（xorshift32）—— 每次執行都重新播種，讓回放／示範可被重現                     */
/* ====================================================================================== */
typedef struct { uint32_t state; } bp_rng;

static inline void bp_rng_seed(bp_rng *r, uint32_t seed)
{
    r->state = seed ? seed : 0x9E3779B9u;
}

static inline uint32_t bp_rng_next(bp_rng *r)
{
    uint32_t x = r->state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->state = x;
    return x;
}

static inline float bp_rng_range(bp_rng *r, float lo, float hi)
{
    return lo + (hi - lo) * ((float)(bp_rng_next(r) & 0xFFFFFF) / 16777215.0f);
}

static inline int bp_rng_int(bp_rng *r, int n) { return (n > 0) ? (int)(bp_rng_next(r) % (uint32_t)n) : 0; }

/* ====================================================================================== */
/* 計時／效能量測                                                                            */
/* ====================================================================================== */
typedef struct {
    float frame_ms;         /* 每幀平滑後的實際耗時                     */
    float sim_ms;           /* 固定步進更新所佔的時間                  */
    float draw_ms;          /* 下達繪製呼叫所佔的時間                  */
    int   fps;
    int   sim_steps;        /* 上一幀實際執行的模擬次數                 */
    int   draw_calls;
    int   tris;
    bool  show_overlay;
} bp_perf;

/* ====================================================================================== */
/* 全域引擎單例                                                                              */
/* ====================================================================================== */
typedef struct {
    bp_rng      rng;
    bp_perf     perf;
    float       time_total;     /* 開機後經過的秒數                         */
    float       time_scale;     /* 慢動作／加速倍率                        */
    int         screen_w;
    int         screen_h;
    int         render_scale;   /* 內部解析度除數（效能旋鈕）              */
    bool        vsync;
    bool        window_focused;
    bool        quit_requested;

    /* 准許幀迴圈阻塞（block）在 OS 事件佇列上的授權。
     *
     * raylib 的 vsync 只涵蓋緩衝區交換（buffer swap）；阻塞等待輸入是另一個獨立的旗標
     * （EnableEventWaiting，它會停在 EndDrawing -> PollInputEvents 裡面）。閒置時這能省下
     * 一整個 CPU 核心，我們確實想要它 —— 但若在開機完成之前就啟用，那麼任何沒有輸入活動的
     * 顯示環境（無頭、失焦視窗、資訊站）永遠不會送出 EndDrawing 正在等待的事件，後續的幀
     * 也就再也不會執行。開機流程是每一幀只推進一個子系統，於是症狀會是黑畫面上的當機畫面、
     * 載入條停在約 75%。這個旗標就是「現在可以阻塞了」這件事唯一的編碼位置。 */
    bool        event_wait_allowed;
    char        data_dir[256];  /* 可寫入的存檔目錄（依平台而定）          */
} bp_engine;

extern bp_engine g_engine;

/* ====================================================================================== */
/* 引擎子系統進入點                                                                          */
/* ====================================================================================== */
void bp_log_init(void);

/* 計數器，讓測試框架可以直接斷言「這次執行沒有任何 error」，而不必去剖析（scrape）stdout。
 * 警告另行計數：缺少一個選用的設定檔只算警告，永遠不該讓建置失敗。 */
int  bp_log_error_count(void);
int  bp_log_warn_count(void);

/* 開啟或關閉事件等待的唯一核准途徑。詳見 bp_engine.event_wait_allowed。 */
void bp_apply_event_wait(void);
void bp_assets_init(void);      /* 程序化材質、模型、字型             */
void bp_assets_shutdown(void);
void bp_audio_init(void);       /* 程序化音效與音樂合成               */
void bp_audio_shutdown(void);
void bp_audio_update(float dt);
void bp_gfx_init(void);
void bp_gfx_begin_frame(void);
void bp_gfx_end_frame(void);

#endif /* BP_COMMON_H */
