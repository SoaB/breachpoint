/******************************************************************************************
 * bp_save.h -- 設定檔（settings profile）+ 單局紀錄 + 依平台而異的持久化。
 *
 * 檔案格式：與 assets/cfg 底下檔案相同的「逐行 key/value」語法，由 engine/bp_config.c 解析。
 * 對這個規模的遊戲而言，這是正確的選擇，理由有三：
 *   1. 一個 parser、一套文法、一份 bug。二進位格式勢必需要第二個讀取器、第二個 fuzzer，
 *      以及第二條資料遷移路徑。
 *   2. 存檔可讀、也可手動編輯 —— 這正是此類遊戲的玩家所期待的（也讓客服工單能在幾秒內
 *      得到答案）。
 *   3. 在網頁版中原樣往返（round-trip）：localStorage 存的本來就是字串。
 *
 * 耐久性保證
 *   - 每次寫入都是原子的：先序列化到暫存緩衝區、寫入 "<name>.tmp"，再以 rename() 取代（不
 *     需要 fsync）。寫入途中當機，絕不會留下一份「被截斷、卻被下次開機信任」的存檔。
 *     （rename() 在 POSIX 與 Emscripten 的 MEMFS/IDBFS 上都是原子操作。）
 *   - 檔案不存在或損毀都不算錯誤：bp_save_load() 會退回編譯進去的預設設定檔，並記一筆
 *     警告。遊戲永遠必須開得起來。
 *   - 會檢查 `version`。若檔案比我們能理解的還新，會原封不動地保留而不是覆寫，這樣把版本
 *     降回去的玩家就不會因此失去紀錄。
 *
 * 平台
 *   桌機：<使用者資料目錄>/BreachPoint/settings.cfg 與 progress.cfg。
 *   網頁：同樣這兩個路徑放在 Emscripten 的虛擬檔案系統中，透過 IDBFS 掛載到 IndexedDB，再
 *         以 FS.syncfs() 同步。少了這一步明確同步，瀏覽器在重新整理時會把內容全部丟棄 ——
 *         這正是最常見的網頁存檔 bug。
 ******************************************************************************************/
#ifndef BP_SAVE_H
#define BP_SAVE_H

#include "bp_common.h"
#include "../engine/bp_gfx.h"
#include "../engine/bp_input.h"

#define SAVE_VERSION        3
#define SAVE_MAX_LEVELS    16
#define SAVE_NAME_LEN      32

typedef enum {
    SAVE_OK = 0,
    SAVE_MISSING,        /* 還沒有檔案：首次執行                            */
    SAVE_CORRUPT,        /* 無法解析：改用預設值，原檔案不動                */
    SAVE_OLD_VERSION,    /* 比 SAVE_VERSION 還舊：已進行資料遷移            */
    SAVE_NEW_VERSION,    /* 比我們能理解的還新：保留但不載入                */
    SAVE_IO_ERROR
} bp_save_result;

/* -------------------------------------------------------------------------------------- */
/* 設定檔：選項選單可調整的所有項目                                                           */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    /* 影像 */
    bp_quality quality;
    float      resolution_scale;   /* 覆寫值；<=0 表示「沿用畫質預設值」          */
    int        fov;                /* 70..120 度                                 */
    bool       vsync;
    bool       crt_filter;
    bool       fullscreen;
    int        window_w, window_h;

    /* 音訊 */
    float      vol_master;
    float      vol_sfx;
    float      vol_music;

    /* 輸入 */
    float      sensitivity;
    bool       invert_y;
    int        binds[ACT_COUNT][BP_MAX_BINDS_PER_ACTION];   /* 按鍵代碼 */
    int        pad_binds[ACT_COUNT][2];

    /* HUD */
    bool       hud_visible;
    bool       hud_minimal;
    bool       show_fps;

    /* 遊戲 */
    int        difficulty;
    int        last_level;

    char       profile_name[SAVE_NAME_LEN];
} bp_settings;

/* -------------------------------------------------------------------------------------- */
/* 單局紀錄：每個關卡、每種難度各一列                                                        */
/* -------------------------------------------------------------------------------------- */
typedef struct {
    bool  completed;
    bool  secret_found;
    int   best_score;
    int   best_kills;
    float best_time;        /* 0 = 從未完成 */
    float best_accuracy;    /* 0..1 */
    int   best_wave;
    int   attempts;
    int   perfect_waves;
    int   total_deaths;
} bp_level_record;

typedef struct {
    int             version;
    bp_settings     settings;
    bp_level_record levels[SAVE_MAX_LEVELS][4];   /* [關卡][難度] */

    /* 全域總計，供戰績畫面使用 */
    int             total_runs;
    int             total_kills;
    int             total_deaths;
    float           total_play_time;
    int             highest_streak;
    int             best_score;

    /* 執行期資料，絕不寫入存檔 */
    char            path_settings[256];
    char            path_progress[256];
    bp_save_result  last_result;
    bool            dirty;
    bool            loaded;
} bp_save;

extern bp_save g_save;

/* 生命週期
 *
 * bp_save_boot() 與 bp_save_init() 刻意拆成兩個函式。建立視窗需要設定檔（尺寸、vsync、
 * MSAA），但那時繪圖、音訊裝置與輸入表都還不存在，若在那個時間點就套用設定，等於去解參考
 * 尚未初始化的子系統。boot 只負責讀檔，別無其他；init 則在這些子系統都起來之後才被呼叫。 */
void           bp_save_boot(void);               /* InitWindow 之前即可安全呼叫：路徑 + 讀檔 */
void           bp_save_init(void);               /* 等 gfx/audio/input 就緒後：重新套用       */
bp_save_result bp_save_load(void);
bp_save_result bp_save_write_settings(void);
bp_save_result bp_save_write_progress(void);
void           bp_save_apply_settings(void);     /* 把設定推給正在運行的各子系統 */
void           bp_save_defaults(void);

/* 進度 */
bp_level_record *bp_save_record(int level, int difficulty);
void             bp_save_submit_run(int level, int difficulty, bool completed, int score,
                                    int kills, float time, float accuracy, int wave,
                                    int perfectWaves, bool secret, bool died);
bool             bp_save_level_unlocked(int level);
int              bp_save_completion_percent(void);

/* 平台 */
void           bp_save_sync_platform(void);      /* 網頁：FS.syncfs；桌機：無事可做（no-op） */
const char    *bp_save_result_name(bp_save_result r);

#endif /* BP_SAVE_H */
