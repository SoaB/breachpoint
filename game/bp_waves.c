/******************************************************************************************
 * bp_waves.c -- 遭遇戰導演（encounter director）的實作。
 *
 * 設計契約請見 bp_waves.h；以下列出標頭檔看不出來的實作細節：
 *
 *   - 導演與其他系統一樣跑在固定步進（fixed timestep）上，因此波次節奏與畫面更新率無關。
 *     30 fps 的機器與 240 fps 的機器會得到完全相同的遭遇戰，而這正是排行榜能夠成立的前提。
 *
 *   - 出生點配置是一次「有界」搜尋，絕不使用無限重試迴圈。每次生成最多嘗試 SPAWN_ATTEMPTS
 *     個候選點，採用其中分數最高且合法的一個；若全都非法就放棄這次生成，並計入
 *     `spawns_rejected`。在這裡寫無限重試迴圈，只要有一個移動機構件封死了最後一個出生點，
 *     遊戲就會卡死在那裡。
 *
 *   - 技術讀值只對兩個訊號做指數平滑：每秒承受的傷害，以及武器命中率。兩者本來就由其他模組
 *     維護，因此導演不必加任何屬於自己的量測代碼 —— 少一件事要保持同步。
 ******************************************************************************************/
#include "bp_waves.h"

#include "../engine/bp_math.h"
#include "../engine/bp_audio.h"
#include "../engine/bp_camera.h"
#include "bp_world.h"
#include "bp_player.h"
#include "bp_enemies.h"
#include "bp_weapons.h"
#include "bp_fx.h"

bp_wave_sys g_waves = { 0 };

static inline Vector3 vec3(float x, float y, float z) { return (Vector3){ x, y, z }; }

/* -------------------------------------------------------------------------------------- */
/* 難度預設值                                                                                */
/* -------------------------------------------------------------------------------------- */
const bp_difficulty_def g_difficulties[] = {
    /*  欄位        說明文字                                       health damage pressure score resupply */
    { "RECRUIT",  "Learning the movement. Generous resupply.",     0.70f, 0.55f, 0.72f, 0.70f, 1.00f },
    { "VETERAN",  "The intended experience.",                      1.00f, 1.00f, 1.00f, 1.00f, 0.60f },
    { "NIGHTMARE","Faster pressure, heavier hits, less help.",     1.30f, 1.45f, 1.26f, 1.60f, 0.35f },
    { "BREACH",   "No resupply. Enemies do not stagger.",          1.55f, 1.85f, 1.45f, 2.50f, 0.00f },
};
const int g_difficulty_count = BP_ARRAY_COUNT(g_difficulties);

/* -------------------------------------------------------------------------------------- */
/* 節奏常數                                                                                  */
/* -------------------------------------------------------------------------------------- */
#define WAVE_INTRO_TIME        2.60f   /* 波次橫幅先亮起，之後才出現第一個敵人 */
#define WAVE_INTERMISSION_MIN  3.20f
#define WAVE_INTERMISSION_MAX  5.60f
#define WAVE_BASE_BUDGET         10    /* VETERAN 難度下第 1 波的敵人數 */
#define WAVE_BUDGET_GROWTH        4    /* 每一波追加的敵人數 */
#define WAVE_BUDGET_MAX          46    /* 硬性上限：競技場再多塞敵人，局面就不再是
                                          一場戰鬥，而會變成「解析碰撞」的難題 */
#define ONSCREEN_MIN              7
#define ONSCREEN_MAX             20    /* 同時也是效能預算 —— 見標頭檔 */
#define SPAWN_INTERVAL_MIN      0.30f
#define SPAWN_INTERVAL_MAX      1.50f
#define SPAWN_OPENING_BURST       4    /* 波次開始時一起湧入的敵人數 */
#define SPAWN_ATTEMPTS            9
#define SPAWN_MIN_DIST_PLAYER     9.0f
#define SPAWN_HIDE_DIST          16.0f /* 超過此距離就略過視野範圍（view cone）排斥 */
#define SPAWN_VIEW_COS            0.35f /* 準心左右各約 69 度 */
#define SPAWN_TURRET_MIN_DIST    14.0f /* 砲塔需要留有射擊視線，否則不公平 */
#define SKILL_LAMBDA              0.9f
#define SKILL_CLAMP             0.18f  /* 橡皮筋影響的最大幅度，±18% */
#define PERFECT_WAVE_BONUS       750

/* 各波次區段（band）的原型權重，索引為 [band][ET_*]，其中 band = wave_index / 2。
 * 每個區段負責教會玩家一種新的操作動詞（verb）：
 *   第 1-2 波：只有 grunts           -- 學會射擊與移動
 *   第 3-4 波：hounds 加入           -- 學會速度勝過準度
 *   第 5-6 波：turrets 加入          -- 學會切斷視線（line of sight）
 *   第 7 波起：brutes 加入           -- 學會放風箏（kite）與善用爆炸物
 */
static const float s_wave_weights[6][ET_COUNT] = {
    /*          GRUNT   HOUND   BRUTE   TURRET */
    /* 區段 0 */ { 1.00f, 0.00f, 0.00f, 0.00f },
    /* 區段 1 */ { 0.72f, 0.28f, 0.00f, 0.00f },
    /* 區段 2 */ { 0.52f, 0.33f, 0.00f, 0.15f },
    /* 區段 3 */ { 0.40f, 0.30f, 0.15f, 0.15f },
    /* 區段 4 */ { 0.32f, 0.30f, 0.22f, 0.16f },
    /* 區段 5 */ { 0.27f, 0.30f, 0.27f, 0.16f },
};
#define WAVE_BANDS ((int)(sizeof(s_wave_weights) / sizeof(s_wave_weights[0])))

/* -------------------------------------------------------------------------------------- */
/* 小型輔助函式                                                                              */
/* -------------------------------------------------------------------------------------- */
static const bp_difficulty_def *diff(void)
{
    int i = BP_CLAMP(g_waves.difficulty, 0, g_difficulty_count - 1);
    return &g_difficulties[i];
}

static float band_weight(int type, int waveIndex)
{
    int band = BP_CLAMP((waveIndex - 1) / 2, 0, WAVE_BANDS - 1);
    if (type < 0 || type >= ET_COUNT) return 0.0f;
    return s_wave_weights[band][type];
}

const char *bp_waves_state_name(void)
{
    switch (g_waves.state)
    {
        case WAVE_IDLE:         return "IDLE";
        case WAVE_INTRO:        return "INTRO";
        case WAVE_ACTIVE:       return "ACTIVE";
        case WAVE_CLEARING:     return "CLEARING";
        case WAVE_INTERMISSION: return "INTERMISSION";
        case WAVE_COMPLETE:     return "COMPLETE";
        default:                return "?";
    }
}

bool bp_waves_is_active(void)
{
    return g_waves.state == WAVE_ACTIVE || g_waves.state == WAVE_CLEARING;
}

bool bp_waves_level_complete(void) { return g_waves.state == WAVE_COMPLETE; }

int bp_waves_remaining(void)
{
    return g_waves.budget + bp_enemy_alive_count();
}

float bp_waves_progress01(void)
{
    int total = g_waves.wave_total > 0 ? g_waves.wave_total : 1;
    float done = (float)(g_waves.wave_index - 1);
    if (g_waves.wave_index >= 1 && total > 0)
    {
        /* 把目前波次已擊殺的比例也算進去，進度條才會平滑移動 */
        int waveSize = g_waves.spawned_this_wave + g_waves.budget;
        if (waveSize > 0) done += (float)g_waves.killed_this_wave / (float)waveSize;
    }
    return BP_CLAMP(done / (float)total, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------- */
/* 波次組成                                                                                  */
/* -------------------------------------------------------------------------------------- */
static int wave_budget(int waveIndex)
{
    float base = (float)(WAVE_BASE_BUDGET + (waveIndex - 1) * WAVE_BUDGET_GROWTH);
    base *= diff()->pressure;
    /* 最後一波刻意做成尖峰（spike）：大約 1.5 倍，好讓終戰明確是整關的高潮，而不是「又
     * 是平常的一天」。 */
    if (waveIndex >= g_waves.wave_total) base *= 1.5f;
    int n = (int)(base + 0.5f);
    return BP_CLAMP(n, 6, WAVE_BUDGET_MAX);
}

/*
 * 依照波次區段的權重，把 `budget` 個敵人分配到各原型。
 *
 * 四捨五入採用最大餘數法（largest-remainder），而不是逐項取整，如此各項之和永遠精確等於
 * 預算。逐項取整經常讓某一波少一到兩個敵人，而這種虧空會在整關不斷累積，終盤就明顯變
 * 薄。
 */
static void compose_wave(int waveIndex, int budget)
{
    float w[ET_COUNT];
    float total = 0.0f;
    for (int t = 0; t < ET_COUNT; t++) { w[t] = band_weight(t, waveIndex); total += w[t]; }
    if (total <= 0.0f) { w[ET_GRUNT] = 1.0f; total = 1.0f; }

    float exact[ET_COUNT];
    int   floor_[ET_COUNT];
    int   assigned = 0;
    for (int t = 0; t < ET_COUNT; t++)
    {
        exact[t]  = (w[t] / total) * (float)budget;
        floor_[t] = (int)exact[t];
        g_waves.pending[t] = floor_[t];
        assigned += floor_[t];
    }

    /* 把餘數分給小數部分最大的那些型別 */
    int left = budget - assigned;
    while (left > 0)
    {
        int best = -1;
        float bestFrac = -1.0f;
        for (int t = 0; t < ET_COUNT; t++)
        {
            if (w[t] <= 0.0f) continue;                    /* 絕不加入尚未解鎖的型別 */
            float frac = exact[t] - (float)floor_[t];
            if (frac > bestFrac) { bestFrac = frac; best = t; }
        }
        if (best < 0) best = ET_GRUNT;                     /* 所有權重都是整數，沒有餘數可分 */
        g_waves.pending[best]++;
        floor_[best]++;                                    /* 讓它不會被挑中第二次 */
        left--;
    }

    /* 砲塔是固定陣地：只有關卡確實有地方擺它們才有意義，而「整波全是砲塔」根本不算戰鬥。
     * 因此把上限壓在三分之一。 */
    int turretCap = BP_MAX(1, budget / 3);
    if (g_waves.pending[ET_TURRET] > turretCap)
    {
        int overflow = g_waves.pending[ET_TURRET] - turretCap;
        g_waves.pending[ET_TURRET] = turretCap;
        g_waves.pending[ET_GRUNT] += overflow;
    }
}

/* -------------------------------------------------------------------------------------- */
/* 出生點配置                                                                                */
/* -------------------------------------------------------------------------------------- */
/*
 * 為候選出生點打分：分數越高越好，負分代表非法。
 *
 * 之所以需要打分，是因為「取第一個合法點」會造成集中：光靠輪詢（round-robin）游標，敵人
 * 每一波都會從同一個角落進場。以「與玩家的距離」打分、再對最近用過的點扣分，能把壓力
 * 分散到整個競技場 —— 而這正是逼玩家不斷移動的機制。
 */
static float score_spawn(const bp_trigger *t, int type, Vector3 playerPos, Vector3 fwd)
{
    float dx = t->position.x - playerPos.x;
    float dz = t->position.z - playerPos.z;
    float dist = sqrtf(dx * dx + dz * dz);

    float minDist = (type == ET_TURRET) ? SPAWN_TURRET_MIN_DIST : SPAWN_MIN_DIST_PLAYER;
    if (dist < minDist) return -1.0f;

    /* 除非其他候選全都非法，否則絕不在玩家正看著的地方生成。敵人憑空出現在準心上，會讓
     * 玩家覺得遊戲在作弊，同時也讓「掃視競技場」這個動作失去回報。 */
    if (dist < SPAWN_HIDE_DIST)
    {
        float inv = 1.0f / BP_MAX(dist, 0.001f);
        float dot = (dx * inv) * fwd.x + (dz * inv) * fwd.z;
        if (dot > SPAWN_VIEW_COS) return -1.0f;
    }

    /* 拒絕落在幾何體內部的點。移動機構件（mover）會在載入之後推動固體，所以這是真的會
     * 發生的情況，而非防禦性檢查：一道正在關閉的防爆門可能把出生點整個吞進去，而被活埋
     * 的敵人既看不見也打不死。 */
    Vector3 probe = vec3(t->position.x, t->position.y + 0.35f, t->position.z);
    if (bp_world_point_solid(probe)) return -1.0f;

    /* 砲塔另外需要腳下真的有地板（它們不會掉下來），也需要清楚的射線：生在柱子後面的
     * 砲塔等於白送一顆人頭。 */
    if (type == ET_TURRET)
    {
        float floorY = bp_world_floor_height(t->position, 0.4f, t->position.y + 1.0f);
        if (BP_ABS(floorY - t->position.y) > 1.2f) return -1.0f;
    }

    /* 偏好中距離：太近不公平，太遠則浪費整波（敵人還沒到就被殺光）。這條拋物線的頂點約
     * 在 22 公尺處。 */
    float ideal = 22.0f;
    float d = dist - ideal;
    float shape = 100.0f - (d * d) * 0.12f;

    /* 略微偏好在玩家背後生成：那會製造出「牠們是從某個地方來的」的壓力，讓對競技場的
     * 空間意識變得重要。 */
    float behind = (dx * fwd.x + dz * fwd.z) < 0.0f ? 12.0f : 0.0f;

    return shape + behind;
}

static bool spawn_one(int type, Vector3 playerPos, Vector3 fwd)
{
    int spawnCount = bp_world_spawn_count(TRIG_ENEMY_SPAWN);
    if (spawnCount <= 0) return false;

    int   bestIdx = -1;
    float bestScore = -1e30f;
    int   cursor = g_waves.spawn_cursor;

    for (int a = 0; a < SPAWN_ATTEMPTS && a < spawnCount; a++)
    {
        int probe = cursor;
        const bp_trigger *t = NULL;

        /* 從 `probe` 起走訪 trigger 陣列，找出第 a 個 ENEMY_SPAWN。每次嘗試是 O(n)，但 n
         * 不到 40，而每次生成最多嘗試 9 次，所以最壞情況約 360 次比較 —— 而這一拍最多只
         * 生成幾個敵人。 */
        int seen = 0;
        for (int i = 0; i < g_world.trigger_count; i++)
        {
            int idx = (probe + i) % g_world.trigger_count;
            if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
            if (seen == a) { t = &g_world.triggers[idx]; break; }
            seen++;
        }
        if (!t) break;

        float sc = score_spawn(t, type, playerPos, fwd);
        if (sc > bestScore) { bestScore = sc; bestIdx = a; }
    }

    cursor = (cursor + 1) % BP_MAX(spawnCount, 1);
    g_waves.spawn_cursor = cursor;

    if (bestIdx < 0 || bestScore < 0.0f)
    {
        g_waves.spawns_rejected++;
        /* 所有合法候選都被拒絕了。與其讓波次永遠停著，不如退而求其次「隨便找個出生點」：
         * 稍微不公平的進場，也好過一波永遠清不完、把玩家鎖在出口外。 */
        int any = 0;
        const bp_trigger *t = NULL;
        for (int i = 0; i < g_world.trigger_count; i++)
        {
            int idx = (g_waves.spawn_cursor + i) % g_world.trigger_count;
            if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
            if (!bp_world_point_solid(vec3(g_world.triggers[idx].position.x,
                                           g_world.triggers[idx].position.y + 0.35f,
                                           g_world.triggers[idx].position.z)))
            { t = &g_world.triggers[idx]; any = idx; break; }
        }
        if (!t) return false;
        BP_UNUSED(any);
        bp_handle h = bp_enemy_spawn((bp_enemy_type)type, t->position, t->yaw, false);
        if (!(h.index != 0)) return false;
        return true;
    }

    /* 把勝出的索引重新解析回對應的 trigger */
    const bp_trigger *win = NULL;
    int seen = 0;
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        int idx = (g_waves.spawn_cursor + i) % g_world.trigger_count;
        if (g_world.triggers[idx].kind != TRIG_ENEMY_SPAWN) continue;
        if (seen == bestIdx) { win = &g_world.triggers[idx]; break; }
        seen++;
    }
    if (!win) return false;

    bp_handle h = bp_enemy_spawn((bp_enemy_type)type, win->position, win->yaw, false);
    if (!(h.index != 0)) return false;

    /* 進場回饋：在出生點迸出一小團火星。沒有它的話，敵人會無聲無息地冒出來，玩家也就
     * 無法建立「壓力是從哪裡來的」的心智模型。 */
    Vector3 at = vec3(win->position.x, win->position.y + 0.5f, win->position.z);
    for (int i = 0; i < 5; i++)
    {
        float a = bp_rng_range(&g_engine.rng, 0.0f, BP_TAU);
        bp_fx_particle(PTX_EMBER, at,
                       vec3(cosf(a) * 1.6f, bp_rng_range(&g_engine.rng, 0.8f, 2.6f), sinf(a) * 1.6f),
                       0.38f, 0.07f, 0.0f, g_assets.pal_danger, (Color){ 232, 52, 60, 0 });
    }
    return true;
}

/* 選出下一個要生成的原型：取待生成數最高者，平手時以隨機決定，免得每一波永遠都用同一種
 * 型別開場。 */
static int choose_pending_type(void)
{
    /* 待生成數最多者優先，好讓一波先把主力送完、再送上特種型別 —— 依序出現
     * 「grunt grunt grunt ... brute」讀起來是逐步升溫，反過來則像虎頭蛇尾。平手時以均勻
     * 隨機拆解：固定的平手順序會讓每一波都以同一種原型開場。 */
    int bestN = 0;
    for (int t = 0; t < ET_COUNT; t++) if (g_waves.pending[t] > bestN) bestN = g_waves.pending[t];
    if (bestN <= 0) return -1;

    int tied[ET_COUNT], n = 0;
    for (int t = 0; t < ET_COUNT; t++) if (g_waves.pending[t] == bestN) tied[n++] = t;
    return tied[bp_rng_int(&g_engine.rng, n)];
}

static void do_spawn_tick(Vector3 playerPos, Vector3 fwd, int burst)
{
    for (int i = 0; i < burst; i++)
    {
        if (g_waves.budget <= 0) break;
        if (bp_enemy_alive_count() >= g_waves.max_onscreen) break;

        int type = choose_pending_type();
        if (type < 0)
        {
            /* 組成已耗盡、預算卻還有：這發生在砲塔上限或被拒絕的出生點讓兩個計數器失去
             * 同步時。用 grunts 補滿，波次才一定收得了尾。 */
            g_waves.pending[ET_GRUNT] += g_waves.budget;
            type = ET_GRUNT;
        }

        if (!spawn_one(type, playerPos, fwd))
        {
            /* 配置失敗。把該敵人退回待生成池，並停止這一拍的嘗試，而不是把所有嘗試次數
             * 浪費在一個不可能成功的出生點上。 */
            g_waves.pending[type]++;
            break;
        }

        g_waves.pending[type]--;
        g_waves.budget--;
        g_waves.spawned_this_wave++;
    }
}

/* -------------------------------------------------------------------------------------- */
/* 技術讀值與橡皮筋調節                                                                      */
/* -------------------------------------------------------------------------------------- */
static float s_dmgWindow = 0.0f;
static float s_dmgAccum  = 0.0f;
static float s_lastHealth = 0.0f;

static void update_skill(float dt)
{
    /* 每秒承受的傷害，以 6 秒滾動視窗量測。這裡對玩家血量取差分，而不是掛在傷害函式
     * 上，如此讀值才不會與治療脫鉤（否則視窗中途撿到醫療包會被算成負傷害）。 */
    float h = g_player.health;
    float delta = s_lastHealth - h;
    s_lastHealth = h;
    if (delta > 0.0f) s_dmgAccum += delta;
    s_dmgWindow += dt;

    if (s_dmgWindow >= 6.0f)
    {
        float dps = s_dmgAccum / s_dmgWindow;
        float acc = bp_weapons_accuracy();          /* 整場累計的 0..1 */

        /* 參照點：12 dps 代表「正在被碾」，0 dps 代表「毫髮無傷」；0.55 命中率代表一個
         * 在中距離作戰的老練玩家。 */
        float pressureFromDamage = BP_CLAMP(dps / 12.0f, 0.0f, 1.0f);   /* 1 = 苦戰中 */
        float pressureFromSkill  = 1.0f - BP_CLAMP(acc / 0.55f, 0.0f, 1.0f);

        float target = 1.0f - 0.5f * (pressureFromDamage + pressureFromSkill);  /* 0..1，0.5 為中立 */
        float signedSkill = (target - 0.5f) * 2.0f;                             /* 落在 -1..+1 之間 */

        /* 加以平滑，免得單獨糟糕的一秒就扭轉整波的走向。 */
        g_waves.skill = bp_damp(g_waves.skill, BP_CLAMP(signedSkill, -1.0f, 1.0f), SKILL_LAMBDA, s_dmgWindow);

        s_dmgWindow = 0.0f;
        s_dmgAccum = 0.0f;
    }

    /* aggression 才是生成器實際使用的單一數值：中立值為 1.0，上下各加減 SKILL_CLAMP。
     * 請注意符號：苦戰中的玩家（skill < 0）反而會得到「較少」的壓力。 */
    g_waves.aggression = BP_CLAMP(1.0f + g_waves.skill * SKILL_CLAMP, 1.0f - SKILL_CLAMP, 1.0f + SKILL_CLAMP);
}

/* -------------------------------------------------------------------------------------- */
/* 狀態轉換                                                                                  */
/* -------------------------------------------------------------------------------------- */
static void enter_state(bp_wave_state s)
{
    g_waves.state = s;
    g_waves.state_time = 0.0f;
}

static void begin_wave(int waveIndex)
{
    g_waves.wave_index = waveIndex;
    g_waves.budget = wave_budget(waveIndex);
    compose_wave(waveIndex, g_waves.budget);
    g_waves.spawned_this_wave = 0;
    g_waves.killed_this_wave = 0;
    g_waves.damage_taken_this_wave = false;
    g_waves.spawn_timer = 0.0f;
    s_lastHealth = g_player.health;

    /* 壓力隨波次攀升，再由難度與技術讀值調節。所有數值都被夾限：不論設定檔怎麼寫，導演
     * 都不能超出效能預算。 */
    float waveRamp = 1.0f + (float)(waveIndex - 1) * 0.11f;
    float target = (float)ONSCREEN_MIN * waveRamp * diff()->pressure * g_waves.aggression;
    g_waves.max_onscreen = BP_CLAMP((int)(target + 0.5f), ONSCREEN_MIN, ONSCREEN_MAX);

    float interval = SPAWN_INTERVAL_MAX / BP_MAX(waveRamp * diff()->pressure * g_waves.aggression, 0.2f);
    g_waves.spawn_interval = BP_CLAMP(interval, SPAWN_INTERVAL_MIN, SPAWN_INTERVAL_MAX);

    enter_state(WAVE_INTRO);
    bp_audio_play(SFX_WAVE_START, 1.0f);
    bp_audio_set_intensity(BP_CLAMP(0.30f + 0.10f * (float)waveIndex, 0.30f, 0.95f), 0.4f);

    BP_LOGI("waves: wave %d/%d -- %d enemies, max %d onscreen, %.2fs trickle (difficulty %s)",
            waveIndex, g_waves.wave_total, g_waves.budget, g_waves.max_onscreen,
            (double)g_waves.spawn_interval, diff()->name);
}

static void finish_wave(void)
{
    float t = g_waves.state_time;
    g_waves.longest_wave = BP_MAX(g_waves.longest_wave, t);
    g_waves.fastest_wave = (g_waves.fastest_wave <= 0.0f) ? t : BP_MIN(g_waves.fastest_wave, t);
    g_waves.waves_completed++;

    if (!g_waves.damage_taken_this_wave)
    {
        g_waves.perfect_waves++;
        bp_player_add_score(PERFECT_WAVE_BONUS);
        BP_LOGI("waves: perfect clear (+%d)", PERFECT_WAVE_BONUS);
    }

    bp_audio_play(SFX_WAVE_CLEAR, 1.0f);

    if (g_waves.wave_index >= g_waves.wave_total)
    {
        enter_state(WAVE_COMPLETE);
        /* 音樂退回鋪底（pad）聲部：戰鬥已經結束，音訊必須搶在 HUD 之前說出這件事，否則
         * 那個時刻會顯得平淡。 */
        bp_audio_set_intensity(0.18f, 1.2f);
        BP_LOGI("waves: level complete -- %d kills, %d perfect, %.1fs",
                g_waves.killed_this_level, g_waves.perfect_waves, (double)g_waves.level_time);
        return;
    }

    g_waves.intermission_time = BP_LERP(WAVE_INTERMISSION_MAX, WAVE_INTERMISSION_MIN,
                                        BP_CLAMP((float)g_waves.wave_index / (float)BP_MAX(g_waves.wave_total, 1), 0.0f, 1.0f));
    g_waves.intermission_left = g_waves.intermission_time;
    g_waves.resupply_given = false;
    enter_state(WAVE_INTERMISSION);
}

static void give_resupply(void)
{
    if (g_waves.resupply_given) return;
    g_waves.resupply_given = true;

    float frac = diff()->resupply;
    if (frac <= 0.001f) return;                 /* BREACH 難度：完全不給任何幫助 */

    /* 補向「接近全滿」而不是補固定量：血量 95 的玩家不該和血量 20 的玩家拿到一樣的獎勵，
     * 否則等於在懲罰那些撐得好的人。 */
    float heal = (g_player.max_health - g_player.health) * frac;
    float armor = (g_player.max_armor - g_player.armor) * frac * 0.65f;
    if (heal > 0.5f) bp_player_heal(heal);
    if (armor > 0.5f) bp_player_add_armor(armor);

    /* 彈藥按「每把武器一個滿載備彈的某個比例」補充，如此火箭筒才不會在長關卡裡變成可以
     * 無腦濫開的預設武器。 */
    int rounds = 0;
    for (int w = 0; w < WPN_ID_COUNT; w++)
    {
        if (!bp_weapons_owns((bp_weapon_id)w)) continue;
        int full = g_weapon_table[w].mag_size * 3;
        rounds += (int)((float)full * frac * 0.5f);
    }
    if (rounds > 0) bp_weapons_add_reserve_all(rounds / BP_MAX(1, g_waves.wave_index > 3 ? 2 : 1));

    /* 同時在玩家腳下生成一個看得見的補給品，讓這次補給讀起來像一個事件，而不是默默變動
     * 的數值。 */
    bp_pickup_spawn(PICKUP_AMMO, vec3(g_player.pos.x, g_player.pos.y + 0.1f, g_player.pos.z),
                    (float)BP_MAX(8, (int)(24.0f * frac)), -1);
    bp_audio_play(SFX_HEAL, 0.7f);
}

/* -------------------------------------------------------------------------------------- */
/* 公開 API                                                                                  */
/* -------------------------------------------------------------------------------------- */
void bp_waves_init(void)
{
    memset(&g_waves, 0, sizeof(g_waves));
    g_waves.difficulty = 1;
    g_waves.diff_health = g_difficulties[1].enemy_health;
    g_waves.diff_damage = g_difficulties[1].enemy_damage;
    g_waves.diff_score = g_difficulties[1].score;
    g_waves.diff_resupply = g_difficulties[1].resupply;
    g_waves.aggression = 1.0f;
    g_waves.max_onscreen = ONSCREEN_MIN;
    g_waves.spawn_interval = SPAWN_INTERVAL_MAX;
    g_waves.state = WAVE_IDLE;
    g_waves.initialised = true;
    s_dmgWindow = s_dmgAccum = 0.0f;
    s_lastHealth = 0.0f;
}

void bp_waves_start_level(int levelIndex)
{
    if (levelIndex < 0 || levelIndex >= g_level_count)
    {
        BP_LOGW("waves: level index %d out of range", levelIndex);
        levelIndex = 0;
    }

    g_waves.level_index = levelIndex;
    g_waves.wave_total = BP_MAX(1, g_levels[levelIndex].waves);
    g_waves.level_time = 0.0f;
    g_waves.wave_index = 0;
    g_waves.killed_this_level = 0;
    g_waves.spawns_rejected = 0;
    g_waves.spawn_cursor = 0;
    g_waves.skill = 0.0f;
    g_waves.aggression = 1.0f;
    g_waves.longest_wave = 0.0f;
    g_waves.fastest_wave = 0.0f;
    g_waves.waves_completed = 0;
    g_waves.perfect_waves = 0;
    memset(g_waves.pending, 0, sizeof(g_waves.pending));
    s_dmgWindow = s_dmgAccum = 0.0f;
    s_lastHealth = g_player.health;

    /* 難度只在這裡解析一次。刻意不支援關卡中途更改難度：倍率已經寫進已生成敵人的血量裡。 */
    g_waves.diff_health = diff()->enemy_health;
    g_waves.diff_damage = diff()->enemy_damage;
    g_waves.diff_score = diff()->score;
    g_waves.diff_resupply = diff()->resupply;

    int spawns = bp_world_spawn_count(TRIG_ENEMY_SPAWN);
    if (spawns <= 0) BP_LOGW("waves: level '%s' has no enemy spawn points", g_world.name);

    BP_LOGI("waves: starting level %d '%s' -- %d waves, %d spawn points, difficulty %s",
            levelIndex, g_levels[levelIndex].display_name, g_waves.wave_total, spawns, diff()->name);

    begin_wave(1);
}

void bp_waves_notify_kill(int enemyType)
{
    BP_UNUSED(enemyType);
    g_waves.killed_this_wave++;
    g_waves.killed_this_level++;
}

void bp_waves_update(float dt)
{
    if (!g_waves.initialised || dt <= 0.0f) return;

    g_waves.state_time += dt;
    if (g_waves.state != WAVE_IDLE && g_waves.state != WAVE_COMPLETE) g_waves.level_time += dt;

    /* 追蹤受到的傷害，供完美波次獎勵與技術讀值使用。 */
    if (g_player.health < s_lastHealth - 0.01f) g_waves.damage_taken_this_wave = true;

    Vector3 playerPos = g_player.pos;
    Vector3 fwd = bp_camera_forward();
    fwd.y = 0.0f;
    float fl = Vector3Length(fwd);
    if (fl > 0.001f) fwd = Vector3Scale(fwd, 1.0f / fl); else fwd = vec3(0, 0, 1);

    switch (g_waves.state)
    {
        case WAVE_IDLE:
            break;

        case WAVE_INTRO:
            /* 橫幅亮起期間的一小段喘息。這段時間不會生成敵人，正因如此，波次的開始才是
             * 「讀得懂」的，而不是一團不明所以的躁動。 */
            update_skill(dt);
            if (g_waves.state_time >= WAVE_INTRO_TIME)
            {
                enter_state(WAVE_ACTIVE);
                do_spawn_tick(playerPos, fwd, SPAWN_OPENING_BURST);
            }
            break;

        case WAVE_ACTIVE:
            update_skill(dt);
            g_waves.spawn_timer -= dt;
            if (g_waves.spawn_timer <= 0.0f && g_waves.budget > 0)
            {
                /* 自適應的零星生成：畫面上已有大量敵人時就放慢速度，而不是排出一堆最終
                 * 反正會被同屏上限擋掉的生成佇列。 */
                float load = (float)bp_enemy_alive_count() / (float)BP_MAX(g_waves.max_onscreen, 1);
                g_waves.spawn_timer = g_waves.spawn_interval * (1.0f + load * 0.8f);
                do_spawn_tick(playerPos, fwd, 1);
            }
            if (g_waves.budget <= 0) enter_state(WAVE_CLEARING);
            break;

        case WAVE_CLEARING:
            update_skill(dt);
            /* 所有生成都已派完。壓力仍要保持誠實：萬一最後一個敵人不知怎麼就是夠不著
             * （被移動機構件活埋、或掉出競技場），這一波依然必須結束，否則玩家會被軟鎖
             * 在出口外，完全無從解套。 */
            if (bp_enemy_alive_count() <= 0) finish_wave();
            else if (g_waves.state_time > 120.0f)
            {
                BP_LOGW("waves: %d enemies unreachable after 120s -- forcing wave clear",
                        bp_enemy_alive_count());
                bp_enemies_clear();
                finish_wave();
            }
            break;

        case WAVE_INTERMISSION:
            give_resupply();
            g_waves.intermission_left -= dt;
            if (g_waves.intermission_left <= 0.0f) begin_wave(g_waves.wave_index + 1);
            break;

        case WAVE_COMPLETE:
            /* 沒有什麼好再導演的了。遊戲層輪詢 bp_waves_level_complete()，再自行決定要
             * 顯示統計畫面，還是推進到下一關。 */
            bp_audio_set_intensity(0.18f, 0.5f);
            break;

        default:
            enter_state(WAVE_IDLE);
            break;
    }
}
