/******************************************************************************************
 * bp_gfx.c
 *
 * The post shader is written in raylib's dual-dialect form: a GLSL 100 body guarded by
 * #ifdef so the *same source string* compiles under OpenGL 3.3 on desktop and OpenGL ES 2.0
 * (WebGL 1.0) in the browser. Shipping one shader instead of two variants is what keeps the
 * Web build from drifting visually from the native build.
 ******************************************************************************************/
#include "bp_gfx.h"

bp_gfx g_gfx = { 0 };

/* -------------------------------------------------------------------------------------- */
/* post-process shader source                                                              */
/* -------------------------------------------------------------------------------------- */
#if defined(PLATFORM_WEB) || defined(GRAPHICS_API_OPENGL_ES2)
    #define BP_GLSL_HEADER "#version 100\n"
    #define BP_PRECISION   "precision mediump float;\n"
#else
    #define BP_GLSL_HEADER "#version 330\n"
    #define BP_PRECISION   ""
#endif

static const char *s_post_vs =
    BP_GLSL_HEADER
    "attribute vec3 vertexPosition;\n"
    "attribute vec2 vertexTexCoord;\n"
    "varying vec2 fragTexCoord;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    gl_Position = vec4(vertexPosition, 1.0);\n"
    "}\n";

/* NOTE: raylib prepends its own default vertex shader uniforms only for the *default*
 * program; a custom shader must declare everything itself. We use an untransformed
 * full-screen quad (rl_load_render_buffer / DrawTexturePro with the shader bound), so the
 * vertex shader above is a passthrough on NDC positions. */

static const char *s_post_fs =
    BP_GLSL_HEADER
    BP_PRECISION
    "varying vec2 fragTexCoord;\n"
    "uniform sampler2D texture0;\n"
    "uniform float uTime;\n"
    "uniform float uDamage;\n"
    "uniform float uSpeed;\n"
    "uniform float uGrain;\n"
    "uniform float uVignette;\n"
    "uniform float uAberration;\n"
    "uniform float uScanline;\n"
    "uniform vec2  uResolution;\n"
    "\n"
    "float hash(vec2 p) {\n"
    "    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 uv = fragTexCoord;\n"
    "    vec2 c = uv - 0.5;\n"
    "    float r2 = dot(c, c);\n"
    "\n"
    "    /* barrel-ish distortion when speed lines are active: sells velocity */\n"
    "    uv += c * r2 * (0.045 * uSpeed);\n"
    "\n"
    "    /* chromatic aberration grows with damage + speed */\n"
    "    float ab = uAberration + 0.0035 * uDamage + 0.0025 * uSpeed;\n"
    "    vec3 col;\n"
    "    col.r = texture2D(texture0, uv + c * ab).r;\n"
    "    col.g = texture2D(texture0, uv).g;\n"
    "    col.b = texture2D(texture0, uv - c * ab).b;\n"
    "\n"
    "    /* damage: red pulse + desaturated periphery */\n"
    "    float lum = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "    col = mix(col, mix(vec3(lum), vec3(0.95, 0.10, 0.10), 0.65), uDamage * (0.35 + 0.65 * r2 * 2.0));\n"
    "\n"
    "    /* speed: slight contrast + cool tint at the edges */\n"
    "    col = mix(col, col * vec3(0.92, 1.02, 1.10), uSpeed * r2 * 1.6);\n"
    "\n"
    "    /* vignette */\n"
    "    col *= 1.0 - uVignette * smoothstep(0.18, 0.78, r2);\n"
    "\n"
    "    /* film grain, animated so it does not crawl */\n"
    "    float g = hash(uv * uResolution + fract(uTime) * 91.7) - 0.5;\n"
    "    col += g * uGrain;\n"
    "\n"
    "    /* scanlines (CRT toggle) */\n"
    "    if (uScanline > 0.001) {\n"
    "        float sl = 0.5 + 0.5 * sin(uv.y * uResolution.y * 3.14159);\n"
    "        col *= 1.0 - uScanline * 0.22 * sl;\n"
    "    }\n"
    "\n"
    "    /* gentle filmic-ish shoulder so bright muzzle flashes roll off instead of clipping */\n"
    "    col = col / (1.0 + col * 0.28);\n"
    "    col = pow(col, vec3(0.94));\n"
    "\n"
    "    gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

/* -------------------------------------------------------------------------------------- */
/* init / resize                                                                           */
/* -------------------------------------------------------------------------------------- */
static void create_target(void)
{
    if (g_gfx.scene.id) UnloadRenderTexture(g_gfx.scene);

    float scale = g_gfx.resolution_scale;
    int w = BP_MAX(160, (int)((float)GetScreenWidth() * scale));
    int h = BP_MAX(120, (int)((float)GetScreenHeight() * scale));

    g_gfx.scene = LoadRenderTexture(w, h);
    g_gfx.scene_w = w;
    g_gfx.scene_h = h;

    if (g_gfx.scene.texture.id)
    {
        /* Nearest filtering at reduced internal resolution is what makes the "retro
         * resolution" mode look intentional rather than blurry. */
        SetTextureFilter(g_gfx.scene.texture,
                         scale < 0.999f ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
    }
    BP_LOGI("gfx: render target %dx%d (scale %.2f)", w, h, (double)scale);
}

void bp_gfx_init(void)
{
    memset(&g_gfx, 0, sizeof(g_gfx));
    g_gfx.quality = BP_QUALITY_HIGH;
    g_gfx.resolution_scale = 1.0f;
    g_gfx.post_enabled = true;
    g_gfx.crt_enabled = false;
    g_gfx.fps_smooth = 60;

    g_gfx.post = LoadShaderFromMemory(s_post_vs, s_post_fs);
    g_gfx.post_valid = (g_gfx.post.id != 0);
    if (g_gfx.post_valid)
    {
        g_gfx.loc_time       = GetShaderLocation(g_gfx.post, "uTime");
        g_gfx.loc_damage     = GetShaderLocation(g_gfx.post, "uDamage");
        g_gfx.loc_speed      = GetShaderLocation(g_gfx.post, "uSpeed");
        g_gfx.loc_grain      = GetShaderLocation(g_gfx.post, "uGrain");
        g_gfx.loc_vignette   = GetShaderLocation(g_gfx.post, "uVignette");
        g_gfx.loc_aberration = GetShaderLocation(g_gfx.post, "uAberration");
        g_gfx.loc_scanline   = GetShaderLocation(g_gfx.post, "uScanline");
        g_gfx.loc_resolution = GetShaderLocation(g_gfx.post, "uResolution");

        float grain = 0.035f, vig = 0.55f, ab = 0.0012f, sl = 0.0f;
        SetShaderValue(g_gfx.post, g_gfx.loc_grain, &grain, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_gfx.post, g_gfx.loc_vignette, &vig, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_gfx.post, g_gfx.loc_aberration, &ab, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_gfx.post, g_gfx.loc_scanline, &sl, SHADER_UNIFORM_FLOAT);
        BP_LOGI("gfx: post shader compiled");
    }
    else BP_LOGW("gfx: post shader failed to compile -- rendering without post FX");

    create_target();
}

void bp_gfx_shutdown(void)
{
    if (g_gfx.post_valid) { UnloadShader(g_gfx.post); g_gfx.post_valid = false; }
    if (g_gfx.scene.id)   { UnloadRenderTexture(g_gfx.scene); g_gfx.scene.id = 0; }
}

const char *bp_gfx_quality_name(bp_quality q)
{
    switch (q)
    {
        case BP_QUALITY_LOW:    return "LOW (0.5x)";
        case BP_QUALITY_MEDIUM: return "MEDIUM (0.75x)";
        case BP_QUALITY_HIGH:   return "HIGH (1.0x)";
        case BP_QUALITY_ULTRA:  return "ULTRA (1.25x SS)";
        default:                return "UNKNOWN";
    }
}

void bp_gfx_set_quality(bp_quality q)
{
    g_gfx.quality = q;
    switch (q)
    {
        case BP_QUALITY_LOW:
            g_gfx.resolution_scale = 0.50f; g_gfx.post_enabled = false; break;
        case BP_QUALITY_MEDIUM:
            g_gfx.resolution_scale = 0.75f; g_gfx.post_enabled = true;  break;
        case BP_QUALITY_HIGH:
            g_gfx.resolution_scale = 1.00f; g_gfx.post_enabled = true;  break;
        case BP_QUALITY_ULTRA:
            g_gfx.resolution_scale = 1.25f; g_gfx.post_enabled = true;  break;
        default: break;
    }
    create_target();
    BP_LOGI("gfx: quality -> %s", bp_gfx_quality_name(q));
}

void bp_gfx_resize(void)
{
    create_target();
}

void bp_gfx_flash_damage(float amount)
{
    g_gfx.damage_flash = BP_CLAMP(g_gfx.damage_flash + amount, 0.0f, 1.0f);
}

void bp_gfx_flash_heal(float amount)
{
    g_gfx.heal_flash = BP_CLAMP(g_gfx.heal_flash + amount, 0.0f, 1.0f);
}

/* -------------------------------------------------------------------------------------- */
/* frame                                                                                   */
/* -------------------------------------------------------------------------------------- */
void bp_gfx_begin_scene(void)
{
    if (!g_gfx.scene.id) return;
    BeginTextureMode(g_gfx.scene);
    ClearBackground((Color){ 6, 7, 10, 255 });
}

void bp_gfx_end_scene(void)
{
    EndTextureMode();

    /* decay gameplay-driven flashes */
    float dt = GetFrameTime();
    g_gfx.damage_flash = bp_damp(g_gfx.damage_flash, 0.0f, 6.0f, dt);
    g_gfx.heal_flash   = bp_damp(g_gfx.heal_flash,   0.0f, 5.0f, dt);
    g_gfx.speed_lines  = bp_damp(g_gfx.speed_lines,  0.0f, 3.0f, dt);
    g_gfx.time += dt;

    BeginDrawing();

    Rectangle src = { 0.0f, 0.0f, (float)g_gfx.scene_w, -(float)g_gfx.scene_h };
    Rectangle dst = { 0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight() };

    if (g_gfx.post_valid && g_gfx.post_enabled)
    {
        SetShaderValue(g_gfx.post, g_gfx.loc_time,   &g_gfx.time, SHADER_UNIFORM_FLOAT);
        SetShaderValue(g_gfx.post, g_gfx.loc_damage, &g_gfx.damage_flash, SHADER_UNIFORM_FLOAT);

        float spd = BP_CLAMP(g_gfx.speed_lines, 0.0f, 1.0f);
        SetShaderValue(g_gfx.post, g_gfx.loc_speed, &spd, SHADER_UNIFORM_FLOAT);

        float sl = g_gfx.crt_enabled ? 1.0f : 0.0f;
        SetShaderValue(g_gfx.post, g_gfx.loc_scanline, &sl, SHADER_UNIFORM_FLOAT);

        float res[2] = { (float)g_gfx.scene_w, (float)g_gfx.scene_h };
        SetShaderValue(g_gfx.post, g_gfx.loc_resolution, res, SHADER_UNIFORM_VEC2);

        BeginShaderMode(g_gfx.post);
        DrawTexturePro(g_gfx.scene.texture, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);
        EndShaderMode();
    }
    else DrawTexturePro(g_gfx.scene.texture, src, dst, (Vector2){ 0, 0 }, 0.0f, WHITE);

    /* heal flash is additive and cheap: one tinted quad over everything */
    if (g_gfx.heal_flash > 0.01f)
    {
        Color c = (Color){ 90, 220, 130, (unsigned char)(g_gfx.heal_flash * 60.0f) };
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), c);
    }

    /* g_gfx.fps_smooth is fed by the caller so the HUD and the overlay agree */
    g_gfx.fps_smooth = (int)(g_gfx.fps_smooth * 0.92f + (float)GetFPS() * 0.08f);
}

/* -------------------------------------------------------------------------------------- */
/* debug primitives                                                                        */
/* -------------------------------------------------------------------------------------- */
void bp_gfx_draw_aabb(Vector3 min, Vector3 max, Color c)
{
    Vector3 s = Vector3Subtract(max, min);
    Vector3 centre = Vector3Add(min, Vector3Scale(s, 0.5f));
    DrawCubeWires(centre, s.x, s.y, s.z, c);
}

void bp_gfx_draw_ray(Vector3 from, Vector3 to, Color c) { DrawLine3D(from, to, c); }

void bp_gfx_draw_grid(Vector3 center, float size, float step, Color c)
{
    int n = (int)(size / step);
    for (int i = -n; i <= n; i++)
    {
        Vector3 a = { center.x + (float)i * step, center.y, center.z - size };
        Vector3 b = { center.x + (float)i * step, center.y, center.z + size };
        DrawLine3D(a, b, c);
        a = (Vector3){ center.x - size, center.y, center.z + (float)i * step };
        b = (Vector3){ center.x + size, center.y, center.z + (float)i * step };
        DrawLine3D(a, b, c);
    }
}
