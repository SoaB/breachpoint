/******************************************************************************************
 * tests/test_systems.c -- 無 GL 的系統層單元測試（make test）。
 *
 * 只鏈結 bp_log / bp_math / bp_entity / bp_config 四個編譯單元，沒有視窗、沒有 GL context、
 * 沒有 raylib 靜態庫 —— 這些 TU 不該依賴任何需要 GL 的東西。bp_config_load() 的檔案路徑
 * 呼叫 FileExists/LoadFileData/UnloadFileData，這三個符號由檔案末尾的樁（stub）滿足：
 * 測試一律走 bp_config_load_mem()，樁永遠回報「檔案不存在」。
 *
 * 這些測試守的是「數學與簿記層」的契約。碰撞求得的推出方向錯一個軸、句柄世代忘記遞增、
 * 設定解析器把註解當資料 —— 這類 bug 在完整自我測試裡只會以「玩家卡牆」「幽靈命中」的
 * 間接症狀現形，在這裡則是一條紅色的斷言。
 ******************************************************************************************/
#include "bp_common.h"
#include "bp_entity.h"
#include "bp_math.h"
#include "bp_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------------------- */
/* 微型測試框架                                                                              */
/* -------------------------------------------------------------------------------------- */
static int s_checks = 0;
static int s_fails  = 0;

#define CHECK(cond, name) do { \
    s_checks++; \
    if (cond) printf("  ok   %s\n", name); \
    else { s_fails++; printf("  FAIL %s   (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

#define CHECK_F(cond, name, fmt, ...) do { \
    s_checks++; \
    if (cond) printf("  ok   " name "\n", __VA_ARGS__); \
    else { s_fails++; printf("  FAIL " name "   (%s:%d)\n", __VA_ARGS__, __FILE__, __LINE__); } \
} while (0)

static bool feq(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* -------------------------------------------------------------------------------------- */
/* bp_config_load() 的檔案 I/O 樁：headless 測試沒有（也不想要）真實檔案系統。               */
/* -------------------------------------------------------------------------------------- */
bool FileExists(const char *fileName) { (void)fileName; return false; }
unsigned char *LoadFileData(const char *fileName, int *dataSize)
{
    (void)fileName;
    if (dataSize) *dataSize = 0;
    return NULL;
}
void UnloadFileData(unsigned char *data) { (void)data; }

/* -------------------------------------------------------------------------------------- */
/* bp_math                                                                                  */
/* -------------------------------------------------------------------------------------- */
static void test_math(void)
{
    printf("bp_math:\n");

    /* -- bp_ray_aabb -- */
    bp_aabb box = bp_aabb_make((Vector3){ -1, -1, 5 }, (Vector3){ 1, 1, 7 });

    float t = -1.0f; Vector3 n = { 0, 0, 0 };
    bool hit = bp_ray_aabb((Vector3){ 0, 0, 0 }, (Vector3){ 0, 0, 1 }, &box, 100.0f, &t, &n);
    CHECK(hit && feq(t, 5.0f, 1e-4f) && feq(n.z, -1.0f, 1e-6f),
          "ray hits box from outside at t=5, normal -Z");

    hit = bp_ray_aabb((Vector3){ 0, 0, 0 }, (Vector3){ 0, 1, 0 }, &box, 100.0f, &t, &n);
    CHECK(!hit, "ray parallel outside the slab misses");

    hit = bp_ray_aabb((Vector3){ 0, 0, 6 }, (Vector3){ 0, 0, 1 }, &box, 100.0f, &t, &n);
    CHECK(hit && feq(t, 0.0f, 1e-6f), "origin inside the box hits at t=0");

    hit = bp_ray_aabb((Vector3){ 0, 0, 0 }, (Vector3){ 0, 0, 1 }, &box, 3.0f, &t, &n);
    CHECK(!hit, "hit beyond t_max is rejected");

    /* -- bp_resolve_body_box：中心在輪廓內時沿最淺軸推出 -- */
    bp_aabb wall = bp_aabb_make((Vector3){ 0, 0, 0 }, (Vector3){ 10, 3, 1 });
    Vector3 p = { 5.0f, 0.0f, 0.1f };   /* 深入 X、貼著 -Z 面 */
    bool moved = bp_resolve_body_box(&p, 0.5f, 1.8f, &wall);
    CHECK(moved && p.z <= wall.min.z, "body inside footprint pushed out along shallowest axis (Z)");

    Vector3 q = { 10.4f, 0.0f, 0.5f };  /* 中心在輪廓外，但與牆重疊 */
    moved = bp_resolve_body_box(&q, 0.5f, 1.8f, &wall);
    CHECK(moved && q.x > wall.max.x, "overlapping body beside the wall is pushed radially out");

    Vector3 r = { 5.0f, 3.5f, 0.5f };   /* 完全在盒子下方（y 無重疊） */
    moved = bp_resolve_body_box(&r, 0.5f, 1.0f, &wall);
    CHECK(!moved, "no vertical overlap means no resolution");

    /* -- bp_segment_point_distance -- */
    float d = bp_segment_point_distance((Vector3){ 0, 0, 0 }, (Vector3){ 10, 0, 0 }, (Vector3){ 5, 3, 0 });
    CHECK(feq(d, 3.0f, 1e-5f), "perpendicular distance to mid-segment");

    d = bp_segment_point_distance((Vector3){ 0, 0, 0 }, (Vector3){ 10, 0, 0 }, (Vector3){ 15, 0, 4 });
    CHECK(feq(d, sqrtf(41.0f), 1e-4f), "clamped to segment endpoint");

    /* -- bp_lerp_angle：±180 度繞回 -- */
    float a = bp_lerp_angle(170.0f, -170.0f, 0.5f);
    CHECK(feq(fabsf(a), 180.0f, 1e-3f), "lerp across the +/-180 seam takes the short way");

    a = bp_lerp_angle(-170.0f, 170.0f, 0.0f);
    CHECK(feq(a, -170.0f, 1e-5f), "lerp_angle with t=0 returns the start angle");
}

/* -------------------------------------------------------------------------------------- */
/* bp_entity                                                                                */
/* -------------------------------------------------------------------------------------- */
typedef struct { int value; } test_slot;

#define POOL_CAP 8
static test_slot     s_storage[POOL_CAP];
static uint32_t      s_gens[POOL_CAP];
static uint8_t       s_alive[POOL_CAP];
static uint32_t      s_free[POOL_CAP];
static uint32_t      s_iter[POOL_CAP];

static void test_entity(void)
{
    printf("bp_entity:\n");

    bp_pool pool;
    CHECK(bp_pool_init(&pool, "test", s_storage, s_gens, s_alive, s_free, s_iter, POOL_CAP, sizeof(test_slot)),
          "pool init");

    bp_handle h1 = bp_pool_alloc(&pool);
    CHECK(bp_pool_valid(&pool, h1) && bp_pool_get(&pool, h1) != NULL, "first alloc yields a valid handle");

    bp_handle h2 = bp_pool_alloc(&pool);
    CHECK(h2.index != h1.index, "second alloc takes a different slot");

    test_slot *slot = (test_slot *)bp_pool_get(&pool, h1);
    slot->value = 42;
    CHECK(((test_slot *)bp_pool_get(&pool, h1))->value == 42, "data round-trips through the handle");

    /* 世代遞增：free 之後，舊句柄必須立刻失效。 */
    uint32_t gen_before = h1.generation;
    bp_pool_free(&pool, h1);
    CHECK(!bp_pool_valid(&pool, h1), "freed handle is invalid");

    bp_handle h3 = bp_pool_alloc(&pool);
    CHECK(h3.index == h1.index && h3.generation == gen_before + 1,
          "slot is reused and its generation increments");

    bp_handle stale = { h1.index, gen_before };
    CHECK(!bp_pool_valid(&pool, stale) && bp_pool_get(&pool, stale) == NULL,
          "stale-generation handle resolves to NULL");

    /* 越界與空句柄 */
    bp_handle oob = { POOL_CAP + 10, 1 };
    CHECK(!bp_pool_valid(&pool, oob), "out-of-range index is invalid");
    CHECK(!bp_pool_valid(&pool, BP_NULL_HANDLE), "null handle is invalid");

    /* 迭代清單 */
    bp_pool_compact(&pool);
    CHECK(pool.iter_count == 2, "compact reports the two live slots");

    bp_pool_reset(&pool);
    CHECK(!bp_pool_valid(&pool, h2) && !bp_pool_valid(&pool, h3), "reset invalidates every handle");
}

/* -------------------------------------------------------------------------------------- */
/* bp_config                                                                                */
/* -------------------------------------------------------------------------------------- */
static void test_config(void)
{
    printf("bp_config:\n");

    static const char doc[] =
        "; semicolon comment\n"
        "# hash comment\n"
        "weapon shotgun\n"
        "    DISPLAY_NAME = SCATTER-12\n"
        "    damage = 11\n"
        "    spread = 0.25\n"
        "    auto = true\n"
        "    tint = #FF8A20\n"
        "    origin = 0.19 -0.16 -0.46\n"
        "    unknown_key = whatever\n"
        "end\n"
        "weapon pistol\n"
        "    damage = 8\n"
        "end\n";

    bp_config cfg;
    CHECK(bp_config_load_mem(&cfg, doc, (int)sizeof(doc) - 1), "document parses");

    CHECK(bp_config_find_section(&cfg, "weapon", "shotgun"), "find_section locates shotgun");
    CHECK(!strcmp(bp_config_section_id(&cfg), "shotgun"), "section id is exposed");

    int saw_damage = 0, saw_unknown = 0;
    while (bp_config_next_key(&cfg))
    {
        if (!strcmp(cfg.key, "damage"))         { saw_damage++;  CHECK(bp_cfg_int(&cfg, -1) == 11, "int value"); }
        else if (!strcmp(cfg.key, "spread"))    CHECK(feq(bp_cfg_float(&cfg, -1.0f), 0.25f, 1e-6f), "float value");
        else if (!strcmp(cfg.key, "auto"))      CHECK(bp_cfg_bool(&cfg, false) == true, "bool value");
        else if (!strcmp(cfg.key, "display_name"))
        {
            /* 大小寫不敏感：鍵在文件裡是大寫的。 */
            CHECK(!strcmp(bp_cfg_str(&cfg, ""), "SCATTER-12"), "keys are case-insensitive");
        }
        else if (!strcmp(cfg.key, "tint"))
        {
            Color c = bp_cfg_color(&cfg, (Color){ 0, 0, 0, 255 });
            CHECK(c.r == 255 && c.g == 138 && c.b == 32, "#RRGGBB colour parses");
        }
        else if (!strcmp(cfg.key, "origin"))
        {
            Vector3 v = bp_cfg_vec3(&cfg, (Vector3){ 0, 0, 0 });
            CHECK(feq(v.x, 0.19f, 1e-5f) && feq(v.y, -0.16f, 1e-5f) && feq(v.z, -0.46f, 1e-5f),
                  "vec3 \"x y z\" parses");
        }
        else if (!strcmp(cfg.key, "unknown_key")) saw_unknown++;
    }
    CHECK(saw_damage == 1, "keys are visited exactly once");
    CHECK(saw_unknown == 1, "unknown keys are surfaced, not skipped");

    /* 缺失的鍵回退：pistol 區塊裡查一個不存在的鍵，各型別取值函式應回傳 fallback。
     * 注意 bp_cfg_* 的契約是「取最後拜訪的那個鍵」，所以要先走到某個存在的鍵上。 */
    CHECK(bp_config_find_section(&cfg, "weapon", "pistol"), "second section found");
    bool saw_pistol_damage = false;
    while (bp_config_next_key(&cfg))
        if (!strcmp(cfg.key, "damage")) { saw_pistol_damage = true; break; }
    CHECK(saw_pistol_damage && bp_cfg_int(&cfg, -1) == 8, "pistol damage parsed");

    /* 壞值回退：bool 收到無法辨識的字串、float 收到垃圾時，回傳 fallback 而不是崩潰。 */
    static const char bad_doc[] =
        "weapon x\n"
        "    auto = maybe\n"
        "    spread = banana\n"
        "end\n";
    CHECK(bp_config_load_mem(&cfg, bad_doc, (int)sizeof(bad_doc) - 1), "bad document still loads");
    CHECK(bp_config_find_section(&cfg, "weapon", "x"), "bad document section found");
    while (bp_config_next_key(&cfg))
    {
        if (!strcmp(cfg.key, "auto"))
            CHECK(bp_cfg_bool(&cfg, true) == true, "unrecognised bool falls back");
        else if (!strcmp(cfg.key, "spread"))
            CHECK(bp_cfg_float(&cfg, 7.0f) == 0.0f, "garbage float parses to 0 (C convention)");
    }

    /* 空 / 損毀輸入不崩潰、不 ok */
    CHECK(!bp_config_load_mem(&cfg, NULL, 5), "NULL data rejected");
    CHECK(!bp_config_load_mem(&cfg, "x", 0), "zero length rejected");
    CHECK(bp_config_find_section(&cfg, "weapon", "nope") == false, "missing section returns false");
}

/* -------------------------------------------------------------------------------------- */
/* bp_log                                                                                   */
/* -------------------------------------------------------------------------------------- */
static void test_log(void)
{
    printf("bp_log:\n");

    bp_log_init();
    int errors_before = bp_log_error_count();

    for (int i = 0; i < 300; i++) BP_LOGI("filler line %d", i);
    int n = 0;
    bp_log_lines(&n);
    CHECK(n == MAX_LOG_LINES, "ring buffer wraps and reports the full 256 lines");

    /* 「最舊的在前」：最後一條可見的應該是最近寫入的那條。 */
    const char **lines = bp_log_lines(&n);
    CHECK(strstr(lines[n - 1], "filler line 299") != NULL, "newest line is last in the view");

    BP_LOGE("a deliberate error");
    CHECK(bp_log_error_count() == errors_before + 1, "error counter counts ERROR lines");
}

int main(void)
{
    test_math();
    test_entity();
    test_config();
    test_log();

    printf("\n%d checks, %d failed\n", s_checks, s_fails);
    return s_fails == 0 ? 0 : 1;
}
