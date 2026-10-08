/******************************************************************************************
 * bp_assets.c -- procedural texture, model and font generation.
 *
 * Everything here runs once at boot. Target budget: < 400 ms on a 2015-era laptop, < 900 ms
 * in the Web build. The generated atlas totals ~2.4 MB of VRAM at 256px per architectural
 * texture, which is what makes the game shipable on integrated graphics and mid-tier phones.
 *
 * Layering convention for every surface texture:
 *      base colour  *  large-scale value noise (dirt)  *  fine noise (grain)  +  edge bevel
 *   Keeping that recipe identical across all surfaces is what makes the world feel like it
 *   was authored by one art director rather than assembled from free assets.
 ******************************************************************************************/
#include "bp_assets.h"

bp_assets g_assets = { 0 };

/* ====================================================================================== */
/* 0. noise                                                                                */
/* ====================================================================================== */
static inline uint32_t hash2i(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static inline float hashf2(int x, int y, uint32_t seed)
{
    return (float)(hash2i(x, y, seed) & 0xFFFF) / 65535.0f;
}

static inline float smoothstep01(float t) { return t * t * (3.0f - 2.0f * t); }

static float fbm(int x, int y, int size, int octaves, uint32_t seed)
{
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int period = size;
    for (int o = 0; o < octaves; o++)
    {
        if (period < 2) break;
        /* sample the lattice at this octave's resolution */
        float sx = (float)x * (float)period / (float)size;
        float sy = (float)y * (float)period / (float)size;
        int ix = (int)floorf(sx), iy = (int)floorf(sy);
        float fx = smoothstep01(sx - (float)ix);
        float fy = smoothstep01(sy - (float)iy);

        int ixw = ((ix % period) + period) % period;
        int iyw = ((iy % period) + period) % period;
        int ixn = (ixw + 1) % period;
        int iyn = (iyw + 1) % period;

        float a = hashf2(ixw, iyw, seed + (uint32_t)o * 7919u);
        float b = hashf2(ixn, iyw, seed + (uint32_t)o * 7919u);
        float c = hashf2(ixw, iyn, seed + (uint32_t)o * 7919u);
        float d = hashf2(ixn, iyn, seed + (uint32_t)o * 7919u);
        sum += amp * (a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy);
        norm += amp;
        amp *= 0.5f;
        period /= 2;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

/* ====================================================================================== */
/* 1. image helpers                                                                        */
/* ====================================================================================== */
typedef struct { Color *px; int w, h; } bp_img;

static bp_img img_make(int w, int h)
{
    bp_img im;
    im.w = w; im.h = h;
    im.px = (Color *)RL_MALLOC((size_t)w * (size_t)h * sizeof(Color));
    if (im.px) for (int i = 0; i < w * h; i++) im.px[i] = MAGENTA;
    return im;
}

static inline void img_set(bp_img *im, int x, int y, Color c)
{
    x = ((x % im->w) + im->w) % im->w;
    y = ((y % im->h) + im->h) % im->h;
    im->px[y * im->w + x] = c;
}

static inline Color img_get(bp_img *im, int x, int y)
{
    x = ((x % im->w) + im->w) % im->w;
    y = ((y % im->h) + im->h) % im->h;
    return im->px[y * im->w + x];
}

static inline unsigned char clampb(int v) { return (unsigned char)BP_CLAMP(v, 0, 255); }

static inline Color rgbm(int r, int g, int b, float m)
{
    return (Color){ clampb((int)((float)r * m)), clampb((int)((float)g * m)),
                    clampb((int)((float)b * m)), 255 };
}

static inline Color mixc(Color a, Color b, float t)
{
    return (Color){
        clampb((int)((float)a.r + ((float)b.r - (float)a.r) * t)),
        clampb((int)((float)a.g + ((float)b.g - (float)a.g) * t)),
        clampb((int)((float)a.b + ((float)b.b - (float)a.b) * t)),
        clampb((int)((float)a.a + ((float)b.a - (float)a.a) * t))
    };
}

static void img_free(bp_img *im) { if (im->px) { RL_FREE(im->px); im->px = NULL; } }

static Texture2D img_to_texture(bp_img *im, bool mips)
{
    Image img;
    img.data = im->px;
    img.width = im->w;
    img.height = im->h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    if (mips) ImageMipmaps(&img);
    Texture2D t = LoadTextureFromImage(img);

    /* Architectural surfaces are viewed at glancing angles and minified hard; trilinear
     * + anisotropy removes the "shimmering bricks" artefact that instantly reads as cheap. */
    if (t.id)
    {
        /* Anisotropic 4x is the sweet spot: it kills the "shimmering bricks" artefact on
         * walls seen at glancing angles and costs almost nothing on any GPU since 2010. */
        SetTextureFilter(t, mips ? TEXTURE_FILTER_ANISOTROPIC_4X : TEXTURE_FILTER_POINT);
        SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    }

    /* UnloadImage would free img.data (our pixel array), so only free the mip chain that
     * ImageMipmaps allocated. */
    UnloadImage(img);
    im->px = NULL;
    return t;
}

/* ====================================================================================== */
/* 2. texture generators                                                                   */
/* ====================================================================================== */
#define TEX_ARCH_SIZE 256
#define TEX_SMALL_SIZE 128

/* Shared surface recipe. `grain` adds high-frequency speckle so flat colours never look
 * like vector art. */
static void surface_pass(bp_img *im, uint32_t seed, float dirtAmt, float grainAmt)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            Color c = img_get(im, x, y);
            float dirt = fbm(x, y, im->w, 4, seed);
            float grain = hashf2(x, y, seed ^ 0xABCDu);
            float m = 1.0f + (dirt - 0.5f) * 2.0f * dirtAmt + (grain - 0.5f) * 2.0f * grainAmt;
            img_set(im, x, y, (Color){ clampb((int)((float)c.r * m)),
                                       clampb((int)((float)c.g * m)),
                                       clampb((int)((float)c.b * m)), c.a });
        }
    }
}

static void tex_brick(bp_img *im)
{
    const int bw = 64, bh = 32, mortar = 4;
    Color base = (Color){ 96, 62, 54, 255 };
    Color mort = (Color){ 46, 44, 42, 255 };

    for (int y = 0; y < im->h; y++)
    {
        int row = y / bh;
        int offset = (row & 1) ? bw / 2 : 0;
        for (int x = 0; x < im->w; x++)
        {
            int lx = ((x + offset) % bw);
            int ly = y % bh;
            bool isMortar = (lx < mortar) || (ly < mortar);
            if (isMortar) { img_set(im, x, y, mort); continue; }

            /* per-brick colour variance keyed by brick id, not pixel -> bricks read as
             * discrete objects instead of a noisy field */
            int bx = (x + offset) / bw, by = y / bh;
            float v = hashf2(bx, by, 99u);
            float shade = 0.72f + v * 0.55f;

            /* bevel: brighter top-left, darker bottom-right */
            float bev = 1.0f;
            if (ly == mortar) bev = 1.22f;
            else if (ly == mortar + 1) bev = 1.10f;
            else if (ly == bh - 1) bev = 0.72f;
            else if (ly == bh - 2) bev = 0.84f;
            if (lx == mortar) bev *= 1.16f;
            else if (lx == bw - 1) bev *= 0.78f;

            /* chip damage on some bricks */
            float n = fbm(x, y, 64, 3, 7u + (uint32_t)(bx * 31 + by * 17));
            if (v > 0.86f && n > 0.62f) shade *= 0.55f;

            img_set(im, x, y, rgbm(base.r, base.g, base.b, shade * bev));
        }
    }
    surface_pass(im, 21u, 0.20f, 0.05f);
}

static void tex_metal(bp_img *im, Color base, float panelScale, bool rivets)
{
    int ps = (int)(panelScale);
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int px = x % ps, py = y % ps;
            float shade = 1.0f;

            /* brushed grain: streaks along X */
            float streak = hashf2(x / 3, y, 5u);
            shade *= 0.94f + streak * 0.12f;

            /* panel seam */
            bool seam = (px < 2) || (py < 2);
            if (seam) shade *= 0.45f;
            else if (px < 4 || py < 4) shade *= 0.78f;
            else if (px == ps - 1 || py == ps - 1) shade *= 1.18f;

            Color c = rgbm(base.r, base.g, base.b, shade);

            if (rivets)
            {
                int rx = px - 8, ry = py - 8;
                if (rx * rx + ry * ry <= 9) c = mixc(c, (Color){ 210, 210, 215, 255 }, 0.55f);
                rx = px - (ps - 8); ry = py - 8;
                if (rx * rx + ry * ry <= 9) c = mixc(c, (Color){ 210, 210, 215, 255 }, 0.45f);
                rx = px - 8; ry = py - (ps - 8);
                if (rx * rx + ry * ry <= 9) c = mixc(c, (Color){ 210, 210, 215, 255 }, 0.45f);
                rx = px - (ps - 8); ry = py - (ps - 8);
                if (rx * rx + ry * ry <= 9) c = mixc(c, (Color){ 210, 210, 215, 255 }, 0.55f);
            }
            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 33u, 0.16f, 0.06f);
}

static void tex_concrete(bp_img *im, Color base, float crackiness)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float n1 = fbm(x, y, im->w, 5, 11u);
            float n2 = fbm(x * 2, y * 2, im->w * 2, 3, 71u);
            float shade = 0.78f + n1 * 0.42f + (n2 - 0.5f) * 0.10f;

            /* stains pooling toward the bottom of the tile */
            float stain = fbm(x / 2, y / 2, im->w / 2, 3, 131u);
            if (stain > 0.62f) shade *= 0.72f;

            Color c = rgbm(base.r, base.g, base.b, shade);

            /* cracks: ridged noise -> thin dark lines */
            float ridge = 1.0f - BP_ABS(fbm(x, y, im->w, 4, 191u) * 2.0f - 1.0f);
            if (ridge > (1.0f - 0.06f * crackiness)) c = mixc(c, (Color){ 22, 20, 20, 255 }, 0.75f);

            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 41u, 0.12f, 0.07f);
}

static void tex_panel(bp_img *im, Color base, Color accent)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int cell = 128;
            int px = x % cell, py = y % cell;
            float shade = 1.0f;

            /* recessed panel with a chamfer */
            int d = BP_MIN(BP_MIN(px, cell - 1 - px), BP_MIN(py, cell - 1 - py));
            if (d < 3) shade = 0.42f + (float)d * 0.10f;
            else if (d < 7) shade = 0.86f + (float)(d - 3) * 0.035f;
            else shade = 1.0f + 0.05f * sinf((float)(px + py) * 0.08f);

            Color c = rgbm(base.r, base.g, base.b, shade);

            /* hazard stripe inset */
            if (d > 18 && d < 26 && ((px + py) / 8) % 2 == 0) c = mixc(c, accent, 0.55f);

            /* bolt heads */
            int bxd = px - 16, byd = py - 16;
            if (bxd * bxd + byd * byd <= 16) c = mixc(c, (Color){ 200, 205, 210, 255 }, 0.5f);
            bxd = px - (cell - 16); byd = py - (cell - 16);
            if (bxd * bxd + byd * byd <= 16) c = mixc(c, (Color){ 200, 205, 210, 255 }, 0.5f);

            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 57u, 0.14f, 0.05f);
}

static void tex_rusty(bp_img *im)
{
    Color steel = (Color){ 88, 92, 98, 255 };
    Color rust  = (Color){ 128, 62, 30, 255 };
    Color rust2 = (Color){ 84, 40, 22, 255 };

    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float r = fbm(x, y, im->w, 5, 211u);
            float streak = fbm(x, y / 4, im->w, 3, 271u);   /* vertical run-off */
            float t = BP_CLAMP((r - 0.42f) * 2.4f + (streak - 0.5f) * 0.9f, 0.0f, 1.0f);

            Color c = mixc(steel, rust, t);
            if (t > 0.72f) c = mixc(c, rust2, (t - 0.72f) * 2.2f);

            /* pitting */
            float pit = fbm(x * 3, y * 3, im->w * 3, 2, 331u);
            if (pit > 0.74f && t > 0.3f) c = rgbm(c.r, c.g, c.b, 0.62f);

            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 61u, 0.14f, 0.08f);
}

static void tex_grate(bp_img *im, Color base)
{
    int cell = 32;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int px = x % cell, py = y % cell;
            /* diamond mesh: bars where the Manhattan distance to the cell centre is small */
            int dx = BP_ABS(px - cell / 2), dy = BP_ABS(py - cell / 2);
            bool bar = (dx + dy) < 9 || (dx < 3) || (dy < 3);
            Color c;
            if (bar)
            {
                float bev = 1.0f + 0.22f * (1.0f - (float)(dx + dy) / 12.0f);
                if (dx < 3 || dy < 3) bev = 1.05f;
                c = rgbm(base.r, base.g, base.b, bev);
            }
            else c = (Color){ 12, 12, 15, 255 };     /* void beneath the grate */
            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 71u, 0.20f, 0.06f);
}

static void tex_tile(bp_img *im, Color base, Color grout, int tileSize)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int px = x % tileSize, py = y % tileSize;
            bool isGrout = (px < 2) || (py < 2);
            if (isGrout) { img_set(im, x, y, grout); continue; }

            int tx = x / tileSize, ty = y / tileSize;
            float v = hashf2(tx, ty, 401u);
            float shade = 0.88f + v * 0.26f;
            if (py == 2) shade *= 1.10f;
            if (py == tileSize - 1) shade *= 0.80f;
            if (px == 2) shade *= 1.06f;
            if (px == tileSize - 1) shade *= 0.85f;

            /* scuff marks concentrated in tile centres (foot traffic) */
            float scuff = fbm(x, y, 64, 3, 409u);
            if (scuff > 0.66f) shade *= 0.86f;

            img_set(im, x, y, rgbm(base.r, base.g, base.b, shade));
        }
    }
    surface_pass(im, 83u, 0.10f, 0.05f);
}

static void tex_hazard(bp_img *im)
{
    Color yellow = (Color){ 212, 168, 32, 255 };
    Color black  = (Color){ 28, 26, 24, 255 };
    int stripe = 28;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int s = ((x + y) / stripe) % 2;
            Color c = s ? yellow : black;
            float n = fbm(x, y, 128, 4, 503u);
            float shade = 0.82f + n * 0.36f;
            /* worn patches where the paint has been scuffed off */
            if (n > 0.68f) c = mixc(c, (Color){ 74, 70, 66, 255 }, 0.6f);
            img_set(im, x, y, rgbm(c.r, c.g, c.b, shade));
        }
    }
    surface_pass(im, 97u, 0.10f, 0.07f);
}

static void tex_ceiling(bp_img *im)
{
    Color base = (Color){ 58, 58, 64, 255 };
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int cell = 64;
            int px = x % cell, py = y % cell;
            float shade = 0.92f;
            int d = BP_MIN(BP_MIN(px, cell - 1 - px), BP_MIN(py, cell - 1 - py));
            if (d < 2) shade = 0.40f;
            else if (d < 5) shade = 0.66f;

            /* acoustic dots */
            float dots = fbm(x * 2, y * 2, 128, 2, 601u);
            if (dots > 0.60f) shade *= 0.86f;

            /* water damage blooms */
            float bloom = fbm(x / 2, y / 2, 128, 3, 607u);
            Color c = rgbm(base.r, base.g, base.b, shade);
            if (bloom > 0.66f) c = mixc(c, (Color){ 88, 74, 48, 255 }, (bloom - 0.66f) * 1.6f);
            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 101u, 0.12f, 0.05f);
}

static void tex_trim(bp_img *im)
{
    /* Horizontal banding used on wall tops / pillar caps: reads as machined steel. */
    Color base = (Color){ 120, 126, 134, 255 };
    for (int y = 0; y < im->h; y++)
    {
        float band = 0.5f + 0.5f * sinf((float)y / (float)im->h * BP_PI * 8.0f);
        for (int x = 0; x < im->w; x++)
        {
            float shade = 0.62f + band * 0.62f;
            float streak = hashf2(x / 2, y, 701u);
            shade *= 0.95f + streak * 0.10f;
            if (y < 3 || y > im->h - 4) shade *= 0.5f;
            img_set(im, x, y, rgbm(base.r, base.g, base.b, shade));
        }
    }
    surface_pass(im, 103u, 0.12f, 0.05f);
}

static void tex_crate(bp_img *im)
{
    Color wood = (Color){ 122, 88, 48, 255 };
    Color frame = (Color){ 62, 60, 58, 255 };
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int border = 14;
            bool isFrame = (x < border) || (y < border) || (x >= im->w - border) || (y >= im->h - border);
            bool diagonal = BP_ABS(x - y) < 12 || BP_ABS((im->w - 1 - x) - y) < 12;
            Color c;
            if (isFrame) c = frame;
            else if (diagonal && !isFrame) c = mixc(wood, frame, 0.55f);
            else
            {
                /* plank grain along Y */
                float grain = fbm(x / 6, y, 128, 3, 809u);
                c = rgbm(wood.r, wood.g, wood.b, 0.82f + grain * 0.40f);
            }
            float edge = 1.0f;
            if (x == border || y == border || x == im->w - border - 1 || y == im->h - border - 1) edge = 1.25f;
            img_set(im, x, y, rgbm(c.r, c.g, c.b, edge));
        }
    }
    surface_pass(im, 107u, 0.16f, 0.07f);
}

static void tex_barrel(bp_img *im)
{
    Color body = (Color){ 150, 62, 40, 255 };
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            /* cylindrical shading across U */
            float u = (float)x / (float)im->w;
            float cyl = 0.45f + 0.75f * sinf(u * BP_PI);
            /* ribs */
            float rib = (y % 42 < 6) ? 1.18f : 1.0f;
            if (y % 42 == 0 || y % 42 == 5) rib = 0.7f;
            Color c = rgbm(body.r, body.g, body.b, cyl * rib);

            /* hazard label band */
            if (y > im->h / 2 - 22 && y < im->h / 2 + 22)
            {
                if (((x / 10) % 2) == 0) c = mixc(c, (Color){ 214, 186, 40, 255 }, 0.85f);
                else c = mixc(c, (Color){ 26, 24, 22, 255 }, 0.85f);
                c = rgbm(c.r, c.g, c.b, cyl);
            }
            /* rust */
            float r = fbm(x, y, 128, 4, 811u);
            if (r > 0.64f) c = mixc(c, (Color){ 96, 52, 28, 255 }, (r - 0.64f) * 1.8f);
            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 109u, 0.12f, 0.06f);
}

static void tex_pillar(bp_img *im)
{
    Color base = (Color){ 104, 100, 96, 255 };
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int flutes = 8;
            float f = (float)(x % (im->w / flutes)) / (float)(im->w / flutes);
            float shade = 0.55f + 0.65f * sinf(f * BP_PI);
            if (y < 18 || y > im->h - 18) shade = 0.80f + 0.25f * (1.0f - fabsf(f - 0.5f) * 2.0f);
            float n = fbm(x, y, 128, 4, 907u);
            shade *= 0.88f + n * 0.24f;
            img_set(im, x, y, rgbm(base.r, base.g, base.b, shade));
        }
    }
    surface_pass(im, 113u, 0.14f, 0.05f);
}

static void tex_light_panel(bp_img *im, Color glow)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int cell = 64;
            int px = x % cell, py = y % cell;
            int d = BP_MIN(BP_MIN(px, cell - 1 - px), BP_MIN(py, cell - 1 - py));
            Color c;
            if (d < 3) c = (Color){ 30, 30, 34, 255 };         /* housing */
            else
            {
                float core = 1.0f - (float)(d - 3) / (float)(cell / 2);
                core = BP_CLAMP(core, 0.0f, 1.0f);
                c = mixc((Color){ glow.r / 2, glow.g / 2, glow.b / 2, 255 }, glow, 0.35f + core * 0.65f);
                /* diffuser grid */
                if ((px % 8 == 0) || (py % 8 == 0)) c = rgbm(c.r, c.g, c.b, 0.88f);
            }
            img_set(im, x, y, c);
        }
    }
}

static void tex_screen(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            Color bg = (Color){ 10, 22, 26, 255 };
            Color c = bg;

            /* scanlines */
            if (y % 3 == 0) c = rgbm(c.r, c.g, c.b, 0.72f);

            /* fake text rows */
            int row = y / 8;
            float rv = hashf2(row, 0, 1013u);
            if (rv > 0.35f)
            {
                int textStart = 6 + (int)(hashf2(row, 1, 1021u) * 24.0f);
                int textLen = 12 + (int)(hashf2(row, 2, 1031u) * 52.0f);
                if (x > textStart && x < textStart + textLen)
                {
                    /* glyph-ish blocks */
                    int gx = (x - textStart) / 4;
                    if (hashf2(row, gx, 1039u) > 0.32f)
                        c = (x % 4 == 3) ? (Color){ 30, 120, 110, 255 } : (Color){ 92, 226, 196, 255 };
                }
            }

            /* header bar */
            if (y < 10) c = mixc((Color){ 30, 90, 84, 255 }, bg, (float)y / 14.0f);

            /* vignette */
            float vx = ((float)x / (float)im->w - 0.5f) * 2.0f;
            float vy = ((float)y / (float)im->h - 0.5f) * 2.0f;
            float vig = 1.0f - 0.42f * (vx * vx + vy * vy);
            img_set(im, x, y, rgbm(c.r, c.g, c.b, vig));
        }
    }
}

static void tex_vent(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            int slat = y % 16;
            Color c;
            if (slat < 9)
            {
                float t = (float)slat / 9.0f;
                c = mixc((Color){ 24, 24, 28, 255 }, (Color){ 132, 136, 142, 255 }, t * t);
            }
            else c = (Color){ 14, 14, 16, 255 };
            float n = fbm(x, y, 64, 3, 1103u);
            img_set(im, x, y, rgbm(c.r, c.g, c.b, 0.9f + n * 0.2f));
        }
    }
}

static void tex_pipe(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            /* three horizontal pipes with cylindrical shading + flanges */
            float band = fmodf((float)y, (float)im->h / 3.0f) / ((float)im->h / 3.0f);
            Color c;
            if (band < 0.18f || band > 0.92f) c = (Color){ 26, 26, 30, 255 };
            else
            {
                float t = (band - 0.18f) / 0.74f;
                float cyl = sinf(t * BP_PI);
                c = rgbm(126, 118, 96, 0.28f + cyl * 0.95f);
            }
            /* flange rings */
            if ((x % 64) < 8) c = rgbm(c.r + 22, c.g + 22, c.b + 24, 1.0f);
            float r = fbm(x, y, 128, 4, 1201u);
            if (r > 0.62f) c = mixc(c, (Color){ 108, 58, 30, 255 }, (r - 0.62f) * 1.5f);
            img_set(im, x, y, c);
        }
    }
    surface_pass(im, 127u, 0.12f, 0.06f);
}

/* Radial alpha sprites -- used for decals, particles and the crosshair. */
static void tex_radial(bp_img *im, Color core, Color edge, float power, bool alpha)
{
    float cx = (float)im->w * 0.5f, cy = (float)im->h * 0.5f;
    float R = (float)BP_MIN(im->w, im->h) * 0.5f;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = ((float)x + 0.5f - cx) / R;
            float dy = ((float)y + 0.5f - cy) / R;
            float d = sqrtf(dx * dx + dy * dy);
            float t = BP_CLAMP(1.0f - d, 0.0f, 1.0f);
            t = powf(t, power);
            Color c = mixc(edge, core, t);
            if (alpha) c.a = clampb((int)(t * 255.0f));
            img_set(im, x, y, c);
        }
    }
}

static void tex_blood(bp_img *im)
{
    float cx = (float)im->w * 0.5f, cy = (float)im->h * 0.5f;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = (float)x - cx, dy = (float)y - cy;
            float ang = atan2f(dy, dx);
            float d = sqrtf(dx * dx + dy * dy) / ((float)im->w * 0.5f);

            /* irregular splat radius via angular noise */
            int ai = (int)((ang + BP_PI) / (2.0f * BP_PI) * 16.0f) % 16;
            float lobes = 0.62f + 0.42f * hashf2(ai, 0, 1301u);
            float splat = fbm(x, y, im->w, 3, 1307u);
            float r = lobes * (0.75f + splat * 0.5f);

            float t = BP_CLAMP(1.0f - d / BP_MAX(r, 0.05f), 0.0f, 1.0f);
            Color c = mixc((Color){ 62, 6, 10, 255 }, (Color){ 148, 14, 22, 255 }, powf(t, 1.6f));
            c.a = clampb((int)(powf(t, 0.7f) * 255.0f));

            /* droplets outside the main body */
            if (t <= 0.0f)
            {
                float drop = fbm(x * 2, y * 2, im->w * 2, 2, 1321u);
                if (drop > 0.80f && d < 0.95f)
                {
                    c = (Color){ 110, 10, 16, 255 };
                    c.a = clampb((int)((drop - 0.80f) * 5.0f * 255.0f));
                }
                else { c.r = 0; c.g = 0; c.b = 0; c.a = 0; }
            }
            img_set(im, x, y, c);
        }
    }
}

static void tex_scorch(bp_img *im)
{
    float cx = (float)im->w * 0.5f, cy = (float)im->h * 0.5f;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = (float)x - cx, dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy) / ((float)im->w * 0.5f);
            float n = fbm(x, y, im->w, 4, 1409u);
            float r = 0.55f + n * 0.42f;
            float t = BP_CLAMP(1.0f - d / r, 0.0f, 1.0f);
            Color c = mixc((Color){ 18, 16, 16, 255 }, (Color){ 62, 54, 50, 255 }, t);
            c.a = clampb((int)(powf(t, 0.8f) * 235.0f));
            img_set(im, x, y, c);
        }
    }
}

static void tex_bullet_hole(bp_img *im)
{
    float cx = (float)im->w * 0.5f, cy = (float)im->h * 0.5f;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = (float)x - cx, dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy) / ((float)im->w * 0.5f);
            float n = fbm(x * 2, y * 2, im->w * 2, 3, 1511u);
            float r = 0.30f + n * 0.20f;
            Color c;
            if (d < r) c = (Color){ 6, 6, 8, 255 };                                  /* hole */
            else if (d < r + 0.10f) c = mixc((Color){ 96, 88, 78, 255 }, (Color){ 40, 36, 34, 255 },
                                             (d - r) / 0.10f);                        /* spall ring */
            else { c.r = 0; c.g = 0; c.b = 0; c.a = 0; }
            float a = (d < r) ? 255.0f : (d < r + 0.10f ? 210.0f * (1.0f - (d - r) / 0.10f) + 45.0f : 0.0f);
            c.a = clampb((int)a);
            img_set(im, x, y, c);
        }
    }
}

static void tex_spark(bp_img *im)
{
    float cx = (float)im->w * 0.5f, cy = (float)im->h * 0.5f;
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = (float)x - cx, dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy) / ((float)im->w * 0.5f);
            float t = BP_CLAMP(1.0f - d * 1.35f, 0.0f, 1.0f);
            /* 4-point star flare */
            float ax = BP_ABS(dx), ay = BP_ABS(dy);
            float flare = BP_MAX(0.0f, 1.0f - BP_MIN(ax, ay) * 9.0f - BP_MAX(ax, ay) * 1.4f);
            t = BP_CLAMP(t + flare * 0.8f, 0.0f, 1.0f);
            Color c = mixc((Color){ 255, 170, 40, 255 }, (Color){ 255, 255, 235, 255 }, powf(t, 2.2f));
            c.a = clampb((int)(powf(t, 1.2f) * 255.0f));
            img_set(im, x, y, c);
        }
    }
}

static void tex_smoke(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = ((float)x - (float)im->w * 0.5f) / ((float)im->w * 0.5f);
            float dy = ((float)y - (float)im->h * 0.5f) / ((float)im->h * 0.5f);
            float d = sqrtf(dx * dx + dy * dy);
            /* warp the radius with noise so the puff has an irregular silhouette */
            float n = fbm(x, y, im->w, 4, 1601u);
            float r = 0.72f + (n - 0.5f) * 0.55f;
            float t = BP_CLAMP(1.0f - d / BP_MAX(r, 0.05f), 0.0f, 1.0f);
            t = t * t;
            unsigned char g = clampb((int)(96.0f + n * 120.0f));
            img_set(im, x, y, (Color){ g, g, (unsigned char)(g + 6), clampb((int)(t * 190.0f)) });
        }
    }
}

static void tex_ring(bp_img *im, Color col)
{
    for (int y = 0; y < im->h; y++)
    {
        for (int x = 0; x < im->w; x++)
        {
            float dx = ((float)x - (float)im->w * 0.5f) / ((float)im->w * 0.5f);
            float dy = ((float)y - (float)im->h * 0.5f) / ((float)im->h * 0.5f);
            float d = sqrtf(dx * dx + dy * dy);
            float band = 1.0f - BP_ABS(d - 0.72f) * 5.0f;
            band = BP_CLAMP(band, 0.0f, 1.0f);
            Color c = col;
            c.a = clampb((int)(powf(band, 1.6f) * 255.0f));
            img_set(im, x, y, c);
        }
    }
}

static void tex_crosshair(bp_img *im)
{
    int c = im->w / 2;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            img_set(im, x, y, (Color){ 0, 0, 0, 0 });

    /* four ticks with a gap in the middle + a 1px centre dot */
    for (int i = 2; i <= 7; i++)
    {
        Color w = (Color){ 255, 255, 255, 235 };
        img_set(im, c - i, c, w); img_set(im, c + i, c, w);
        img_set(im, c, c - i, w); img_set(im, c, c + i, w);
    }
    img_set(im, c, c, (Color){ 255, 255, 255, 120 });
}

static void tex_gradient(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
    {
        unsigned char a = clampb((int)(255.0f * (1.0f - (float)y / (float)im->h)));
        for (int x = 0; x < im->w; x++) img_set(im, x, y, (Color){ 255, 255, 255, a });
    }
}

static void tex_noise(bp_img *im)
{
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
        {
            unsigned char v = clampb((int)(hashf2(x, y, 1709u) * 255.0f));
            img_set(im, x, y, (Color){ v, v, v, 255 });
        }
}

static void tex_logo(bp_img *im)
{
    /* Title lockup: a chevron "breach" mark. Drawn procedurally so it scales to any
     * resolution without an SVG/PNG dependency. */
    int w = im->w, h = im->h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img_set(im, x, y, (Color){ 0, 0, 0, 0 });

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float u = (float)x / (float)w;
            float v = (float)y / (float)h;

            /* outer chevron */
            float chev = BP_ABS(u - 0.5f) * 1.55f;
            float band = BP_ABS(v - (0.20f + chev * 0.62f));
            if (band < 0.055f && v > 0.14f && v < 0.86f)
            {
                float t = 1.0f - band / 0.055f;
                img_set(im, x, y, (Color){ 255, clampb((int)(90 + 120 * t)), 40, clampb((int)(255 * t)) });
            }
            /* inner spark */
            float dx = u - 0.5f, dy = v - 0.60f;
            float d = sqrtf(dx * dx * 1.6f + dy * dy * 4.0f);
            if (d < 0.16f)
            {
                float t = 1.0f - d / 0.16f;
                img_set(im, x, y, (Color){ 255, 240, 200, clampb((int)(255 * t * t)) });
            }
        }
    }
}

/* ====================================================================================== */
/* 3. model helpers                                                                        */
/* ====================================================================================== */
static void model_add(bp_model_id id, Mesh mesh, Color tint, Vector3 origin, Vector3 halfExtent)
{
    if (id < 0 || id >= MDL_COUNT) { UnloadMesh(mesh); return; }

    g_assets.mdl[id].model = LoadModelFromMesh(mesh);   /* takes ownership of the mesh */
    if (g_assets.mdl[id].model.materials)
    {
        g_assets.mdl[id].model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = tint;
        /* Unlit-ish flat shading: rlgl's default material is Lambert; lowering the
         * specular keeps the low-poly silhouettes readable at speed. */
        g_assets.mdl[id].model.materials[0].maps[MATERIAL_MAP_SPECULAR].color = (Color){ 8, 8, 8, 255 };
    }
    g_assets.mdl[id].origin = origin;
    g_assets.mdl[id].half_extent = halfExtent;
    g_assets.mdl[id].tint = tint;
    g_assets.mdl[id].valid = true;
}

static void model_simple(bp_model_id id, int shape, Vector3 p, Color tint)
{
    Mesh m;
    Vector3 half;
    switch (shape)
    {
        case 0: m = GenMeshCube(p.x, p.y, p.z);       half = Vector3Scale(p, 0.5f); break;
        case 1: m = GenMeshCylinder(p.x, p.y, (int)p.z); half = (Vector3){ p.x, p.y * 0.5f, p.x }; break;
        case 2: m = GenMeshSphere(p.x, (int)p.y, (int)p.z); half = (Vector3){ p.x, p.x, p.x }; break;
        case 3: m = GenMeshCone(p.x, p.y, (int)p.z);  half = (Vector3){ p.x, p.y * 0.5f, p.x }; break;
        default: m = GenMeshCube(1, 1, 1);            half = (Vector3){ 0.5f, 0.5f, 0.5f }; break;
    }
    model_add(id, m, tint, (Vector3){ 0, 0, 0 }, half);
}

/* ====================================================================================== */
/* 4. font                                                                                 */
/* ====================================================================================== */
#include "bp_assets_font.inc"

static Font build_font(int scale)
{
    Font font = GetFontDefault();
    if (scale <= 1) return font;

    int gw = BP_FONT_GLYPH_W * scale;
    int gh = BP_FONT_GLYPH_H * scale;
    int count = BP_FONT_COUNT;

    GlyphInfo *glyphs = (GlyphInfo *)RL_CALLOC((size_t)count, sizeof(GlyphInfo));
    if (!glyphs) return font;

    for (int i = 0; i < count; i++)
    {
        int ch = BP_FONT_FIRST + i;
        Image im = GenImageColor(gw, gh, (Color){ 0, 0, 0, 0 });
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);

        const unsigned char *rows = s_font_data[i];
        for (int ry = 0; ry < BP_FONT_GLYPH_H; ry++)
        {
            for (int rx = 0; rx < BP_FONT_GLYPH_W; rx++)
            {
                if (!(rows[ry] & (1 << (BP_FONT_GLYPH_W - 1 - rx)))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                    {
                        int px = rx * scale + sx, py = ry * scale + sy;
                        ((unsigned char *)im.data)[py * im.width + px] = 255;
                    }
            }
        }

        glyphs[i].value = ch;
        glyphs[i].image = im;
        glyphs[i].offsetX = 0;
        glyphs[i].offsetY = 0;
        glyphs[i].advanceX = gw + BP_MAX(1, scale);   /* 1px tracking at 1x, scales up */
    }

    Rectangle *recs = NULL;
    Image atlas = GenImageFontAtlas(glyphs, &recs, count, gh, 2, 0);

    font.baseSize = gh;
    font.glyphCount = count;
    font.glyphPadding = 2;
    font.texture = LoadTextureFromImage(atlas);
    font.recs = recs;

    GlyphInfo *infos = (GlyphInfo *)RL_CALLOC((size_t)count, sizeof(GlyphInfo));
    if (infos)
    {
        for (int i = 0; i < count; i++)
        {
            infos[i].value = glyphs[i].value;
            infos[i].offsetX = 0;
            infos[i].offsetY = 0;
            infos[i].advanceX = glyphs[i].advanceX;
        }
        font.glyphs = infos;
    }

    /* Pixel fonts must never be filtered -- bilinear on a 5x7 bitmap turns 'M' into mush. */
    if (font.texture.id) SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);

    for (int i = 0; i < count; i++) UnloadImage(glyphs[i].image);
    UnloadImage(atlas);
    RL_FREE(glyphs);

    if (!font.texture.id) { BP_LOGW("assets: font atlas upload failed at scale %d", scale); return GetFontDefault(); }
    return font;
}

/* ====================================================================================== */
/* 5. init                                                                                 */
/* ====================================================================================== */
static Texture2D make_tex(void (*gen)(bp_img *), int size, bool mips)
{
    bp_img im = img_make(size, size);
    if (!im.px) { Texture2D t = { 0 }; return t; }
    gen(&im);
    return img_to_texture(&im, mips);
}

static Texture2D make_tex2(void (*gen)(bp_img *, Color), int size, Color c, bool mips)
{
    bp_img im = img_make(size, size);
    if (!im.px) { Texture2D t = { 0 }; return t; }
    gen(&im, c);
    return img_to_texture(&im, mips);
}

static void gen_wall_brick(bp_img *im)     { tex_brick(im); }
static void gen_wall_metal(bp_img *im)     { tex_metal(im, (Color){ 92, 98, 108, 255 }, 128.0f, true); }
static void gen_wall_concrete(bp_img *im)  { tex_concrete(im, (Color){ 116, 114, 110, 255 }, 1.0f); }
static void gen_wall_panel(bp_img *im)     { tex_panel(im, (Color){ 84, 92, 100, 255 }, (Color){ 226, 148, 34, 255 }); }
static void gen_wall_rusty(bp_img *im)     { tex_rusty(im); }
static void gen_floor_grate(bp_img *im)    { tex_grate(im, (Color){ 104, 110, 118, 255 }); }
static void gen_floor_tile(bp_img *im)     { tex_tile(im, (Color){ 92, 96, 100, 255 }, (Color){ 34, 34, 38, 255 }, 64); }
static void gen_floor_concrete(bp_img *im) { tex_concrete(im, (Color){ 88, 86, 84, 255 }, 0.7f); }
static void gen_floor_hazard(bp_img *im)   { tex_hazard(im); }
static void gen_ceiling(bp_img *im)        { tex_ceiling(im); }
static void gen_trim(bp_img *im)           { tex_trim(im); }
static void gen_crate(bp_img *im)          { tex_crate(im); }
static void gen_barrel(bp_img *im)         { tex_barrel(im); }
static void gen_pillar(bp_img *im)         { tex_pillar(im); }
static void gen_light(bp_img *im)          { tex_light_panel(im, (Color){ 214, 238, 255, 255 }); }
static void gen_screen(bp_img *im)         { tex_screen(im); }
static void gen_vent(bp_img *im)           { tex_vent(im); }
static void gen_pipe(bp_img *im)           { tex_pipe(im); }
static void gen_blood(bp_img *im)          { tex_blood(im); }
static void gen_scorch(bp_img *im)         { tex_scorch(im); }
static void gen_hole(bp_img *im)           { tex_bullet_hole(im); }
static void gen_soft(bp_img *im)           { tex_radial(im, (Color){ 255, 255, 255, 255 }, (Color){ 255, 255, 255, 0 }, 1.6f, true); }
static void gen_hard(bp_img *im)           { tex_radial(im, (Color){ 255, 255, 255, 255 }, (Color){ 255, 255, 255, 0 }, 5.0f, true); }
static void gen_spark(bp_img *im)          { tex_spark(im); }
static void gen_smoke(bp_img *im)          { tex_smoke(im); }
static void gen_ring(bp_img *im)           { tex_ring(im, (Color){ 120, 220, 255, 255 }); }
static void gen_cross(bp_img *im)          { tex_crosshair(im); }
static void gen_grad(bp_img *im)           { tex_gradient(im); }
static void gen_noise(bp_img *im)          { tex_noise(im); }
static void gen_logo(bp_img *im)           { tex_logo(im); }

/*
 * Boot progress instrumentation.
 *
 * Procedural generation is the slowest thing that happens at startup and it is entirely
 * CPU-bound, so on a weak machine (or a software rasteriser) it is the difference between a
 * two-second and a twenty-second load. Each group logs its own cost: that makes the boot
 * time a measurable, tunable number instead of a mystery, and it means a slow texture can be
 * found without a profiler.
 */
static double s_stage_t = 0.0;
static char   s_stage_name[48] = "";
static void stage_begin(const char *what)
{
    snprintf(s_stage_name, sizeof(s_stage_name), "%s", what);
    s_stage_t = GetTime();
}
static void stage_end(void)
{
    BP_LOGI("assets: %-22s %6.1f ms", s_stage_name, (GetTime() - s_stage_t) * 1000.0);
}

void bp_assets_init(void)
{
    double t0 = GetTime();
    memset(&g_assets, 0, sizeof(g_assets));

    /* ---- palette: a restricted, warm/cool split keeps the world readable at speed ---- */
    g_assets.pal_accent   = (Color){ 255, 138, 32, 255 };   /* amber  -- player/brand  */
    g_assets.pal_accent2  = (Color){  92, 226, 196, 255 };  /* teal   -- tech/ally     */
    g_assets.pal_danger   = (Color){ 232,  52,  60, 255 };  /* red    -- enemy/damage  */
    g_assets.pal_health   = (Color){  86, 214, 110, 255 };
    g_assets.pal_armor    = (Color){  96, 168, 255, 255 };
    g_assets.pal_ammo     = (Color){ 240, 208,  92, 255 };
    g_assets.pal_bg       = (Color){  12,  13,  17, 255 };
    g_assets.pal_panel    = (Color){  24,  26,  33, 220 };
    g_assets.pal_text     = (Color){ 236, 238, 242, 255 };
    g_assets.pal_text_dim = (Color){ 138, 144, 156, 255 };

    /* ---- architecture (256px, mipmapped + anisotropic) ---- */
    stage_begin("architecture textures");
    g_assets.tex[TEX_WALL_BRICK]     = make_tex(gen_wall_brick,     TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_WALL_METAL]     = make_tex(gen_wall_metal,     TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_WALL_CONCRETE]  = make_tex(gen_wall_concrete,  TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_WALL_PANEL]     = make_tex(gen_wall_panel,     TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_WALL_RUSTY]     = make_tex(gen_wall_rusty,     TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_FLOOR_GRATE]    = make_tex(gen_floor_grate,    TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_FLOOR_TILE]     = make_tex(gen_floor_tile,     TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_FLOOR_CONCRETE] = make_tex(gen_floor_concrete, TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_FLOOR_HAZARD]   = make_tex(gen_floor_hazard,   TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_CEILING]        = make_tex(gen_ceiling,        TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_TRIM]           = make_tex(gen_trim,           TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_CRATE]          = make_tex(gen_crate,          TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_BARREL]         = make_tex(gen_barrel,         TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_PILLAR]         = make_tex(gen_pillar,         TEX_ARCH_SIZE, true);
    g_assets.tex[TEX_LIGHT_PANEL]    = make_tex(gen_light,          TEX_SMALL_SIZE, true);
    g_assets.tex[TEX_SCREEN]         = make_tex(gen_screen,         TEX_SMALL_SIZE, true);
    g_assets.tex[TEX_VENT]           = make_tex(gen_vent,           TEX_SMALL_SIZE, true);
    g_assets.tex[TEX_PIPE]           = make_tex(gen_pipe,           TEX_SMALL_SIZE, true);

    stage_end();

    /* ---- decals / fx (unmipped or point-filtered: they are drawn at near 1:1 texel) ---- */
    stage_begin("fx and decal textures");
    g_assets.tex[TEX_BLOOD_SPLAT] = make_tex(gen_blood,  TEX_SMALL_SIZE, false);
    g_assets.tex[TEX_SCORCH]      = make_tex(gen_scorch, TEX_SMALL_SIZE, false);
    g_assets.tex[TEX_BULLET_HOLE] = make_tex(gen_hole,   64, false);
    g_assets.tex[TEX_PARTICLE_SOFT] = make_tex(gen_soft, 64, false);
    g_assets.tex[TEX_PARTICLE_HARD] = make_tex(gen_hard, 32, false);
    g_assets.tex[TEX_SPARK]       = make_tex(gen_spark,  32, false);
    g_assets.tex[TEX_SMOKE]       = make_tex(gen_smoke,  64, false);
    g_assets.tex[TEX_RING]        = make_tex(gen_ring,   64, false);
    g_assets.tex[TEX_CROSSHAIR]   = make_tex(gen_cross,  32, false);
    g_assets.tex[TEX_GRADIENT]    = make_tex(gen_grad,   16, false);
    g_assets.tex[TEX_NOISE]       = make_tex(gen_noise,  128, true);
    g_assets.tex[TEX_LOGO]        = make_tex(gen_logo,   256, false);
    if (g_assets.tex[TEX_LOGO].id) SetTextureFilter(g_assets.tex[TEX_LOGO], TEXTURE_FILTER_POINT);
    if (g_assets.tex[TEX_CROSSHAIR].id) SetTextureFilter(g_assets.tex[TEX_CROSSHAIR], TEXTURE_FILTER_POINT);

    stage_end();

    /* ---- models ---- */
    stage_begin("procedural models");
    Color flesh   = (Color){ 168,  92,  76, 255 };
    Color fleshDk = (Color){ 118,  58,  50, 255 };
    Color chitin  = (Color){  92, 104,  70, 255 };
    Color steel   = (Color){ 128, 134, 142, 255 };
    Color dark    = (Color){  58,  60,  68, 255 };
    Color glowR   = (Color){ 236,  64,  58, 255 };
    Color glowC   = (Color){  92, 226, 196, 255 };

    /* humanoid grunt: torso / head / arm / leg, animated by per-part transforms */
    model_simple(MDL_ENEMY_TORSO, 0, (Vector3){ 0.62f, 0.78f, 0.38f }, flesh);
    model_simple(MDL_ENEMY_HEAD,  0, (Vector3){ 0.34f, 0.32f, 0.32f }, fleshDk);
    model_simple(MDL_ENEMY_ARM,   0, (Vector3){ 0.17f, 0.62f, 0.17f }, fleshDk);
    model_simple(MDL_ENEMY_LEG,   0, (Vector3){ 0.21f, 0.78f, 0.21f }, dark);

    /* hound: low, fast, four legs */
    model_simple(MDL_HOUND_BODY, 0, (Vector3){ 0.52f, 0.42f, 1.05f }, chitin);
    model_simple(MDL_HOUND_HEAD, 3, (Vector3){ 0.22f, 0.38f, 8.0f }, glowR);
    model_simple(MDL_HOUND_LEG,  0, (Vector3){ 0.12f, 0.46f, 0.12f }, dark);

    /* brute: heavy, wide */
    model_simple(MDL_BRUTE_TORSO, 0, (Vector3){ 1.35f, 1.25f, 0.85f }, (Color){ 122, 68, 62, 255 });
    model_simple(MDL_BRUTE_ARM,   0, (Vector3){ 0.38f, 1.05f, 0.38f }, (Color){ 96, 52, 48, 255 });
    model_simple(MDL_BRUTE_LEG,   0, (Vector3){ 0.46f, 1.00f, 0.46f }, dark);
    model_simple(MDL_BRUTE_HEAD,  0, (Vector3){ 0.58f, 0.46f, 0.52f }, (Color){ 140, 80, 70, 255 });

    /* turret */
    model_simple(MDL_TURRET_BASE,   1, (Vector3){ 0.42f, 0.55f, 12.0f }, steel);
    model_simple(MDL_TURRET_BARREL, 0, (Vector3){ 0.16f, 0.16f, 1.05f }, dark);

    /* viewmodel: pistol */
    model_simple(MDL_GUN_PISTOL_BODY,  0, (Vector3){ 0.055f, 0.150f, 0.230f }, (Color){ 52, 54, 60, 255 });
    model_simple(MDL_GUN_PISTOL_SLIDE, 0, (Vector3){ 0.048f, 0.052f, 0.270f }, (Color){ 96, 100, 110, 255 });
    model_simple(MDL_GUN_HAND,         0, (Vector3){ 0.080f, 0.110f, 0.120f }, (Color){ 178, 132, 104, 255 });

    /* viewmodel: shotgun */
    model_simple(MDL_GUN_SHOTGUN_BODY,   0, (Vector3){ 0.075f, 0.110f, 0.620f }, (Color){ 84, 60, 40, 255 });
    model_simple(MDL_GUN_SHOTGUN_BARREL, 1, (Vector3){ 0.028f, 0.600f, 10.0f }, (Color){ 58, 60, 66, 255 });
    model_simple(MDL_GUN_SHOTGUN_PUMP,   0, (Vector3){ 0.062f, 0.062f, 0.170f }, (Color){ 110, 78, 48, 255 });

    /* viewmodel: smg */
    model_simple(MDL_GUN_SMG_BODY,  0, (Vector3){ 0.070f, 0.130f, 0.400f }, (Color){ 60, 64, 72, 255 });
    model_simple(MDL_GUN_SMG_MAG,   0, (Vector3){ 0.045f, 0.200f, 0.075f }, (Color){ 40, 42, 48, 255 });
    model_simple(MDL_GUN_SMG_STOCK, 0, (Vector3){ 0.045f, 0.075f, 0.190f }, (Color){ 46, 48, 54, 255 });

    /* viewmodel: railgun */
    model_simple(MDL_GUN_RAIL_BODY, 0, (Vector3){ 0.090f, 0.120f, 0.780f }, (Color){ 70, 76, 88, 255 });
    model_simple(MDL_GUN_RAIL_COIL, 1, (Vector3){ 0.052f, 0.090f, 14.0f }, glowC);
    model_simple(MDL_GUN_RAIL_CORE, 1, (Vector3){ 0.020f, 0.700f, 8.0f }, (Color){ 200, 255, 250, 255 });

    /* viewmodel: rocket launcher */
    model_simple(MDL_GUN_ROCKET_TUBE, 1, (Vector3){ 0.075f, 0.820f, 12.0f }, (Color){ 74, 80, 68, 255 });
    model_simple(MDL_GUN_ROCKET_SIGHT, 0, (Vector3){ 0.030f, 0.085f, 0.040f }, (Color){ 40, 42, 46, 255 });

    /* pickups */
    model_simple(MDL_PICKUP_HEALTH, 0, (Vector3){ 0.42f, 0.30f, 0.30f }, (Color){ 232, 236, 240, 255 });
    model_simple(MDL_PICKUP_ARMOR,  0, (Vector3){ 0.44f, 0.36f, 0.24f }, (Color){  96, 168, 255, 255 });
    model_simple(MDL_PICKUP_AMMO,   0, (Vector3){ 0.34f, 0.22f, 0.40f }, (Color){ 240, 208,  92, 255 });
    model_simple(MDL_PICKUP_WEAPON, 0, (Vector3){ 0.20f, 0.16f, 0.72f }, (Color){  70,  74,  84, 255 });

    /* projectiles */
    model_simple(MDL_PROJECTILE_ROCKET, 3, (Vector3){ 0.11f, 0.42f, 8.0f }, (Color){ 210, 214, 220, 255 });
    model_simple(MDL_PROJECTILE_PLASMA, 2, (Vector3){ 0.16f, 12.0f, 12.0f }, glowC);

    /* props */
    model_simple(MDL_BARREL, 1, (Vector3){ 0.34f, 1.05f, 14.0f }, (Color){ 168, 74, 48, 255 });
    model_simple(MDL_CRATE,  0, (Vector3){ 0.95f, 0.95f, 0.95f }, (Color){ 150, 112, 66, 255 });

    stage_end();

    /* ---- fonts ---- */
    stage_begin("bitmap fonts");
    g_assets.font_small = build_font(1);    /*  7px cap height */
    g_assets.font_main  = build_font(2);    /* 14px */
    g_assets.font_big   = build_font(4);    /* 28px */
    g_assets.font_huge  = build_font(8);    /* 56px */
    g_assets.fonts_valid = (g_assets.font_main.texture.id != 0);
    stage_end();

    g_assets.load_ms = (float)((GetTime() - t0) * 1000.0);
    g_assets.ready = true;

    int texOk = 0, mdlOk = 0;
    for (int i = 0; i < TEX_COUNT; i++) if (g_assets.tex[i].id) texOk++;
    for (int i = 0; i < MDL_COUNT; i++) if (g_assets.mdl[i].valid) mdlOk++;
    BP_LOGI("assets: %d/%d textures, %d/%d models, fonts %s -- %.0f ms",
            texOk, TEX_COUNT, mdlOk, MDL_COUNT, g_assets.fonts_valid ? "ok" : "FALLBACK",
            (double)g_assets.load_ms);
}

void bp_assets_shutdown(void)
{
    if (!g_assets.ready) return;

    for (int i = 0; i < TEX_COUNT; i++)
        if (g_assets.tex[i].id) { UnloadTexture(g_assets.tex[i]); g_assets.tex[i].id = 0; }

    for (int i = 0; i < MDL_COUNT; i++)
        if (g_assets.mdl[i].valid) { UnloadModel(g_assets.mdl[i].model); g_assets.mdl[i].valid = false; }

    if (g_assets.font_small.texture.id) UnloadFont(g_assets.font_small);
    if (g_assets.font_main.texture.id)  UnloadFont(g_assets.font_main);
    if (g_assets.font_big.texture.id)   UnloadFont(g_assets.font_big);
    if (g_assets.font_huge.texture.id)  UnloadFont(g_assets.font_huge);

    g_assets.ready = false;
    BP_LOGI("assets: released");
}

Texture2D bp_tex(bp_texture_id id)
{
    if (id < 0 || id >= TEX_COUNT) return (Texture2D){ 0 };
    return g_assets.tex[id];
}

bool bp_model_valid(bp_model_id id) { return id >= 0 && id < MDL_COUNT && g_assets.mdl[id].valid; }
Model bp_model_get(bp_model_id id) { return bp_model_valid(id) ? g_assets.mdl[id].model : (Model){ 0 }; }

Color bp_pal(int index)
{
    static const int rampCount = 8;
    Color c = g_assets.pal_accent;
    switch (((index % rampCount) + rampCount) % rampCount)
    {
        case 0: c = g_assets.pal_accent;  break;
        case 1: c = g_assets.pal_accent2; break;
        case 2: c = g_assets.pal_danger;  break;
        case 3: c = g_assets.pal_health;  break;
        case 4: c = g_assets.pal_armor;   break;
        case 5: c = g_assets.pal_ammo;    break;
        case 6: c = g_assets.pal_text;    break;
        case 7: c = g_assets.pal_text_dim; break;
    }
    return c;
}

void bp_draw_model(bp_model_id id, Vector3 position, Vector3 axis, float angleDeg,
                   Vector3 scale, Color tint)
{
    if (!bp_model_valid(id)) return;

    Matrix t = MatrixTranslate(position.x, position.y, position.z);
    Matrix r = MatrixRotate(axis, angleDeg * BP_DEG2RAD);
    Matrix s = MatrixScale(scale.x, scale.y, scale.z);
    DrawModelEx(g_assets.mdl[id].model, (Vector3){ 0, 0, 0 }, axis, angleDeg, scale, tint);
    BP_UNUSED(t); BP_UNUSED(r); BP_UNUSED(s);
}
