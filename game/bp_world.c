/******************************************************************************************
 * bp_world.c -- 關卡世界：材質表、幾何烘焙、碰撞解算、移動體、觸發器與天穹。
 ******************************************************************************************/
#include "bp_world.h"
#include "../engine/bp_gfx.h"
#include "bp_fx.h"

bp_world g_world = { 0 };

/* ====================================================================================== */
/* 0. 調校常數                                                                               */
/* ====================================================================================== */
#define STEP_UP_HEIGHT      0.55f   /* 玩家無需跳躍即可直接走上去的台階高度 */
#define HEAD_CLEARANCE      0.05f
#define MAX_TRACE_DIST      400.0f
#define CULL_DISTANCE       170.0f  /* 距離超過此值的動態方塊不會被繪製 */

#define SKY_RINGS   18
#define SKY_SLICES  40
#define SKY_RADIUS  1000.0f

static Mesh  s_sky_mesh = { 0 };
static Model s_sky_model = { 0 };
static bool  s_sky_ready = false;

/* ====================================================================================== */
/* 1. 材質表：所有表面行為都是資料                                                           */
/* ====================================================================================== */
static const bp_material_def s_materials[MAT_COUNT] = {
    /* 名稱         紋理                命中音效       腳步音效     UV     摩擦   硬度   穿透  發光  彈痕  傷害 */
    { "concrete",  TEX_WALL_CONCRETE,  SFX_HIT_WALL,  SFX_FOOTSTEP, 2.60f, 1.00f, 0.55f, false,false,true, 0.0f, {255,255,255,255} },
    { "brick",     TEX_WALL_BRICK,     SFX_HIT_WALL,  SFX_FOOTSTEP, 2.20f, 1.00f, 0.45f, false,false,true, 0.0f, {255,255,255,255} },
    { "metal",     TEX_WALL_METAL,     SFX_HIT_ARMOR, SFX_FOOTSTEP, 2.40f, 0.92f, 0.85f, false,false,true, 0.0f, {255,255,255,255} },
    { "panel",     TEX_WALL_PANEL,     SFX_HIT_ARMOR, SFX_FOOTSTEP, 3.00f, 0.95f, 0.80f, false,false,true, 0.0f, {255,255,255,255} },
    { "rust",      TEX_WALL_RUSTY,     SFX_HIT_WALL,  SFX_FOOTSTEP, 2.40f, 1.00f, 0.40f, false,false,true, 0.0f, {255,255,255,255} },
    { "grate",     TEX_FLOOR_GRATE,    SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.80f, 0.88f, 0.90f, true, false,false,0.0f, {255,255,255,255} },
    { "tile",      TEX_FLOOR_TILE,     SFX_HIT_WALL,  SFX_FOOTSTEP, 3.20f, 1.00f, 0.50f, false,false,true, 0.0f, {255,255,255,255} },
    { "hazard",    TEX_FLOOR_HAZARD,   SFX_HIT_WALL,  SFX_FOOTSTEP, 2.00f, 1.00f, 0.50f, false,false,true, 9.0f, {255,255,255,255} },
    { "crate",     TEX_CRATE,          SFX_HIT_WALL,  SFX_FOOTSTEP, 1.00f, 1.05f, 0.30f, false,false,true, 0.0f, {255,255,255,255} },
    { "barrel",    TEX_BARREL,         SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.00f, 1.00f, 0.70f, false,false,false,0.0f, {255,255,255,255} },
    { "trim",      TEX_TRIM,           SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.60f, 1.00f, 0.85f, false,false,true, 0.0f, {255,255,255,255} },
    { "pillar",    TEX_PILLAR,         SFX_HIT_WALL,  SFX_FOOTSTEP, 2.00f, 1.00f, 0.60f, false,false,true, 0.0f, {255,255,255,255} },
    { "light",     TEX_LIGHT_PANEL,    SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.50f, 1.00f, 0.90f, false,true, false,0.0f, {255,255,255,255} },
    { "screen",    TEX_SCREEN,         SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.50f, 1.00f, 0.90f, false,true, false,0.0f, {255,255,255,255} },
    { "vent",      TEX_VENT,           SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.40f, 1.00f, 0.85f, false,false,true, 0.0f, {255,255,255,255} },
    { "pipe",      TEX_PIPE,           SFX_HIT_ARMOR, SFX_FOOTSTEP, 1.20f, 1.00f, 0.85f, false,false,true, 0.0f, {255,255,255,255} },
    { "field",     TEX_RING,           SFX_HIT_ARMOR, SFX_FOOTSTEP, 2.00f, 1.00f, 1.00f, true, true, false,0.0f, {120,220,255,255} },
};

const bp_material_def *bp_world_material(int index)
{
    if (index < 0 || index >= MAT_COUNT) return &s_materials[MAT_CONCRETE];
    return &s_materials[index];
}

/* ====================================================================================== */
/* 2. 關卡表                                                                                 */
/* ====================================================================================== */
const bp_level_def g_levels[] = {
    { "foundry",  "FOUNDRY",   "Symmetrical arena. Learn the movement.", 6, 150.0f, 0 },
    { "slagpit",  "SLAG PIT",  "Three height bands. Own the catwalk.",   8, 210.0f, 1 },
    { "conduit",  "CONDUIT",   "Tight corridors. Shotguns win.",         9, 240.0f, 2 },
};
const int g_level_count = BP_ARRAY_COUNT(g_levels);

/* ====================================================================================== */
/* 3. 關卡編輯巨集與關卡資料                                                                 */
/* ====================================================================================== */
static void add_solid_impl(float x, float y, float z, float w, float h, float d, int mat)
{
    if (g_world.solid_count >= MAX_SOLIDS) { BP_LOGW("world: solid limit hit (%d)", MAX_SOLIDS); return; }
    bp_solid *s = &g_world.solids[g_world.solid_count++];
    s->box.min = (Vector3){ x, y, z };
    s->box.max = (Vector3){ x + w, y + h, z + d };
    s->material = mat;
    s->dynamic = false;
    s->shoot_through = s_materials[mat >= 0 && mat < MAT_COUNT ? mat : 0].penetrable;
    s->explode = false;
    s->health = 0.0f;
    s->destroyed = false;
}

static void add_solid_explode_impl(float x, float y, float z, float w, float h, float d,
                                   int mat, float health)
{
    add_solid_impl(x, y, z, w, h, d, mat);
    if (g_world.solid_count == 0) return;
    bp_solid *s = &g_world.solids[g_world.solid_count - 1];
    s->explode = true;
    s->health = health;
    s->dynamic = true;
    s->shoot_through = false;
}

static void add_trigger_impl(int kind, float x, float y, float z, float yaw,
                             int param, float respawn, float radius)
{
    if (g_world.trigger_count >= MAX_TRIGGERS) { BP_LOGW("world: trigger limit hit"); return; }
    bp_trigger *t = &g_world.triggers[g_world.trigger_count++];
    t->kind = kind;
    t->position = (Vector3){ x, y, z };
    t->yaw = yaw;
    t->param = param;
    t->respawn = respawn;
    t->timer = 0.0f;
    t->consumed = false;
    t->radius = radius;
}

static void add_mover_impl(int solidIndex, float fx, float fy, float fz,
                           float tx, float ty, float tz, float seconds, int sfx)
{
    if (g_world.mover_count >= MAX_MOVERS) { BP_LOGW("world: mover limit hit"); return; }
    if (solidIndex < 0 || solidIndex >= g_world.solid_count) { BP_LOGE("world: bad mover index %d", solidIndex); return; }
    bp_mover *m = &g_world.movers[g_world.mover_count++];
    m->solid = &g_world.solids[solidIndex];
    m->from = (Vector3){ fx, fy, fz };
    m->to = (Vector3){ tx, ty, tz };
    m->duration = BP_MAX(seconds, 0.05f);
    m->phase = 0.0f;
    m->open = false;
    m->triggered = false;
    m->cooldown = 0.0f;
    m->sfx = sfx;
}

#define ADD_SOLID(x,y,z,w,h,d,m)          add_solid_impl(x,y,z,w,h,d,m)
#define ADD_SOLID_EXPLODE(x,y,z,w,h,d,m,hp) add_solid_explode_impl(x,y,z,w,h,d,m,hp)
#define ADD_TRIGGER(k,x,y,z,yaw,p,r,rad)  add_trigger_impl(k,x,y,z,yaw,p,r,rad)
#define ADD_MOVER(i,fx,fy,fz,tx,ty,tz,s,sfx) add_mover_impl(i,fx,fy,fz,tx,ty,tz,s,sfx)

/* 關卡資料會以名稱引用 WPN_* 編號；在這裡（而非標頭檔）宣告，是為了讓 bp_levels.inc
 * 不必引入任何 gameplay 標頭檔——這個關卡檔的設計初衷是讓關卡設計師可以直接編輯。 */
enum { WPN_PISTOL = 0, WPN_SHOTGUN = 1, WPN_SMG = 2, WPN_RAIL = 3, WPN_ROCKET = 4 };

#include "bp_levels.inc"

/* ====================================================================================== */
/* 4. 靜態網格烘焙                                                                           */
/* ====================================================================================== */
/*
 * 每個方塊貢獻六個面。每個面儲存三個向量：法線，以及兩個用來計算 UV 的面內基底向量，
 * 而 UV 是以「世界單位」為基準。用世界單位計算 UV，正是全遊戲每個表面紋理密度
 * （texel density）一致的關鍵：20 公尺的牆和 1 公尺的木箱，同樣都是每 `uv_scale` 公尺
 * 重複一次紋理。少了這一點，大面積牆面的貼圖會被明顯拉伸，關卡幾何立刻給人業餘作品的觀感。
 */
typedef struct { Vector3 n, u, v; } bp_face_basis;

static const bp_face_basis s_faces[6] = {
    { { 0, 1, 0}, { 1, 0, 0}, { 0, 0, 1} },   /* +Y 頂面    */
    { { 0,-1, 0}, { 1, 0, 0}, { 0, 0,-1} },   /* -Y 底面    */
    { { 0, 0, 1}, { 1, 0, 0}, { 0, 1, 0} },   /* +Z 前面    */
    { { 0, 0,-1}, {-1, 0, 0}, { 0, 1, 0} },   /* -Z 背面    */
    { { 1, 0, 0}, { 0, 0,-1}, { 0, 1, 0} },   /* +X 右面    */
    { {-1, 0, 0}, { 0, 0, 1}, { 0, 1, 0} },   /* -X 左面    */
};

/* 每個面一個假的方向性光照係數。算圖端沒有任何光照 pass（raylib 的預設材質是平面
 * 著色），因此把一組廉價的 lambert 近似值烘進頂點顏色，就是幾何體獲得可讀立體感的
 * 手段。數值刻意讓頂面最亮、底面接近全黑（反正底面永遠看不到）。 */
static const float s_face_light[6] = { 1.00f, 0.30f, 0.78f, 0.62f, 0.86f, 0.55f };

#define VERTS_PER_BOX 24
#define TRIS_PER_BOX  12

static bool bake_static_meshes(void)
{
    /* 先按材質統計靜態方塊數量，決定每個批次緩衝區要配置多少頂點與索引 */
    int perMat[MAT_COUNT] = { 0 };
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->dynamic || s->destroyed) continue;
        if (s->material < 0 || s->material >= MAT_COUNT) continue;
        perMat[s->material]++;
    }

    g_world.batch_count = 0;

    for (int m = 0; m < MAT_COUNT; m++)
    {
        int n = perMat[m];
        if (n == 0) continue;

        int vCount = n * VERTS_PER_BOX;
        int iCount = n * TRIS_PER_BOX * 3;

        Vector3 *verts  = (Vector3 *)RL_MALLOC(sizeof(Vector3) * (size_t)vCount);
        Vector2 *uvs    = (Vector2 *)RL_MALLOC(sizeof(Vector2) * (size_t)vCount);
        Vector3 *norms  = (Vector3 *)RL_MALLOC(sizeof(Vector3) * (size_t)vCount);
        Color   *colors = (Color *)RL_MALLOC(sizeof(Color) * (size_t)vCount);
        unsigned short *idx = (unsigned short *)RL_MALLOC(sizeof(unsigned short) * (size_t)iCount);
        if (!verts || !uvs || !norms || !colors || !idx)
        {
            BP_LOGE("world: mesh bake allocation failed");
            RL_FREE(verts); RL_FREE(uvs); RL_FREE(norms); RL_FREE(colors); RL_FREE(idx);
            continue;
        }

        const bp_material_def *md = &s_materials[m];
        float uvScale = md->uv_scale > 0.01f ? md->uv_scale : 2.0f;

        int vi = 0, ii = 0;
        for (int b = 0; b < g_world.solid_count; b++)
        {
            const bp_solid *s = &g_world.solids[b];
            if (s->dynamic || s->destroyed || s->material != m) continue;

            Vector3 mn = s->box.min, mx = s->box.max;
            /* c 是 UV 原點：每個面的貼圖座標都從方塊中心量起，因此相鄰但尺寸不同的
             * 方塊仍能對齊同一張世界格線，紋理彼此無縫接續。 */
            Vector3 c = Vector3Scale(Vector3Add(mn, mx), 0.5f);

            /* 方塊的八個角落；面繞序表 faceVerts 以索引選取每個面的四個角 */
            Vector3 corner[8] = {
                { mn.x, mn.y, mn.z }, { mx.x, mn.y, mn.z },
                { mx.x, mn.y, mx.z }, { mn.x, mn.y, mx.z },
                { mn.x, mx.y, mn.z }, { mx.x, mx.y, mn.z },
                { mx.x, mx.y, mx.z }, { mn.x, mx.y, mx.z },
            };
            /* 每個面的角落繞序（從方塊外側看為逆時針） */
            static const int faceVerts[6][4] = {
                { 4, 5, 6, 7 },   /* +Y */
                { 0, 3, 2, 1 },   /* -Y */
                { 3, 7, 6, 2 },   /* +Z */
                { 1, 5, 4, 0 },   /* -Z */
                { 2, 6, 5, 1 },   /* +X */
                { 0, 4, 7, 3 },   /* -X */
            };

            for (int f = 0; f < 6; f++)
            {
                Vector3 fn = s_faces[f].n;
                /* s_faces 的 u/v 是單位向量，因此下方內積的結果直接就是以公尺計的
                 * 距離——正好是 uv_scale 所預期的單位。 */
                Vector3 u = s_faces[f].u;
                Vector3 v = s_faces[f].v;

                float light = s_face_light[f];
                /* 自發光材質無視假光照係數，直接以全亮輸出。 */
                if (md->emissive) light = 1.0f;

                Color vc = (Color){
                    (unsigned char)BP_CLAMP((int)((float)md->tint.r * light), 0, 255),
                    (unsigned char)BP_CLAMP((int)((float)md->tint.g * light), 0, 255),
                    (unsigned char)BP_CLAMP((int)((float)md->tint.b * light), 0, 255),
                    255
                };

                int base = vi;
                for (int k = 0; k < 4; k++)
                {
                    Vector3 p = corner[faceVerts[f][k]];
                    verts[vi] = p;
                    norms[vi] = fn;
                    colors[vi] = vc;

                    Vector3 rel = Vector3Subtract(p, c);
                    float tu = Vector3DotProduct(rel, u) / uvScale + 0.5f;
                    float tv = Vector3DotProduct(rel, v) / uvScale + 0.5f;
                    uvs[vi] = (Vector2){ tu, tv };
                    vi++;
                }
                idx[ii++] = (unsigned short)(base + 0);
                idx[ii++] = (unsigned short)(base + 1);
                idx[ii++] = (unsigned short)(base + 2);
                idx[ii++] = (unsigned short)(base + 0);
                idx[ii++] = (unsigned short)(base + 2);
                idx[ii++] = (unsigned short)(base + 3);
            }
        }

        Mesh mesh = { 0 };
        mesh.vertexCount = vi;
        mesh.triangleCount = ii / 3;
        mesh.vertices  = (float *)verts;
        mesh.texcoords = (float *)uvs;
        mesh.normals   = (float *)norms;
        mesh.colors    = (unsigned char *)colors;
        mesh.indices   = idx;
        mesh.vboId = NULL;

        UploadMesh(&mesh, false);

        bp_static_batch *batch = &g_world.batch[g_world.batch_count];
        batch->mesh = mesh;
        batch->model = LoadModelFromMesh(mesh);
        batch->material_index = m;
        batch->box_count = n;
        batch->tri_count = ii / 3;
        batch->valid = (batch->model.meshCount > 0);

        if (batch->valid && batch->model.materials)
        {
            Material mat = batch->model.materials[0];
            Texture2D t = bp_tex(md->texture);
            if (t.id) mat.maps[MATERIAL_MAP_DIFFUSE].texture = t;
            mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            mat.maps[MATERIAL_MAP_SPECULAR].color = (Color){ 6, 6, 6, 255 };
            batch->material = mat;
        }
        g_world.batch_count++;
    }

    int total = 0;
    for (int i = 0; i < g_world.batch_count; i++) total += g_world.batch[i].tri_count;
    BP_LOGI("world: baked %d static batches, %d triangles", g_world.batch_count, total);
    return g_world.batch_count > 0;
}

static void release_static_meshes(void)
{
    for (int i = 0; i < g_world.batch_count; i++)
    {
        if (!g_world.batch[i].valid) continue;
        UnloadModel(g_world.batch[i].model);   /* 連同已上傳至 GPU 的 mesh 一併釋放 */
        g_world.batch[i].valid = false;
    }
    g_world.batch_count = 0;
}

static void build_sky_dome(Color zenith, Color horizon, Color ground,
                           Vector3 glowDir, Color glowCol);

/* ====================================================================================== */
/* 6. 載入 / 卸載                                                                            */
/* ====================================================================================== */
void bp_world_init(void)
{
    memset(&g_world, 0, sizeof(g_world));
}

void bp_world_shutdown(void) { bp_world_unload(); }

void bp_world_unload(void)
{
    release_static_meshes();
    if (s_sky_ready) { UnloadModel(s_sky_model); s_sky_ready = false; }
    g_world.loaded = false;
}

bool bp_world_load(int levelIndex)
{
    if (levelIndex < 0 || levelIndex >= g_level_count)
    {
        BP_LOGE("world: level index %d out of range", levelIndex);
        return false;
    }

    double t0 = GetTime();
    bp_world_unload();
    memset(&g_world, 0, sizeof(g_world));
    g_world.level_index = levelIndex;
    snprintf(g_world.id, sizeof(g_world.id), "%s", g_levels[levelIndex].id);
    snprintf(g_world.name, sizeof(g_world.name), "%s", g_levels[levelIndex].display_name);
    snprintf(g_world.subtitle, sizeof(g_world.subtitle), "%s", g_levels[levelIndex].subtitle);

    s_level_builders[levelIndex]();

    /* 掃過所有固體，算出整張地圖的軸對齊邊界範圍（bounds） */
    g_world.bounds_min = (Vector3){  1e9f,  1e9f,  1e9f };
    g_world.bounds_max = (Vector3){ -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_aabb *b = &g_world.solids[i].box;
        g_world.bounds_min.x = BP_MIN(g_world.bounds_min.x, b->min.x);
        g_world.bounds_min.y = BP_MIN(g_world.bounds_min.y, b->min.y);
        g_world.bounds_min.z = BP_MIN(g_world.bounds_min.z, b->min.z);
        g_world.bounds_max.x = BP_MAX(g_world.bounds_max.x, b->max.x);
        g_world.bounds_max.y = BP_MAX(g_world.bounds_max.y, b->max.y);
        g_world.bounds_max.z = BP_MAX(g_world.bounds_max.z, b->max.z);
    }

    /* 玩家出生點：從觸發器中找出 TRIG_PLAYER_START，順便統計敵人生成點數量 */
    g_world.player_start = (Vector3){ 0, 0.1f, 0 };
    g_world.player_start_yaw = 0.0f;
    g_world.spawn_enemy_count = 0;
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        bp_trigger *t = &g_world.triggers[i];
        if (t->kind == TRIG_PLAYER_START) { g_world.player_start = t->position; g_world.player_start_yaw = t->yaw; }
        if (t->kind == TRIG_ENEMY_SPAWN) g_world.spawn_enemy_count++;
    }

    /* 每個關卡一組專屬的環境氛圍：環境光、霧色、霧密度與天穹配色 */
    switch (levelIndex)
    {
        case 0:
            g_world.ambient = (Color){ 150, 138, 122, 255 };
            g_world.fog_color = (Color){ 34, 28, 26, 255 };
            g_world.fog_density = 0.0035f;
            build_sky_dome((Color){ 16, 14, 26, 255 }, (Color){ 92, 54, 34, 255 },
                           (Color){ 22, 18, 16, 255 }, (Vector3){ -0.70f, 0.10f, 0.55f },
                           (Color){ 226, 128, 48, 255 });
            break;
        case 1:
            g_world.ambient = (Color){ 128, 140, 152, 255 };
            g_world.fog_color = (Color){ 22, 26, 32, 255 };
            g_world.fog_density = 0.0042f;
            build_sky_dome((Color){ 10, 12, 22, 255 }, (Color){ 58, 40, 46, 255 },
                           (Color){ 18, 16, 18, 255 }, (Vector3){ 0.60f, 0.06f, -0.70f },
                           (Color){ 210, 96, 60, 255 });
            break;
        default:
            g_world.ambient = (Color){ 140, 148, 138, 255 };
            g_world.fog_color = (Color){ 26, 28, 24, 255 };
            g_world.fog_density = 0.0055f;
            build_sky_dome((Color){ 8, 10, 14, 255 }, (Color){ 44, 52, 44, 255 },
                           (Color){ 16, 16, 14, 255 }, (Vector3){ 0.20f, 0.14f, 0.90f },
                           (Color){ 120, 190, 150, 255 });
            break;
    }

    if (!bake_static_meshes())
    {
        BP_LOGE("world: level '%s' produced no geometry", g_world.id);
        return false;
    }

    g_world.build_ms = (float)((GetTime() - t0) * 1000.0);
    g_world.loaded = true;

    BP_LOGI("world: '%s' -- %d solids, %d triggers, %d movers, %d enemy spawns, built in %.0f ms",
            g_world.name, g_world.solid_count, g_world.trigger_count, g_world.mover_count,
            g_world.spawn_enemy_count, (double)g_world.build_ms);
    return true;
}

Vector3 bp_world_player_start(float *out_yaw)
{
    if (out_yaw) *out_yaw = g_world.player_start_yaw;
    return g_world.player_start;
}

int bp_world_count_solids(void) { return g_world.solid_count; }
const bp_solid *bp_world_solid(int index)
{
    if (index < 0 || index >= g_world.solid_count) return NULL;
    return &g_world.solids[index];
}

/* ====================================================================================== */
/* 7. 碰撞                                                                                   */
/* ====================================================================================== */
bool bp_world_sweep(Vector3 from, Vector3 to, float radius,
                    float *out_t, Vector3 *out_normal, const bp_solid **out_solid)
{
    Vector3 dir = Vector3Subtract(to, from);
    float len = Vector3Length(dir);
    if (len < 1e-5f)
    {
        if (out_t) *out_t = 0.0f;
        if (out_normal) *out_normal = (Vector3){ 0, 1, 0 };
        if (out_solid) *out_solid = NULL;
        return false;
    }
    dir = Vector3Scale(dir, 1.0f / len);

    float best = len;
    bool hit = false;
    const bp_solid *bestSolid = NULL;
    Vector3 bestN = { 0, 1, 0 };

    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        if (s->shoot_through) continue;

        /* 提早剔除：先用外擴後的 AABB 粗略濾掉線段絕對碰不到的方塊。這道廉價測試正是
         * 在沒有 broadphase 的前提下，整條清單掃描還能負擔得起的關鍵。 */
        bp_aabb grown = bp_aabb_grow(&s->box, radius + 0.01f);
        float lo = BP_MIN(from.x, to.x), hi = BP_MAX(from.x, to.x);
        if (hi + radius < grown.min.x || lo - radius > grown.max.x) continue;
        lo = BP_MIN(from.y, to.y); hi = BP_MAX(from.y, to.y);
        if (hi + radius < grown.min.y || lo - radius > grown.max.y) continue;
        lo = BP_MIN(from.z, to.z); hi = BP_MAX(from.z, to.z);
        if (hi + radius < grown.min.z || lo - radius > grown.max.z) continue;

        float t; Vector3 n;
        if (bp_ray_aabb(from, dir, &grown, best, &t, &n))
        {
            best = t;
            hit = true;
            bestSolid = s;
            bestN = n;
        }
    }

    if (out_t) *out_t = hit ? best : len;
    if (out_normal) *out_normal = bestN;
    if (out_solid) *out_solid = bestSolid;
    return hit;
}

bool bp_world_point_solid(Vector3 p)
{
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed || s->shoot_through) continue;
        if (bp_aabb_contains_point(&s->box, p)) return true;
    }
    return false;
}

int bp_world_material_at_point(Vector3 p)
{
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        if (bp_aabb_contains_point(&s->box, p)) return s->material;
    }
    return MAT_CONCRETE;
}

bool bp_world_line_of_sight(Vector3 from, Vector3 to)
{
    float t; Vector3 n; const bp_solid *s;
    return !bp_world_sweep(from, to, 0.0f, &t, &n, &s);
}

float bp_world_floor_height(Vector3 pos, float radius, float max_y)
{
    float best = -1e9f;
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        /* 只檢查 XZ 水平重疊；高度上限由呼叫端傳入的 max_y 過濾 */
        if (pos.x + radius < s->box.min.x || pos.x - radius > s->box.max.x) continue;
        if (pos.z + radius < s->box.min.z || pos.z - radius > s->box.max.z) continue;
        if (s->box.max.y > max_y + 0.001f) continue;
        if (s->box.max.y > best) best = s->box.max.y;
    }
    return best;
}

/* 完整的角色對世界碰撞解算：水平分離、垂直落地，外加走上台階的輔助。順序是有意的——
 * 必須先解算水平，台階檢測看到的才會是玩家真正要佔據的位置，否則會在錯誤的立足點上判定。 */
bool bp_world_resolve_body(Vector3 *feet, float radius, float height,
                           Vector3 *velocity, bool *on_ground, int *out_material)
{
    bool grounded = false;
    int groundMat = MAT_CONCRETE;
    float stepTo = 0.0f;
    bool doStep = false;

    bp_aabb body = bp_aabb_make((Vector3){ feet->x - radius, feet->y, feet->z - radius },
                                (Vector3){ feet->x + radius, feet->y + height, feet->z + radius });

    /* ---- 水平推出 + 台階檢測 ---- */
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        if (!bp_aabb_overlaps(&body, &s->box)) continue;

        if (!s->shoot_through)
        {
            float cx = BP_CLAMP(feet->x, s->box.min.x, s->box.max.x);
            float cz = BP_CLAMP(feet->z, s->box.min.z, s->box.max.z);
            float dx = feet->x - cx, dz = feet->z - cz;
            float d2 = dx * dx + dz * dz;

            if (d2 < radius * radius)
            {
                float rise = s->box.max.y - feet->y;
                if (rise > 0.02f && rise <= STEP_UP_HEIGHT)
                {
                    /* 可攀上：先驗證新的腳部高度上方留有頭部淨空，被擋住就退回水平推出 */
                    bp_aabb up = bp_aabb_make(
                        (Vector3){ feet->x - radius, s->box.max.y + HEAD_CLEARANCE, feet->z - radius },
                        (Vector3){ feet->x + radius, s->box.max.y + HEAD_CLEARANCE + height, feet->z + radius });
                    bool blocked = false;
                    for (int j = 0; j < g_world.solid_count; j++)
                    {
                        if (j == i) continue;
                        const bp_solid *o = &g_world.solids[j];
                        if (o->destroyed || o->shoot_through) continue;
                        if (bp_aabb_overlaps(&up, &o->box)) { blocked = true; break; }
                    }
                    if (!blocked)
                    {
                        if (!doStep || s->box.max.y > stepTo)
                        {
                            doStep = true;
                            stepTo = s->box.max.y;
                        }
                        continue;
                    }
                }

                if (d2 > 1e-8f)
                {
                    float d = sqrtf(d2);
                    float push = radius - d;
                    feet->x += (dx / d) * push;
                    feet->z += (dz / d) * push;
                    if (velocity)
                    {
                        float nx = dx / d, nz = dz / d;
                        float vn = velocity->x * nx + velocity->z * nz;
                        if (vn < 0.0f) { velocity->x -= vn * nx; velocity->z -= vn * nz; }
                    }
                }
                else
                {
                    /* 中心已陷入方塊投影內：沿最淺的軸向彈出（離哪個面最近就往哪邊推） */
                    float pxMin = feet->x - s->box.min.x, pxMax = s->box.max.x - feet->x;
                    float pzMin = feet->z - s->box.min.z, pzMax = s->box.max.z - feet->z;
                    float m = BP_MIN(BP_MIN(pxMin, pxMax), BP_MIN(pzMin, pzMax));
                    if (m == pxMin)      feet->x = s->box.min.x - radius;
                    else if (m == pxMax) feet->x = s->box.max.x + radius;
                    else if (m == pzMin) feet->z = s->box.min.z - radius;
                    else                 feet->z = s->box.max.z + radius;
                }
                body = bp_aabb_make((Vector3){ feet->x - radius, feet->y, feet->z - radius },
                                    (Vector3){ feet->x + radius, feet->y + height, feet->z + radius });
            }
        }
    }

    if (doStep)
    {
        feet->y = stepTo;
        if (velocity) velocity->y = BP_MAX(velocity->y, 0.0f);
        grounded = true;
        body = bp_aabb_make((Vector3){ feet->x - radius, feet->y, feet->z - radius },
                            (Vector3){ feet->x + radius, feet->y + height, feet->z + radius });
    }

    /* ---- 垂直解算：天花板夾限 + 地板落地 ---- */
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        if (feet->x + radius < s->box.min.x || feet->x - radius > s->box.max.x) continue;
        if (feet->z + radius < s->box.min.z || feet->z - radius > s->box.max.z) continue;
        if (feet->y + height <= s->box.min.y || feet->y >= s->box.max.y) continue;

        if (!s->shoot_through)
        {
            /* 落在方塊頂面：腳部已陷入頂面下方但落差不大，直接抬上頂面並接地 */
            if (feet->y < s->box.max.y && feet->y > s->box.min.y + 0.01f &&
                (s->box.max.y - feet->y) < height * 0.5f)
            {
                feet->y = s->box.max.y;
                if (velocity && velocity->y < 0.0f) velocity->y = 0.0f;
                grounded = true;
                groundMat = s->material;
                continue;
            }
            /* 頭部撞上方塊底面：把身體壓回天花板之下並消除向上速度 */
            if (feet->y < s->box.min.y && feet->y + height > s->box.min.y)
            {
                feet->y = s->box.min.y - height;
                if (velocity && velocity->y > 0.0f) velocity->y = 0.0f;
                continue;
            }
        }

        /* 可穿透幾何（柵欄、力場）在玩家位於其上方時仍要能承載重量，但絕不擋住頭部——
         * 這正是柵欄腳感正確的關鍵：子彈穿得過、人站得住、跳起來不撞頭。 */
        if (feet->y < s->box.max.y && feet->y > s->box.min.y &&
            (s->box.max.y - feet->y) < 0.35f)
        {
            feet->y = s->box.max.y;
            if (velocity && velocity->y < 0.0f) velocity->y = 0.0f;
            grounded = true;
            groundMat = s->material;
        }
    }

    /* 世界地板保險網：無論如何都不能讓玩家掉出地圖外 */
    if (feet->y < g_world.bounds_min.y + 1.0f)
    {
        feet->y = g_world.bounds_min.y + 1.0f;
        if (velocity && velocity->y < 0.0f) velocity->y = 0.0f;
        grounded = true;
        groundMat = MAT_CONCRETE;
    }

    if (on_ground) *on_ground = grounded;
    if (out_material) *out_material = groundMat;
    return grounded;
}

/* ====================================================================================== */
/* 8. 移動體與觸發器                                                                         */
/* ====================================================================================== */
static void mover_set_position(bp_mover *m, float phase)
{
    bp_solid *s = m->solid;
    if (!s) return;
    Vector3 size = bp_aabb_size(&s->box);
    /* 加入緩動（easing）：讓門加速起步、緩和收尾，而不是以等速猛衝 */
    float e = phase < 0.5f ? 2.0f * phase * phase
                           : 1.0f - powf(-2.0f * phase + 2.0f, 2.0f) * 0.5f;
    Vector3 p = Vector3Add(m->from, Vector3Scale(Vector3Subtract(m->to, m->from), e));
    s->box.min = p;
    s->box.max = Vector3Add(p, size);
}

void bp_world_update(float dt)
{
    if (!g_world.loaded) return;

    for (int i = 0; i < g_world.mover_count; i++)
    {
        bp_mover *m = &g_world.movers[i];
        if (m->cooldown > 0.0f) m->cooldown -= dt;

        float target = m->open ? 1.0f : 0.0f;
        if (BP_ABS(m->phase - target) > 0.0001f)
        {
            m->phase = bp_approach(m->phase, target, dt / m->duration);
            if (BP_ABS(m->phase - target) <= 0.0001f) m->phase = target;
            mover_set_position(m, m->phase);
        }
        else if (m->open && m->auto_close > 0.0f)
        {
            m->hold += dt;
            if (m->hold >= m->auto_close)
            {
                m->open = false;
                m->hold = 0.0f;
                m->triggered = false;
                if (m->sfx > 0) BP_LOGD("world: mover %d auto-closing", i);
            }
        }
    }

    for (int i = 0; i < g_world.trigger_count; i++)
    {
        bp_trigger *t = &g_world.triggers[i];
        if (t->consumed && t->respawn > 0.0f)
        {
            t->timer -= dt;
            if (t->timer <= 0.0f) { t->consumed = false; t->timer = 0.0f; }
        }
    }
}

/* 開啟所有觸發範圍涵蓋 `pos` 的移動體。玩家控制器踩到地板壓板時會呼叫，戰鬥程式碼
 * 把門射開時也會呼叫。 */
void bp_world_activate_doors(Vector3 pos, float radius)
{
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        bp_trigger *t = &g_world.triggers[i];
        if (t->kind != TRIG_DOOR) continue;
        if (Vector3Distance(t->position, pos) > BP_MAX(radius, t->radius)) continue;

        for (int m = 0; m < g_world.mover_count; m++)
        {
            bp_mover *mv = &g_world.movers[m];
            if (mv->open) continue;
            /* 離這個壓板最近的移動體，就是它所控制的門 */
            Vector3 mc = bp_aabb_center(&mv->solid->box);
            if (Vector3Distance(mc, t->position) > 9.0f) continue;
            mv->open = true;
            mv->triggered = true;
            mv->hold = 0.0f;
            if (mv->auto_close <= 0.0f) mv->auto_close = 6.0f;
            t->consumed = true;
            t->timer = mv->auto_close;
            BP_LOGD("world: door %d opened by pad %d", m, i);
        }
    }
}

int bp_world_next_spawn(int kind, int *cursor)
{
    if (g_world.trigger_count == 0) return -1;
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        int idx = (*cursor + i) % g_world.trigger_count;
        if (g_world.triggers[idx].kind == kind)
        {
            *cursor = (idx + 1) % g_world.trigger_count;
            return idx;
        }
    }
    return -1;
}

int bp_world_spawn_count(int kind)
{
    int n = 0;
    for (int i = 0; i < g_world.trigger_count; i++)
        if (g_world.triggers[i].kind == kind) n++;
    return n;
}

/* 從少數幾個隨機候選點裡評分挑選，而不是永遠取唯一最遠的生成點。永遠取最遠會讓敵人的
 * 進攻路線固定不變，玩家很快就會學會蹲守同一個門口刷怪。隨機取最佳既保住壓力的變化性，
 * 又仍然尊重最小距離規則。 */
Vector3 bp_world_random_spawn(int kind, Vector3 avoid, float min_distance)
{
    int best = -1;
    float bestScore = -1e9f;
    int candidates = BP_MIN(10, BP_MAX(4, g_world.spawn_enemy_count));

    for (int c = 0; c < candidates; c++)
    {
        int idx = bp_rng_int(&g_engine.rng, g_world.trigger_count);
        if (g_world.triggers[idx].kind != kind)
        {
            for (int k = 0; k < g_world.trigger_count; k++)
            {
                int j = (idx + k) % g_world.trigger_count;
                if (g_world.triggers[j].kind == kind) { idx = j; break; }
            }
        }
        if (g_world.triggers[idx].kind != kind) break;

        Vector3 p = g_world.triggers[idx].position;
        float d = Vector3Distance(p, avoid);
        float score = d;
        if (d < min_distance) score -= (min_distance - d) * 6.0f;
        if (bp_world_point_solid((Vector3){ p.x, p.y + 0.9f, p.z })) score -= 1000.0f;
        if (score > bestScore) { bestScore = score; best = idx; }
    }

    if (best < 0)
        for (int i = 0; i < g_world.trigger_count; i++)
            if (g_world.triggers[i].kind == kind) { best = i; break; }

    if (best < 0) return g_world.player_start;
    return g_world.triggers[best].position;
}

/* ====================================================================================== */
/* 9. 天穹                                                                                   */
/* ====================================================================================== */
/*
 * 每個關卡建造一次：一顆內翻球體，以逐頂點顏色上色。對必須相容固定管線的算圖器而言，
 * 這個做法有兩個關鍵優點：
 *   - 完全不需要自訂著色器，因此 OpenGL 3.3 與 WebGL 1 跑的是同一條程式碼路徑
 *   - 只需一次 draw call，而且永遠不必跟著相機重新置中——競技場夠小，在 1000 單位
 *     半徑下，68 公尺範圍內的視差小到看不出來
 * 同一顆天穹也兼作主選單的背景，主選單會讓相機緩慢自轉。
 */
static void build_sky_dome(Color zenith, Color horizon, Color ground,
                           Vector3 glowDir, Color glowCol)
{
    if (s_sky_ready) { UnloadModel(s_sky_model); s_sky_ready = false; }

    int vCount = (SKY_RINGS + 1) * (SKY_SLICES + 1);
    int iCount = SKY_RINGS * SKY_SLICES * 6;

    Vector3 *verts  = (Vector3 *)RL_MALLOC(sizeof(Vector3) * (size_t)vCount);
    Color   *colors = (Color *)RL_MALLOC(sizeof(Color) * (size_t)vCount);
    unsigned short *idx = (unsigned short *)RL_MALLOC(sizeof(unsigned short) * (size_t)iCount);
    if (!verts || !colors || !idx)
    {
        BP_LOGE("world: sky allocation failed");
        RL_FREE(verts); RL_FREE(colors); RL_FREE(idx);
        return;
    }

    Vector3 gd = Vector3Normalize(glowDir);
    int vi = 0;

    for (int r = 0; r <= SKY_RINGS; r++)
    {
        float v = (float)r / (float)SKY_RINGS;
        float pitch = (0.5f - v) * BP_PI;             /* 頂端為 +90 度 */
        float cp = cosf(pitch), sp = sinf(pitch);

        for (int sIx = 0; sIx <= SKY_SLICES; sIx++)
        {
            float u = (float)sIx / (float)SKY_SLICES;
            float yaw = u * 2.0f * BP_PI;

            Vector3 dir = { cp * sinf(yaw), sp, cp * cosf(yaw) };
            verts[vi] = Vector3Scale(dir, SKY_RADIUS);

            Color c;
            if (dir.y >= 0.0f)
            {
                float t = powf(dir.y, 0.52f);
                c.r = (unsigned char)((float)horizon.r + ((float)zenith.r - (float)horizon.r) * t);
                c.g = (unsigned char)((float)horizon.g + ((float)zenith.g - (float)horizon.g) * t);
                c.b = (unsigned char)((float)horizon.b + ((float)zenith.b - (float)horizon.b) * t);
            }
            else
            {
                float t = powf(-dir.y, 0.72f);
                c.r = (unsigned char)((float)horizon.r + ((float)ground.r - (float)horizon.r) * t);
                c.g = (unsigned char)((float)horizon.g + ((float)ground.g - (float)horizon.g) * t);
                c.b = (unsigned char)((float)horizon.b + ((float)ground.b - (float)horizon.b) * t);
            }
            c.a = 255;

            /* 沿單一方向貼著地平線的熔爐光暈 */
            float d = Vector3DotProduct(dir, gd);
            if (d > 0.0f)
            {
                float g = powf(d, 30.0f) * 1.05f + powf(d, 5.0f) * 0.20f;
                g *= 0.25f + 0.75f * BP_CLAMP(1.0f - BP_ABS(dir.y) * 1.5f, 0.0f, 1.0f);
                c.r = (unsigned char)BP_CLAMP((int)((float)c.r + (float)glowCol.r * g), 0, 255);
                c.g = (unsigned char)BP_CLAMP((int)((float)c.g + (float)glowCol.g * g), 0, 255);
                c.b = (unsigned char)BP_CLAMP((int)((float)c.b + (float)glowCol.b * g), 0, 255);
            }

            /* 地平線上方的稀疏星星 */
            if (dir.y > 0.10f)
            {
                uint32_t h = (uint32_t)(u * 997.0f) * 374761393u + (uint32_t)(v * 991.0f) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
                if ((h & 0xFFFF) > 0xFFD8)
                {
                    float b = BP_CLAMP((dir.y - 0.10f) * 2.6f, 0.0f, 1.0f)
                            * (0.35f + (float)((h >> 16) & 0xFF) / 380.0f);
                    c.r = (unsigned char)BP_CLAMP(c.r + (int)(190.0f * b), 0, 255);
                    c.g = (unsigned char)BP_CLAMP(c.g + (int)(200.0f * b), 0, 255);
                    c.b = (unsigned char)BP_CLAMP(c.g + (int)(225.0f * b), 0, 255);
                }
            }
            colors[vi] = c;
            vi++;
        }
    }

    int ii = 0;
    for (int r = 0; r < SKY_RINGS; r++)
    {
        for (int sIx = 0; sIx < SKY_SLICES; sIx++)
        {
            int a = r * (SKY_SLICES + 1) + sIx;
            int b = a + SKY_SLICES + 1;
            /* 繞序反向：相機位於天穹的「內部」 */
            idx[ii++] = (unsigned short)a;
            idx[ii++] = (unsigned short)b;
            idx[ii++] = (unsigned short)(a + 1);
            idx[ii++] = (unsigned short)(a + 1);
            idx[ii++] = (unsigned short)b;
            idx[ii++] = (unsigned short)(b + 1);
        }
    }

    s_sky_mesh.vertexCount = vi;
    s_sky_mesh.triangleCount = ii / 3;
    s_sky_mesh.vertices = (float *)verts;
    s_sky_mesh.colors = (unsigned char *)colors;
    s_sky_mesh.indices = idx;
    s_sky_mesh.texcoords = NULL;
    s_sky_mesh.normals = NULL;
    s_sky_mesh.tangents = NULL;
    s_sky_mesh.boneIndices = NULL;
    s_sky_mesh.boneWeights = NULL;
    s_sky_mesh.vboId = NULL;

    UploadMesh(&s_sky_mesh, false);
    s_sky_model = LoadModelFromMesh(s_sky_mesh);

    if (s_sky_model.materials)
    {
        s_sky_model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        s_sky_model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture.id = 0;
        s_sky_model.materials[0].maps[MATERIAL_MAP_SPECULAR].color = (Color){ 0, 0, 0, 255 };
    }
    s_sky_ready = (s_sky_model.meshCount > 0);
    if (s_sky_ready) BP_LOGI("world: sky dome built (%d verts, %d tris)", vi, ii / 3);
}

void bp_world_draw_sky(Camera3D cam)
{
    BP_UNUSED(cam);
    if (!s_sky_ready) { ClearBackground(g_world.fog_color); return; }

    /* 關閉深度寫入：天穹絕不能遮住場景幾何，也絕不能被遠平面（far plane）裁掉。
     * 關閉背面剔除：因為我們畫的是它的內側表面。 */
    rlDisableBackfaceCulling();
    DrawMesh(s_sky_mesh, s_sky_model.materials[0], MatrixIdentity());
    rlEnableBackfaceCulling();
}

/* ====================================================================================== */
/* 10. 世界繪製                                                                              */
/* ====================================================================================== */
void bp_world_draw(Camera3D cam, Vector3 viewer)
{
    BP_UNUSED(cam);

    /* ---- 烘焙好的靜態幾何：整張地圖每種材質一次 draw call ---- */
    for (int i = 0; i < g_world.batch_count; i++)
    {
        bp_static_batch *b = &g_world.batch[i];
        if (!b->valid) continue;
        DrawMesh(b->mesh, b->material, MatrixIdentity());
    }

    /* ---- 動態幾何：門、升降平台、可破壞的桶 ---- */
    float cull2 = CULL_DISTANCE * CULL_DISTANCE;
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (!s->dynamic || s->destroyed) continue;

        Vector3 c = bp_aabb_center(&s->box);
        if (Vector3LengthSqr(Vector3Subtract(c, viewer)) > cull2) continue;

        Vector3 size = bp_aabb_size(&s->box);
        const bp_material_def *md = bp_world_material(s->material);
        Texture2D t = bp_tex(md->texture);

        if (s->explode)
        {
            /* 桶是唯一值得給予真實輪廓的動態道具：一個貼了貼圖的圓柱，從任何角度都能
             * 在一瞬間讀出「可射爆的爆裂物」——這正是競技場射擊遊戲中一個道具的全部
             * 職責。 */
            Vector3 base = { c.x, s->box.min.y, c.z };
            if (bp_model_valid(MDL_BARREL))
            {
                Vector3 sc = { size.x / 0.68f, size.y / 1.05f, size.x / 0.68f };
                DrawModelEx(bp_model_get(MDL_BARREL), base, (Vector3){ 0, 1, 0 }, 0.0f, sc, md->tint);
            }
            else DrawCylinder(base, size.x * 0.5f, size.x * 0.5f, size.y, 12, md->tint);
            /* 危險警示環：即使只看剪影也認得出這是個桶 */
            DrawCylinderWires(base, size.x * 0.52f, size.x * 0.52f, size.y, 12,
                              (Color){ 240, 200, 60, 190 });
        }
        else
        {
            DrawCube(c, size.x, size.y, size.z, md->tint);
            DrawCubeWires(c, size.x * 1.001f, size.y * 1.001f, size.z * 1.001f,
                          (Color){ 0, 0, 0, 70 });
        }
        BP_UNUSED(t);
    }

    /* ---- 自發光 bloom 近似 ----
     * raylib 的預設材質沒有光照 pass，所以「燈是亮的」這件事必須靠加成混合的光暈來
     * 營造。每片燈板畫一個稍微放大的加成方塊，成本幾乎為零，卻能立刻被讀成發光體。 */
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        const bp_material_def *md = bp_world_material(s->material);
        if (!md->emissive) continue;

        Vector3 c = bp_aabb_center(&s->box);
        Vector3 d = Vector3Subtract(c, viewer);
        float dist2 = Vector3LengthSqr(d);
        if (dist2 > 1200.0f) continue;

        float fade = BP_CLAMP(1.0f - sqrtf(dist2) / 34.0f, 0.0f, 1.0f);
        Vector3 size = bp_aabb_size(&s->box);
        Color glow = { md->tint.r, md->tint.g, md->tint.b, (unsigned char)(54.0f * fade) };
        DrawCube(c, size.x * 1.10f, size.y * 1.10f, size.z * 1.10f, glow);
    }
    rlSetBlendMode(RL_BLEND_ALPHA);
}

void bp_world_draw_debug(void)
{
    for (int i = 0; i < g_world.solid_count; i++)
    {
        const bp_solid *s = &g_world.solids[i];
        if (s->destroyed) continue;
        Vector3 c = bp_aabb_center(&s->box);
        Vector3 size = bp_aabb_size(&s->box);
        Color col = s->explode ? ORANGE : (s->shoot_through ? SKYBLUE : LIME);
        DrawCubeWires(c, size.x, size.y, size.z, col);
    }
    for (int i = 0; i < g_world.trigger_count; i++)
    {
        const bp_trigger *t = &g_world.triggers[i];
        Color col;
        switch (t->kind)
        {
            case TRIG_ENEMY_SPAWN:  col = RED;    break;
            case TRIG_HEALTH_SPAWN: col = GREEN;  break;
            case TRIG_ARMOR_SPAWN:  col = BLUE;   break;
            case TRIG_AMMO_SPAWN:   col = YELLOW; break;
            case TRIG_WEAPON_SPAWN: col = ORANGE; break;
            case TRIG_EXIT:         col = VIOLET; break;
            case TRIG_SECRET:       col = PINK;   break;
            case TRIG_DOOR:         col = SKYBLUE;break;
            default:                col = MAGENTA;break;
        }
        if (t->consumed) col = DARKGRAY;
        DrawSphereWires(t->position, t->radius, 6, 8, col);
    }
    for (int i = 0; i < g_world.mover_count; i++)
    {
        const bp_mover *m = &g_world.movers[i];
        DrawLine3D(m->from, m->to, m->open ? LIME : GRAY);
    }
}
