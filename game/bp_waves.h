/******************************************************************************************
 * bp_waves.h -- 遭遇戰導演（encounter director）：何時、在哪裡、生成什麼，以及難度多高。
 *
 * 為什麼要導演，而不是一份手寫的生成清單
 *   手寫的生成清單只能讓關卡刺激一次 —— 玩家第二次遊玩就已背熟套路，遊戲也就結束了。導演
 *   會從關卡既有的生成點組合出每一波，用「螢幕上實際還有多少敵人」來節流，再依玩家的表現
 *   調整強度，於是同一張地圖每次開打都是另一場戰鬥 —— 這正是競技場射擊的全部經濟學論點：
 *   一張地圖，玩上數百小時。
 *
 * 三個旋鈕
 *   1. 組合（composition）：採用哪些原型（archetype）、比例如何，並隨波次編號升級。
 *   2. 壓力（pressure）：同時最多允許多少敵人存活，以及它們以多快的速度陸續湧入。這會
 *      受到 MAX_ONSCREEN 的硬性上限約束，因此再糟的組合也不會演變成幀率問題 —— 導演同
 *      時也就是效能預算。
 *   3. 橡皮筋（rubber band）：一組隱藏的技巧讀值（每秒受傷量 + 命中率），會把壓力悄悄調低
 *      給掙扎中的玩家、調高給佔盡優勢的玩家。刻意做得極輕：最多 ±18%。玩家絕不能感覺到
 *      遊戲「決定放水」，只會覺得難度永遠「剛剛好」。
 *
 * 擺放規則（強制執行，而非建議）
 *   - 只要存在更遠的生成點，就絕不生成在玩家視野錐形（view cone）內：憑空出現在玩家臉上
 *     會被當成作弊，也抹煞了掃視競技場的價值
 *   - 絕不近於 MIN_SPAWN_DIST：敵人一出生就造成接觸傷害是不可接受的
 *   - 優先使用玩家最近沒用過的生成點（對生成點集合做輪流 round robin）
 *   - 若移動平台位移之後該點落在幾何體內部，這次生成會被直接拒絕 —— 如此一來，正在關閉
 *     的防爆門永遠不可能把敵人活埋
 ******************************************************************************************/
#ifndef BP_WAVES_H
#define BP_WAVES_H

#include "bp_common.h"

typedef enum {
    WAVE_IDLE = 0,        /* 關卡之間，或第一波尚未開始的空檔                       */
    WAVE_INTRO,           /* 橫幅已升起、尚無敵人的短暫喘息                         */
    WAVE_ACTIVE,          /* 持續生成敵人並交戰中                                   */
    WAVE_CLEARING,        /* 本波預算已耗盡，等待倖存者被消滅                       */
    WAVE_INTERMISSION,    /* 全數殲滅：補給、結算分數、下一波倒數                   */
    WAVE_COMPLETE         /* 本關所有波次結束，交棒給出口流程                       */
} bp_wave_state;

typedef struct {
    /* 關卡設定 */
    int          level_index;
    int          wave_total;         /* 取自關卡資料表的波次總數                    */
    int          difficulty;         /* 0 放鬆 .. 3 夢魘                            */

    /* 目前波次 */
    int          wave_index;         /* 進行中從 1 起算                             */
    bp_wave_state state;
    float        state_time;         /* 目前狀態已持續的秒數                        */
    float        level_time;         /* 關卡開始至今的秒數                          */

    int          budget;             /* 本波仍待生成的敵人數                        */
    int          spawned_this_wave;
    int          killed_this_wave;
    int          killed_this_level;

    /* 正在組合的這一波：各原型（archetype）剩餘待生成的數量 */
    int          pending[4];

    /* 難度倍率，在關卡開始時從 g_difficulties[] 解析一次。敵人直接讀取這些欄位，而不是各自
     * 攜帶一份副本：難度是「這一場遊玩」的屬性，而不是原型的屬性；若每個敵人都複製一份，
     * 關卡中途變更難度就會有一半的敵人套用到舊值。 */
    float        diff_health;
    float        diff_damage;
    float        diff_score;
    float        diff_resupply;

    /* 壓力 */
    int          max_onscreen;       /* 同時存活敵人的硬性上限                      */
    float        spawn_interval;     /* 每次零星生成之間的秒數                      */
    float        spawn_timer;
    int          spawn_cursor;       /* 對生成點集合輪流的索引（round robin）       */
    float        skill;              /* -1 掙扎 .. +1 佔優                          */
    float        aggression;         /* 0.7 .. 1.3，乘算壓力的係數                  */

    /* 休整期 */
    float        intermission_time;
    float        intermission_left;
    bool         resupply_given;

    /* 統計 */
    int          waves_completed;
    int          spawns_rejected;    /* 擺放被拒：位於幾何體內／距離過近            */
    float        longest_wave;
    float        fastest_wave;
    int          perfect_waves;      /* 未受傷即順利清除的波次                      */
    bool         damage_taken_this_wave;

    bool         initialised;
} bp_wave_sys;

extern bp_wave_sys g_waves;

void  bp_waves_init(void);
void  bp_waves_start_level(int levelIndex);
void  bp_waves_update(float dt);

/* 敵人死亡的瞬間由 bp_enemies 呼叫。刻意宣告在這裡，好讓依賴關係只有一個方向（enemies ->
 * waves）：waves 模組永遠不需要知道 enemy 結構體長什麼樣子。 */
void  bp_waves_notify_kill(int enemyType);

bool  bp_waves_is_active(void);
bool  bp_waves_level_complete(void);
int   bp_waves_remaining(void);        /* 預算加現存敵人：HUD 倒數所顯示的數字      */
float bp_waves_progress01(void);
const char *bp_waves_state_name(void);

/* 難度預設值。對外公開，好讓選單能顯示「選了這一項究竟改變了什麼」。 */
typedef struct {
    const char *name;
    const char *blurb;
    float       enemy_health;     /* 乘上原型（archetype）生命的倍率                */
    float       enemy_damage;     /* 乘上攻擊與投射物傷害的倍率                     */
    float       pressure;         /* 乘上 max_onscreen 與生成速率的倍率             */
    float       score;            /* 計分用的分數倍率                               */
    float       resupply;         /* 休整期補給（彈藥／血量）佔比                   */
} bp_difficulty_def;

extern const bp_difficulty_def g_difficulties[];
extern const int g_difficulty_count;

#endif /* BP_WAVES_H */
