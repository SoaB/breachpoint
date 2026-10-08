/******************************************************************************************
 * bp_audio.c -- procedural DSP: one-shot SFX bank + a live dynamic-music synthesiser.
 *
 * The music is generated *inside raylib's audio-thread callback*, so the soundtrack reacts
 * to gameplay with zero latency and zero downloaded assets. The game thread publishes four
 * layer weights (pad / bass / drums / lead) every frame; the audio thread smooths them and
 * mixes accordingly. Because the transport never stops, entering combat is a *fade*, not a
 * seek -- no musical seam, which is the most common flaw in dynamic game audio.
 ******************************************************************************************/
#include "bp_audio.h"

bp_audio_sys g_audio = { 0 };

/* ====================================================================================== */
/* 0. constants                                                                            */
/* ====================================================================================== */
#define SFX_SAMPLE_RATE   44100
#define SFX_MAX_SECONDS   2.5f
#define SFX_SCRATCH       ((int)(SFX_SAMPLE_RATE * SFX_MAX_SECONDS))
#define SFX_VOICES_PER_ID 3            /* alias pool: rapid fire must not cut itself off  */

typedef struct {
    Sound  voices[SFX_VOICES_PER_ID];
    bool   loaded;
    double last_used[SFX_VOICES_PER_ID];
} bp_sfx_bank;

static bp_sfx_bank s_bank[SFX_COUNT];
static float      *s_scratch = NULL;   /* alive only during init */

/* ====================================================================================== */
/* 1. DSP primitives                                                                       */
/* ====================================================================================== */
typedef struct { float lp; } bp_onepole;

static uint32_t s_noise_state = 0x12345678u;

static inline float dsp_noise(void)
{
    uint32_t x = s_noise_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_noise_state = x;
    return ((float)(x & 0xFFFF) / 32767.5f) - 1.0f;
}

static inline float dsp_env_perc(float t, float attack, float decay)
{
    if (t < 0.0f) return 0.0f;
    if (t < attack) return attack > 0.0f ? t / attack : 1.0f;
    float d = t - attack;
    if (d > decay) return 0.0f;
    float x = d / decay;
    return (1.0f - x) * (1.0f - x);          /* squared ramp ~ natural acoustic decay */
}

static inline float dsp_env_ad(float t, float attack, float decay)
{
    if (t < 0.0f) return 0.0f;
    if (t < attack) return attack > 0.0f ? t / attack : 1.0f;
    float d = t - attack;
    if (d > decay) return 0.0f;
    return expf(-4.0f * d / decay);
}

static inline float dsp_lowpass(bp_onepole *s, float in, float cutoffHz, float dt)
{
    float rc = 1.0f / (2.0f * BP_PI * BP_MAX(cutoffHz, 10.0f));
    float a = dt / (rc + dt);
    s->lp += a * (in - s->lp);
    return s->lp;
}

static inline float dsp_highpass(bp_onepole *s, float in, float cutoffHz, float dt)
{
    return in - dsp_lowpass(s, in, cutoffHz, dt);
}

static inline float dsp_saturate(float x, float drive)
{
    float y = x * drive;
    return y / (1.0f + BP_ABS(y));           /* cheap tanh-like soft clip */
}

static void dsp_mix_at(float *dst, int dstLen, int offset, const float *src, int srcLen, float vol)
{
    if (offset < 0) { srcLen += offset; src -= offset; offset = 0; }
    if (srcLen <= 0) return;
    int n = BP_MIN(dstLen - offset, srcLen);
    for (int i = 0; i < n; i++) dst[offset + i] += src[i] * vol;
}

/* ====================================================================================== */
/* 2. instrument generators                                                                */
/* ====================================================================================== */

/* Gunshot core: noise burst through a downward-sweeping band, a transient "crack", and a
 * low sine thump for weight. Every weapon in the bank is a parameterisation of this. */
static void gen_gunshot(float *buf, int len, float sr,
                        float bodyStartHz, float bodyEndHz, float bodyLen,
                        float crackHz, float crackLen,
                        float thumpHz, float thumpLen,
                        float noiseAmt, float crackAmt, float thumpAmt)
{
    bp_onepole hp = { 0 };
    float dt = 1.0f / sr;
    float pCrack = 0.0f, pThump = 0.0f;

    for (int i = 0; i < len; i++)
    {
        float t = (float)i * dt;
        float v = 0.0f;

        if (t < bodyLen)
        {
            float k = t / bodyLen;
            float cutoff = bodyStartHz + (bodyEndHz - bodyStartHz) * k;
            float env = dsp_env_perc(t, 0.0008f, bodyLen);
            v += dsp_highpass(&hp, dsp_noise(), 190.0f, dt) * env * noiseAmt;
            v += sinf(2.0f * BP_PI * cutoff * t) * env * 0.13f * noiseAmt;
        }
        if (t < crackLen)
        {
            float env = dsp_env_perc(t, 0.0002f, crackLen);
            pCrack += crackHz * dt * (1.0f - 0.5f * t / crackLen);
            v += sinf(2.0f * BP_PI * pCrack) * env * crackAmt;
            v += dsp_noise() * env * crackAmt * 0.55f;
        }
        if (t < thumpLen)
        {
            float env = dsp_env_perc(t, 0.001f, thumpLen);
            pThump += thumpHz * (1.0f - 0.6f * t / thumpLen) * dt;
            v += sinf(2.0f * BP_PI * pThump) * env * thumpAmt;
        }
        buf[i] += v;
    }
}

static void gen_explosion(float *buf, int len, float sr, float size)
{
    bp_onepole lp = { 0 };
    float dt = 1.0f / sr;
    float dur = 0.35f + 0.55f * size;
    float pSub = 0.0f;

    for (int i = 0; i < len; i++)
    {
        float t = (float)i * dt;
        float v = 0.0f;

        if (t < dur)
        {
            float k = t / dur;
            float cutoff = 2600.0f * (1.0f - k) * (1.0f - k) + 90.0f;
            float env = (1.0f - k) * (1.0f - k);
            v += dsp_lowpass(&lp, dsp_noise(), cutoff, dt) * env * 1.35f;
        }
        if (t < 0.30f)
        {
            float env = dsp_env_perc(t, 0.002f, 0.30f);
            pSub += 72.0f * (1.0f - 0.7f * t / 0.30f) * dt;
            v += sinf(2.0f * BP_PI * pSub) * env * (0.95f + 0.5f * size);
        }
        if (t > 0.14f && t < dur + 0.45f)      /* debris tail */
        {
            float k = (t - 0.14f) / (dur + 0.31f);
            v += dsp_noise() * (1.0f - BP_CLAMP(k, 0.0f, 1.0f)) * 0.055f;
        }
        buf[i] += v;
    }
}

/* waveType: 0 sine, 1 square, 2 saw, 3 triangle */
static void gen_tone(float *buf, int len, float sr, float f0, float f1,
                     float vol, int waveType, float attack, float decay)
{
    float dt = 1.0f / sr, phase = 0.0f;
    for (int i = 0; i < len; i++)
    {
        float t = (float)i * dt;
        if (t > attack + decay) break;
        float k = BP_CLAMP(t / BP_MAX(decay, 0.0001f), 0.0f, 1.0f);
        phase += (f0 + (f1 - f0) * k) * dt;
        float ph = fmodf(phase, 1.0f);
        float s;
        switch (waveType)
        {
            case 1:  s = (ph < 0.5f) ? 1.0f : -1.0f; break;
            case 2:  s = 2.0f * ph - 1.0f; break;
            case 3:  s = fabsf(ph * 2.0f - 1.0f) * 2.0f - 1.0f; break;
            default: s = sinf(2.0f * BP_PI * phase); break;
        }
        buf[i] += s * dsp_env_ad(t, attack, decay) * vol;
    }
}

static void gen_noise(float *buf, int len, float sr, float cutoffHz, float vol,
                      float attack, float decay, bool bandpass)
{
    bp_onepole a = { 0 }, b = { 0 };
    float dt = 1.0f / sr;
    for (int i = 0; i < len; i++)
    {
        float t = (float)i * dt;
        if (t > attack + decay) break;
        float n = dsp_noise();
        float s;
        if (bandpass)
        {
            s = dsp_highpass(&a, n, cutoffHz * 0.55f, dt);
            s = dsp_lowpass(&b, s, cutoffHz * 1.9f, dt);
        }
        else s = dsp_lowpass(&a, n, cutoffHz, dt);
        buf[i] += s * dsp_env_ad(t, attack, decay) * vol;
    }
}

/* Creature voice: detuned saw pair through a downward-sweeping low pass, tremolo'd for a
 * "throat" quality, then saturated. */
static void gen_growl(float *buf, int len, float sr, float baseHz, float dur, float vol, float sweep)
{
    bp_onepole lp = { 0 };
    float dt = 1.0f / sr, p1 = 0.0f, p2 = 0.0f;
    for (int i = 0; i < len; i++)
    {
        float t = (float)i * dt;
        if (t > dur) break;
        float k = t / dur;
        float hz = baseHz * (1.0f + sweep * k);
        p1 += hz * dt;
        p2 += hz * 1.007f * dt;
        float saw = (2.0f * fmodf(p1, 1.0f) - 1.0f) * 0.6f + (2.0f * fmodf(p2, 1.0f) - 1.0f) * 0.4f;
        float formant = dsp_lowpass(&lp, saw, 340.0f + 900.0f * (1.0f - k), dt);
        float am = 0.65f + 0.35f * sinf(2.0f * BP_PI * (18.0f + 26.0f * k) * t);
        float env = sinf(BP_PI * k);
        buf[i] += dsp_saturate(formant * am * env, 3.0f) * vol;
    }
}

static void gen_blip(float *buf, int len, float sr, float hz, float vol)
{
    gen_tone(buf, len, sr, hz, hz * 0.98f, vol, 1, 0.002f, 0.055f);
    gen_tone(buf, len, sr, hz * 2.0f, hz * 1.9f, vol * 0.25f, 0, 0.002f, 0.040f);
}

static void normalise(float *buf, int len, float peak)
{
    float m = 0.0f;
    for (int i = 0; i < len; i++) { float a = BP_ABS(buf[i]); if (a > m) m = a; }
    if (m < 1e-6f) return;
    float g = peak / m;
    for (int i = 0; i < len; i++) buf[i] *= g;
}

/* ====================================================================================== */
/* 3. SFX recipes                                                                          */
/* ====================================================================================== */
#define GEN(name) static void name(float *buf, int len, float sr)

GEN(g_pistol)    { gen_gunshot(buf, len, sr, 2600, 700, 0.10f, 1500, 0.030f, 150, 0.10f, 0.85f, 0.75f, 0.55f); }
GEN(g_pistolDry) { gen_noise(buf, len, sr, 3200, 0.35f, 0.001f, 0.035f, true);
                   gen_tone(buf, len, sr, 900, 400, 0.18f, 1, 0.001f, 0.030f); }
GEN(g_shotgun)   { gen_gunshot(buf, len, sr, 1800, 320, 0.22f, 900, 0.045f, 95, 0.24f, 1.0f, 0.90f, 1.0f);
                   gen_noise(buf, len, sr, 4200, 0.35f, 0.001f, 0.090f, true); }
GEN(g_pump)      { float tmp[4096]; memset(tmp, 0, sizeof(tmp));
                   gen_noise(buf, len, sr, 1500, 0.50f, 0.002f, 0.050f, true);
                   gen_tone(buf, len, sr, 260, 180, 0.22f, 1, 0.002f, 0.070f);
                   memset(tmp, 0, sizeof(tmp));
                   gen_noise(tmp, 4096, sr, 2300, 0.45f, 0.002f, 0.045f, true);
                   dsp_mix_at(buf, len, (int)(0.155f * sr), tmp, 4096, 1.0f); }
GEN(g_smg)       { gen_gunshot(buf, len, sr, 3200, 900, 0.065f, 1900, 0.020f, 190, 0.06f, 0.70f, 0.85f, 0.35f); }
GEN(g_railChg)   { gen_tone(buf, len, sr, 180, 1500, 0.30f, 2, 0.050f, 0.550f);
                   gen_tone(buf, len, sr,  90,  750, 0.18f, 0, 0.050f, 0.550f); }
GEN(g_railFire)  { gen_gunshot(buf, len, sr, 5200, 900, 0.30f, 2600, 0.070f, 60, 0.35f, 0.75f, 1.0f, 1.10f);
                   gen_tone(buf, len, sr, 1200, 120, 0.42f, 2, 0.002f, 0.420f); }
GEN(g_rocket)    { gen_gunshot(buf, len, sr, 900, 260, 0.18f, 420, 0.050f, 70, 0.28f, 0.90f, 0.50f, 1.20f);
                   gen_noise(buf, len, sr, 700, 0.30f, 0.020f, 0.550f, false); }
GEN(g_reloadOut) { gen_noise(buf, len, sr, 2400, 0.40f, 0.002f, 0.050f, true);
                   gen_tone(buf, len, sr, 420, 260, 0.20f, 1, 0.002f, 0.050f); }
GEN(g_reloadIn)  { gen_noise(buf, len, sr, 1800, 0.45f, 0.002f, 0.060f, true);
                   gen_tone(buf, len, sr, 240, 520, 0.24f, 1, 0.002f, 0.060f); }
GEN(g_reloadEnd) { gen_tone(buf, len, sr, 620, 900, 0.22f, 1, 0.002f, 0.070f);
                   gen_noise(buf, len, sr, 3000, 0.25f, 0.001f, 0.040f, true); }
GEN(g_swap)      { gen_noise(buf, len, sr, 1200, 0.35f, 0.003f, 0.100f, true);
                   gen_tone(buf, len, sr, 300, 520, 0.14f, 1, 0.003f, 0.100f); }

GEN(g_hitFlesh)  { gen_noise(buf, len, sr, 700, 0.85f, 0.001f, 0.100f, false);
                   gen_tone(buf, len, sr, 180, 90, 0.35f, 0, 0.001f, 0.090f); }
GEN(g_hitArmor)  { gen_noise(buf, len, sr, 4200, 0.60f, 0.001f, 0.070f, true);
                   gen_tone(buf, len, sr, 1800, 900, 0.30f, 1, 0.001f, 0.060f); }
GEN(g_hitWall)   { gen_noise(buf, len, sr, 2600, 0.55f, 0.001f, 0.060f, true);
                   gen_tone(buf, len, sr, 480, 220, 0.20f, 0, 0.001f, 0.070f); }
GEN(g_ricochet)  { gen_tone(buf, len, sr, 3400, 900, 0.30f, 0, 0.001f, 0.300f);
                   gen_noise(buf, len, sr, 5200, 0.25f, 0.001f, 0.050f, true); }
GEN(g_explode)   { gen_explosion(buf, len, sr, 1.00f); }
GEN(g_explodeD)  { gen_explosion(buf, len, sr, 0.45f); }

GEN(g_gruntSee)  { gen_growl(buf, len, sr, 150, 0.42f, 0.55f,  0.50f); }
GEN(g_gruntPain) { gen_growl(buf, len, sr, 210, 0.22f, 0.55f, -0.25f); }
GEN(g_gruntDie)  { gen_growl(buf, len, sr, 130, 0.70f, 0.70f, -0.55f);
                   gen_noise(buf, len, sr, 600, 0.30f, 0.010f, 0.550f, false); }
GEN(g_houndSee)  { gen_growl(buf, len, sr, 320, 0.30f, 0.55f,  0.80f);
                   gen_noise(buf, len, sr, 3000, 0.22f, 0.001f, 0.180f, true); }
GEN(g_houndAtk)  { gen_noise(buf, len, sr, 2600, 0.55f, 0.002f, 0.120f, true);
                   gen_growl(buf, len, sr, 400, 0.18f, 0.40f, -0.30f); }
GEN(g_houndDie)  { gen_growl(buf, len, sr, 260, 0.50f, 0.65f, -0.60f); }
GEN(g_turretFire){ gen_gunshot(buf, len, sr, 3600, 1100, 0.060f, 2100, 0.018f, 210, 0.05f, 0.60f, 0.70f, 0.25f); }
GEN(g_turretDie) { gen_explosion(buf, len, sr, 0.60f);
                   gen_tone(buf, len, sr, 900, 80, 0.30f, 2, 0.005f, 0.600f); }
GEN(g_bruteRoar) { gen_growl(buf, len, sr, 82, 0.95f, 0.90f, 0.35f);
                   gen_noise(buf, len, sr, 400, 0.35f, 0.050f, 0.900f, false); }
GEN(g_bruteDie)  { gen_growl(buf, len, sr, 70, 1.30f, 0.95f, -0.45f);
                   gen_explosion(buf, len, sr, 0.80f); }

GEN(g_jump)      { gen_noise(buf, len, sr, 900, 0.30f, 0.004f, 0.100f, false); }
GEN(g_land)      { gen_noise(buf, len, sr, 500, 0.55f, 0.001f, 0.130f, false);
                   gen_tone(buf, len, sr, 120, 60, 0.30f, 0, 0.001f, 0.120f); }
GEN(g_step)      { gen_noise(buf, len, sr, 1400, 0.30f, 0.001f, 0.055f, true);
                   gen_tone(buf, len, sr, 150, 100, 0.14f, 0, 0.001f, 0.050f); }
GEN(g_hurt)      { gen_growl(buf, len, sr, 190, 0.28f, 0.55f, -0.20f);
                   gen_noise(buf, len, sr, 900, 0.30f, 0.001f, 0.200f, false); }
GEN(g_heal)      { gen_tone(buf, len, sr, 420,  880, 0.30f, 0, 0.010f, 0.280f);
                   gen_tone(buf, len, sr, 630, 1320, 0.16f, 0, 0.020f, 0.260f); }
GEN(g_armor)     { gen_tone(buf, len, sr, 300, 600, 0.30f, 1, 0.005f, 0.220f);
                   gen_noise(buf, len, sr, 3000, 0.22f, 0.002f, 0.160f, true); }
GEN(g_ammo)      { gen_tone(buf, len, sr, 700, 1050, 0.24f, 1, 0.003f, 0.100f);
                   gen_noise(buf, len, sr, 2600, 0.20f, 0.002f, 0.070f, true); }
GEN(g_lowHp)     { gen_tone(buf, len, sr, 110, 96, 0.42f, 0, 0.050f, 0.550f); }

GEN(g_uiMove)    { gen_blip(buf, len, sr, 620, 0.22f); }
GEN(g_uiSelect)  { gen_blip(buf, len, sr, 880, 0.30f);
                   gen_tone(buf, len, sr, 1320, 1320, 0.16f, 0, 0.005f, 0.120f); }
GEN(g_uiBack)    { gen_blip(buf, len, sr, 380, 0.24f); }
GEN(g_waveStart) { gen_tone(buf, len, sr, 180, 540, 0.34f, 2, 0.020f, 0.600f);
                   gen_tone(buf, len, sr,  90, 270, 0.30f, 0, 0.020f, 0.700f);
                   gen_noise(buf, len, sr, 1200, 0.16f, 0.050f, 0.700f, false); }
GEN(g_waveClear) { gen_tone(buf, len, sr,  523,  523, 0.26f, 0, 0.010f, 0.300f);
                   gen_tone(buf, len, sr,  659,  659, 0.24f, 0, 0.100f, 0.300f);
                   gen_tone(buf, len, sr,  784,  784, 0.24f, 0, 0.200f, 0.450f);
                   gen_tone(buf, len, sr, 1047, 1047, 0.20f, 0, 0.300f, 0.600f); }
GEN(g_door)      { gen_noise(buf, len, sr, 420, 0.55f, 0.050f, 0.700f, false);
                   gen_tone(buf, len, sr, 70, 55, 0.40f, 0, 0.050f, 0.800f); }
GEN(g_secret)    { gen_tone(buf, len, sr, 392, 392, 0.24f, 0, 0.010f, 0.250f);
                   gen_tone(buf, len, sr, 523, 523, 0.24f, 0, 0.120f, 0.250f);
                   gen_tone(buf, len, sr, 659, 659, 0.24f, 0, 0.240f, 0.250f);
                   gen_tone(buf, len, sr, 880, 880, 0.26f, 0, 0.360f, 0.600f); }

typedef struct {
    const char *name;
    float duration;
    float volume;
    float pitchMin, pitchMax;
    float falloff;
    float cooldown;
    void (*generate)(float *buf, int len, float sr);
} bp_sfx_recipe;

static const bp_sfx_recipe s_recipes[SFX_COUNT] = {
    [SFX_PISTOL_FIRE]       = { "pistol_fire",   0.30f, 0.85f, 0.94f, 1.07f,  90.0f, 0.030f, g_pistol },
    [SFX_PISTOL_DRY]        = { "pistol_dry",    0.12f, 0.55f, 0.97f, 1.03f,  30.0f, 0.150f, g_pistolDry },
    [SFX_SHOTGUN_FIRE]      = { "shotgun_fire",  0.55f, 1.00f, 0.93f, 1.06f, 110.0f, 0.030f, g_shotgun },
    [SFX_SHOTGUN_PUMP]      = { "shotgun_pump",  0.40f, 0.60f, 0.96f, 1.05f,  40.0f, 0.050f, g_pump },
    [SFX_SMG_FIRE]          = { "smg_fire",      0.18f, 0.62f, 0.92f, 1.09f,  80.0f, 0.020f, g_smg },
    [SFX_RAIL_CHARGE]       = { "rail_charge",   0.70f, 0.55f, 1.00f, 1.00f,  50.0f, 0.400f, g_railChg },
    [SFX_RAIL_FIRE]         = { "rail_fire",     0.80f, 1.00f, 0.96f, 1.04f, 150.0f, 0.030f, g_railFire },
    [SFX_ROCKET_FIRE]       = { "rocket_fire",   0.80f, 0.95f, 0.95f, 1.05f, 110.0f, 0.030f, g_rocket },
    [SFX_RELOAD_OUT]        = { "reload_out",    0.18f, 0.50f, 0.97f, 1.04f,  30.0f, 0.020f, g_reloadOut },
    [SFX_RELOAD_IN]         = { "reload_in",     0.20f, 0.55f, 0.97f, 1.04f,  30.0f, 0.020f, g_reloadIn },
    [SFX_RELOAD_DONE]       = { "reload_done",   0.20f, 0.55f, 0.98f, 1.03f,  30.0f, 0.020f, g_reloadEnd },
    [SFX_WEAPON_SWAP]       = { "weapon_swap",   0.24f, 0.50f, 0.96f, 1.05f,  30.0f, 0.030f, g_swap },

    [SFX_HIT_FLESH]         = { "hit_flesh",     0.22f, 0.75f, 0.92f, 1.10f,  45.0f, 0.000f, g_hitFlesh },
    [SFX_HIT_ARMOR]         = { "hit_armor",     0.18f, 0.70f, 0.94f, 1.08f,  45.0f, 0.000f, g_hitArmor },
    [SFX_HIT_WALL]          = { "hit_wall",      0.18f, 0.45f, 0.90f, 1.12f,  55.0f, 0.010f, g_hitWall },
    [SFX_RICOCHET]          = { "ricochet",      0.42f, 0.40f, 0.90f, 1.15f,  70.0f, 0.030f, g_ricochet },
    [SFX_EXPLOSION]         = { "explosion",     1.10f, 1.00f, 0.92f, 1.06f, 160.0f, 0.020f, g_explode },
    [SFX_EXPLOSION_DISTANT] = { "explosion_far", 1.10f, 0.55f, 0.90f, 1.05f, 400.0f, 0.020f, g_explodeD },

    [SFX_GRUNT_SIGHT]       = { "grunt_sight",   0.55f, 0.70f, 0.94f, 1.08f,  60.0f, 0.300f, g_gruntSee },
    [SFX_GRUNT_PAIN]        = { "grunt_pain",    0.30f, 0.55f, 0.92f, 1.10f,  45.0f, 0.060f, g_gruntPain },
    [SFX_GRUNT_DIE]         = { "grunt_die",     0.90f, 0.80f, 0.92f, 1.08f,  60.0f, 0.020f, g_gruntDie },
    [SFX_HOUND_SIGHT]       = { "hound_sight",   0.40f, 0.65f, 0.94f, 1.08f,  55.0f, 0.250f, g_houndSee },
    [SFX_HOUND_ATTACK]      = { "hound_attack",  0.25f, 0.70f, 0.93f, 1.09f,  40.0f, 0.100f, g_houndAtk },
    [SFX_HOUND_DIE]         = { "hound_die",     0.65f, 0.75f, 0.92f, 1.08f,  50.0f, 0.020f, g_houndDie },
    [SFX_TURRET_FIRE]       = { "turret_fire",   0.15f, 0.50f, 0.93f, 1.08f,  70.0f, 0.020f, g_turretFire },
    [SFX_TURRET_DIE]        = { "turret_die",    0.80f, 0.80f, 0.94f, 1.06f,  80.0f, 0.020f, g_turretDie },
    [SFX_BRUTE_ROAR]        = { "brute_roar",    1.20f, 0.90f, 0.95f, 1.05f, 100.0f, 0.600f, g_bruteRoar },
    [SFX_BRUTE_DIE]         = { "brute_die",     1.60f, 1.00f, 0.95f, 1.05f, 110.0f, 0.020f, g_bruteDie },

    [SFX_JUMP]              = { "jump",          0.18f, 0.28f, 0.94f, 1.08f,  25.0f, 0.050f, g_jump },
    [SFX_LAND]              = { "land",          0.25f, 0.45f, 0.92f, 1.08f,  30.0f, 0.030f, g_land },
    [SFX_FOOTSTEP]          = { "footstep",      0.14f, 0.30f, 0.90f, 1.12f,  22.0f, 0.040f, g_step },
    [SFX_HURT]              = { "hurt",          0.40f, 0.80f, 0.95f, 1.06f,  30.0f, 0.120f, g_hurt },
    [SFX_HEAL]              = { "heal",          0.40f, 0.55f, 1.00f, 1.00f,  30.0f, 0.050f, g_heal },
    [SFX_ARMOR]             = { "armor",         0.30f, 0.55f, 1.00f, 1.00f,  30.0f, 0.050f, g_armor },
    [SFX_AMMO_PICKUP]       = { "ammo_pickup",   0.20f, 0.50f, 1.00f, 1.00f,  30.0f, 0.030f, g_ammo },
    [SFX_LOW_HEALTH]        = { "low_health",    0.70f, 0.45f, 1.00f, 1.00f,  20.0f, 0.900f, g_lowHp },

    [SFX_UI_MOVE]           = { "ui_move",       0.10f, 0.35f, 1.00f, 1.00f,  10.0f, 0.000f, g_uiMove },
    [SFX_UI_SELECT]         = { "ui_select",     0.22f, 0.50f, 1.00f, 1.00f,  10.0f, 0.000f, g_uiSelect },
    [SFX_UI_BACK]           = { "ui_back",       0.18f, 0.45f, 1.00f, 1.00f,  10.0f, 0.000f, g_uiBack },
    [SFX_WAVE_START]        = { "wave_start",    0.90f, 0.75f, 1.00f, 1.00f, 500.0f, 0.500f, g_waveStart },
    [SFX_WAVE_CLEAR]        = { "wave_clear",    1.10f, 0.70f, 1.00f, 1.00f, 500.0f, 0.500f, g_waveClear },
    [SFX_DOOR]              = { "door",          1.00f, 0.70f, 0.97f, 1.03f,  70.0f, 0.200f, g_door },
    [SFX_SECRET]            = { "secret",        1.10f, 0.65f, 1.00f, 1.00f, 500.0f, 0.500f, g_secret },
};

const char *bp_audio_sfx_name(bp_sfx_id id)
{
    if (id < 0 || id >= SFX_COUNT || !s_recipes[id].name) return "none";
    return s_recipes[id].name;
}

/* ====================================================================================== */
/* 4. dynamic music synthesiser -- runs on raylib's audio thread                           */
/* ====================================================================================== */
#define MUSIC_SR          SFX_SAMPLE_RATE
#define MUSIC_BPM         180.0f
#define MUSIC_STEPS_BAR   16
#define MUSIC_BARS        4
#define MUSIC_LOOP_STEPS  (MUSIC_STEPS_BAR * MUSIC_BARS)
#define MUSIC_STEP_SEC    (60.0f / MUSIC_BPM / 4.0f)      /* one 16th note */

typedef struct {
    float phase;
    float freq;      /* latched at trigger time -- never re-read from the pattern */
    float amp;
    float cutoff;
    float lp;
    float age;
    float dur;
    float drive;
    bool  active;
} music_voice;

typedef struct {
    /* published by the game thread; read by the audio thread. Plain floats: a torn read
     * can only cause one sample of slightly wrong gain, which is inaudible. */
    volatile float w_drums;
    volatile float w_bass;
    volatile float w_lead;
    volatile float w_pad;
    volatile float gain;
    volatile bool  running;

    /* audio-thread private */
    double sample_pos;
    float  step_phase;
    int    step;

    music_voice kick, snare, hat, bass;
    music_voice lead[3], pad[3];

    /* smoothed weights (audio thread) */
    float sD, sB, sL, sP;
    uint32_t rng;
} music_synth;

static music_synth s_music = {
    .w_drums = 0.0f, .w_bass = 0.0f, .w_lead = 0.0f, .w_pad = 0.6f,
    .gain = 0.9f, .running = false, .rng = 0xC0FFEEu
};

/* E natural minor, 16 sixteenth steps per bar, 4 bars. 0 = rest, else MIDI note. */
static const int s_bass_pat[MUSIC_LOOP_STEPS] = {
    40, 0, 40, 0,  40, 0, 47, 0,  40, 0, 40, 0,  52, 0, 47, 0,
    36, 0, 36, 0,  36, 0, 43, 0,  36, 0, 36, 0,  48, 0, 43, 0,
    38, 0, 38, 0,  38, 0, 45, 0,  38, 0, 38, 0,  50, 0, 45, 0,
    35, 0, 35, 0,  42, 0, 35, 0,  47, 0, 42, 0,  35, 0, 47, 0,
};

static const int s_lead_pat[MUSIC_LOOP_STEPS] = {
    76, 0, 0, 79,   0, 76, 0, 0,  74, 0, 0, 76,   0, 0, 71, 0,
    72, 0, 0, 76,   0, 79, 0, 0,  84, 0, 79, 0,   0, 76, 0, 0,
    74, 0, 0, 78,   0, 74, 0, 0,  81, 0, 78, 0,   0, 74, 0, 0,
    71, 0, 74, 0,  78, 0, 83, 0,  86, 0, 83, 0,  78, 0, 74, 0,
};

static const int s_pad_chords[MUSIC_BARS][3] = {
    { 52, 55, 59 },   /* Em */
    { 48, 52, 55 },   /* C  */
    { 50, 54, 57 },   /* D  */
    { 47, 50, 54 },   /* Bm */
};

/* bitmask per 16th: 1 kick, 2 snare, 4 hat */
static const int s_drum_pat[MUSIC_LOOP_STEPS] = {
    5,0,4,0, 5,0,4,2, 4,0,5,0, 4,0,6,4,
    5,0,4,0, 5,0,4,2, 4,0,5,0, 4,4,6,6,
    5,0,4,0, 5,0,4,2, 4,0,5,0, 4,0,6,4,
    5,0,4,4, 5,4,4,2, 5,4,5,4, 6,6,7,7,
};

static inline float midi_hz(int m) { return 440.0f * powf(2.0f, (float)(m - 69) / 12.0f); }

static inline float music_noise(void)
{
    uint32_t x = s_music.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_music.rng = x;
    return ((float)(x & 0xFFFF) / 32767.5f) - 1.0f;
}

static void voice_trigger(music_voice *v, float freq, float dur, float amp, float cutoff, float drive)
{
    v->phase = 0.0f; v->age = 0.0f; v->lp = 0.0f;
    v->freq = freq; v->dur = dur; v->amp = amp;
    v->cutoff = cutoff; v->drive = drive;
    v->active = true;
}

static void music_sequencer_tick(void)
{
    int step = s_music.step;

    int d = s_drum_pat[step];
    if (d & 1) voice_trigger(&s_music.kick,  55.0f, 0.24f, 1.00f, 200.0f, 0.0f);
    if (d & 2) voice_trigger(&s_music.snare, 195.0f, 0.19f, 0.85f, 2700.0f, 0.0f);
    if (d & 4) voice_trigger(&s_music.hat,   7500.0f, 0.055f, 0.40f, 7500.0f, 0.0f);

    int bn = s_bass_pat[step];
    if (bn > 0)
        voice_trigger(&s_music.bass, midi_hz(bn - 12), MUSIC_STEP_SEC * 1.7f, 0.62f,
                      midi_hz(bn) * 5.5f, 6.0f);

    int ln = s_lead_pat[step];
    if (ln > 0)
    {
        int slot = (step / 2) % 3;
        voice_trigger(&s_music.lead[slot], midi_hz(ln), MUSIC_STEP_SEC * 2.4f, 0.15f,
                      midi_hz(ln), 3.5f);
    }

    if ((step % MUSIC_STEPS_BAR) == 0)
    {
        int bar = step / MUSIC_STEPS_BAR;
        for (int i = 0; i < 3; i++)
            voice_trigger(&s_music.pad[i], midi_hz(s_pad_chords[bar][i]),
                          MUSIC_STEP_SEC * (float)MUSIC_STEPS_BAR * 1.02f, 0.075f,
                          midi_hz(s_pad_chords[bar][i]), 1.0f);
    }

    s_music.step = (step + 1) % MUSIC_LOOP_STEPS;
}

/* Render `frames` stereo float samples. Called from the audio thread only. */
static void music_callback(void *bufferData, unsigned int frames)
{
    float *out = (float *)bufferData;
    const float dt = 1.0f / (float)MUSIC_SR;

    /* Smooth the published weights on the audio thread so fades never click. */
    const float k = 0.02f;
    s_music.sD += ((float)s_music.w_drums - s_music.sD) * k;
    s_music.sB += ((float)s_music.w_bass  - s_music.sB) * k;
    s_music.sL += ((float)s_music.w_lead  - s_music.sL) * k;
    s_music.sP += ((float)s_music.w_pad   - s_music.sP) * k;

    float wD = s_music.sD, wB = s_music.sB, wL = s_music.sL, wP = s_music.sP;
    float gain = (float)s_music.gain;

    if (!s_music.running)
    {
        memset(out, 0, sizeof(float) * frames * 2);
        return;
    }

    for (unsigned int f = 0; f < frames; f++)
    {
        s_music.step_phase += dt;
        while (s_music.step_phase >= MUSIC_STEP_SEC)
        {
            s_music.step_phase -= MUSIC_STEP_SEC;
            music_sequencer_tick();
        }

        float dL = 0.0f, dR = 0.0f, bS = 0.0f, lL = 0.0f, lR = 0.0f, pL = 0.0f, pR = 0.0f;

        /* --- kick: pitch-dropping sine, lightly saturated --- */
        if (s_music.kick.active)
        {
            music_voice *v = &s_music.kick;
            v->age += dt;
            if (v->age >= v->dur) v->active = false;
            else
            {
                float hz = 135.0f * expf(-15.0f * v->age) + 45.0f;
                v->phase += hz * dt;
                float s = sinf(2.0f * BP_PI * v->phase) * expf(-13.5f * v->age) * v->amp;
                s += dsp_saturate(s, 2.2f) * 0.30f;
                dL += s; dR += s;
            }
        }
        /* --- snare: noise + tuned body --- */
        if (s_music.snare.active)
        {
            music_voice *v = &s_music.snare;
            v->age += dt;
            if (v->age >= v->dur) v->active = false;
            else
            {
                float env = expf(-23.0f * v->age);
                float lp = dsp_lowpass((bp_onepole *)&v->lp, music_noise(), v->cutoff, dt);
                float s = music_noise() * env * v->amp * 0.70f + lp * env * v->amp * 0.55f;
                s += sinf(2.0f * BP_PI * v->freq * v->age) * env * 0.16f;
                dL += s; dR += s;
            }
        }
        /* --- hat: high-passed noise click, slightly right-biased --- */
        if (s_music.hat.active)
        {
            music_voice *v = &s_music.hat;
            v->age += dt;
            if (v->age >= v->dur) v->active = false;
            else
            {
                float n = music_noise();
                float hp = n - dsp_lowpass((bp_onepole *)&v->lp, n, v->cutoff, dt);
                float s = hp * expf(-75.0f * v->age) * v->amp;
                dL += s * 0.75f; dR += s;
            }
        }
        /* --- bass: saturated saw+square through a low pass --- */
        if (s_music.bass.active)
        {
            music_voice *v = &s_music.bass;
            v->age += dt;
            if (v->age >= v->dur) v->active = false;
            else
            {
                v->phase += v->freq * dt;
                float ph = fmodf(v->phase, 1.0f);
                float s = dsp_saturate((2.0f * ph - 1.0f) * 0.7f + ((ph < 0.5f) ? 0.55f : -0.55f) * 0.3f,
                                       v->drive);
                s = dsp_lowpass((bp_onepole *)&v->lp, s, BP_MAX(v->cutoff, 80.0f), dt);
                bS += s * v->amp * (0.20f + 0.80f * expf(-3.4f * v->age));
            }
        }
        /* --- lead: 3 detuned voices, hard-panned for width --- */
        for (int i = 0; i < 3; i++)
        {
            music_voice *v = &s_music.lead[i];
            if (!v->active) continue;
            v->age += dt;
            if (v->age >= v->dur) { v->active = false; continue; }

            float det = (i == 1) ? 1.005f : (i == 2 ? 0.995f : 1.0f);
            v->phase += v->freq * det * dt;
            float s = dsp_saturate(2.0f * fmodf(v->phase, 1.0f) - 1.0f, v->drive);
            float fc = v->cutoff * 5.0f * (0.30f + 0.70f * expf(-8.0f * v->age));
            s = dsp_lowpass((bp_onepole *)&v->lp, s, BP_MAX(fc, 90.0f), dt);
            float env = expf(-2.6f * v->age) * v->amp;
            float pan = (i == 0) ? 0.5f : (i == 1 ? 0.20f : 0.80f);
            lL += s * env * (1.0f - pan);
            lR += s * env * pan;
        }
        /* --- pad: slow triangle chord, drifting pan --- */
        for (int i = 0; i < 3; i++)
        {
            music_voice *v = &s_music.pad[i];
            if (!v->active) continue;
            v->age += dt;
            if (v->age >= v->dur) { v->active = false; continue; }

            v->phase += v->freq * dt;
            float tri = fabsf(fmodf(v->phase * 2.0f, 2.0f) - 1.0f) * 2.0f - 1.0f;
            tri = dsp_lowpass((bp_onepole *)&v->lp, tri, BP_MAX(v->cutoff * 3.0f, 120.0f), dt);
            float env = sinf(BP_PI * BP_CLAMP(v->age / v->dur, 0.0f, 1.0f)) * v->amp;
            float pan = 0.5f + 0.32f * sinf(0.08f * v->age + (float)i * 2.1f);
            pL += tri * env * (1.0f - pan);
            pR += tri * env * pan;
        }

        float mixL = dL * wD + bS * wB + lL * wL + pL * wP;
        float mixR = dR * wD + bS * wB + lR * wL + pR * wP;

        mixL = dsp_saturate(mixL * gain, 1.30f) * 0.85f;
        mixR = dsp_saturate(mixR * gain, 1.30f) * 0.85f;

        out[f * 2 + 0] = BP_CLAMP(mixL, -1.0f, 1.0f);
        out[f * 2 + 1] = BP_CLAMP(mixR, -1.0f, 1.0f);

        s_music.sample_pos += 1.0;
    }
}

/* ====================================================================================== */
/* 5. init / shutdown                                                                      */
/* ====================================================================================== */
static int16_t *scratch_to_pcm(int frames)
{
    int16_t *pcm = (int16_t *)malloc(sizeof(int16_t) * (size_t)frames);
    if (!pcm) return NULL;
    for (int i = 0; i < frames; i++)
        pcm[i] = (int16_t)(BP_CLAMP(s_scratch[i], -1.0f, 1.0f) * 32760.0f);
    return pcm;
}

void bp_audio_init(void)
{
    memset(&g_audio, 0, sizeof(g_audio));
    g_audio.master_volume = 1.0f;
    g_audio.sfx_volume    = 1.0f;
    g_audio.music_volume  = 0.55f;
    g_audio.music_current = -1;

    InitAudioDevice();
    if (!IsAudioDeviceReady())
    {
        BP_LOGW("audio: device failed to open -- running silent");
        g_audio.initialised = false;
        return;
    }

    s_scratch = (float *)malloc(sizeof(float) * SFX_SCRATCH);
    if (!s_scratch) { BP_LOGE("audio: scratch allocation failed"); return; }

    double t0 = GetTime();
    int loaded = 0;

    for (int i = 0; i < SFX_COUNT; i++)
    {
        const bp_sfx_recipe *r = &s_recipes[i];
        if (!r->generate) continue;

        int frames = (int)(r->duration * (float)SFX_SAMPLE_RATE);
        frames = BP_CLAMP(frames, 1, SFX_SCRATCH);

        memset(s_scratch, 0, sizeof(float) * (size_t)frames);
        r->generate(s_scratch, frames, (float)SFX_SAMPLE_RATE);
        normalise(s_scratch, frames, 0.92f);

        int16_t *pcm = scratch_to_pcm(frames);
        if (!pcm) continue;

        Wave w;
        w.data = pcm;
        w.frameCount = (unsigned int)frames;
        w.sampleRate = SFX_SAMPLE_RATE;
        w.sampleSize = 16;
        w.channels = 1;

        bp_sfx_bank *b = &s_bank[i];
        b->voices[0] = LoadSoundFromWave(w);
        for (int v = 1; v < SFX_VOICES_PER_ID; v++)
            b->voices[v] = LoadSoundAlias(b->voices[0]);
        for (int v = 0; v < SFX_VOICES_PER_ID; v++) b->last_used[v] = -1e9;
        b->loaded = true;

        UnloadWave(w);   /* LoadSoundFromWave copied + format-converted the samples; UnloadWave frees w.data (pcm) */

        g_audio.sfx[i].valid       = true;
        g_audio.sfx[i].sound       = b->voices[0];
        g_audio.sfx[i].base_volume = r->volume;
        g_audio.sfx[i].pitch_min   = r->pitchMin;
        g_audio.sfx[i].pitch_max   = r->pitchMax;
        g_audio.sfx[i].falloff     = r->falloff;
        g_audio.sfx[i].cooldown    = r->cooldown;
        loaded++;
    }

    free(s_scratch);
    s_scratch = NULL;

    /* ---- dynamic music stream ---- */
    AudioStream stream = LoadAudioStream(MUSIC_SR, 32, 2);
    if (stream.buffer)
    {
        SetAudioStreamCallback(stream, music_callback);
        g_audio.music_calm.stream = stream;
        g_audio.music_calm.looping = true;
        g_audio.music_valid[0] = true;
        s_music.running = true;
        PlayAudioStream(stream);
        BP_LOGI("audio: dynamic music synth online (%d BPM, %d-step loop)",
                (int)MUSIC_BPM, MUSIC_LOOP_STEPS);
    }
    else BP_LOGW("audio: music stream unavailable");

    g_audio.initialised = true;
    BP_LOGI("audio: %d/%d SFX synthesised in %.0f ms", loaded, SFX_COUNT, (GetTime() - t0) * 1000.0);
}

void bp_audio_shutdown(void)
{
    if (!g_audio.initialised) return;

    s_music.running = false;

    for (int i = 0; i < SFX_COUNT; i++)
    {
        if (!s_bank[i].loaded) continue;
        for (int v = 1; v < SFX_VOICES_PER_ID; v++) UnloadSoundAlias(s_bank[i].voices[v]);
        UnloadSound(s_bank[i].voices[0]);
        s_bank[i].loaded = false;
        g_audio.sfx[i].valid = false;
    }

    if (g_audio.music_valid[0])
    {
        StopAudioStream(g_audio.music_calm.stream);
        UnloadAudioStream(g_audio.music_calm.stream);
        g_audio.music_valid[0] = false;
    }

    CloseAudioDevice();
    g_audio.initialised = false;
    BP_LOGI("audio: shut down");
}

/* ====================================================================================== */
/* 6. playback                                                                             */
/* ====================================================================================== */
static float rand01(void)
{
    return (float)(bp_rng_next(&g_engine.rng) & 0xFFFF) / 65535.0f;
}

static void play_voice(bp_sfx_id id, float volume, float pitch, float pan)
{
    if (!g_audio.initialised || id < 0 || id >= SFX_COUNT) return;
    bp_sfx_bank *b = &s_bank[id];
    if (!b->loaded) return;

    /* Choose the alias that has been silent longest so a machine gun never self-cancels. */
    int best = 0;
    double now = GetTime();
    for (int v = 1; v < SFX_VOICES_PER_ID; v++)
        if (b->last_used[v] < b->last_used[best]) best = v;
    b->last_used[best] = now;

    Sound s = b->voices[best];
    float vol = BP_CLAMP(volume * g_audio.sfx_volume * g_audio.master_volume, 0.0f, 1.0f);
    if (vol <= 0.003f) { g_audio.dropped_voices++; return; }

    SetSoundPitch(s, BP_CLAMP(pitch, 0.25f, 4.0f));
    SetSoundPan(s, BP_CLAMP(pan, 0.0f, 1.0f));
    SetSoundVolume(s, vol);
    PlaySound(s);
    g_audio.active_voices++;
}

void bp_audio_play(bp_sfx_id id, float volumeScale)
{
    if (id < 0 || id >= SFX_COUNT || !g_audio.initialised) return;
    const bp_sfx_recipe *r = &s_recipes[id];
    if (!r->generate) return;

    double now = GetTime();
    if (r->cooldown > 0.0f && (now - g_audio.sfx[id].last_played) < (double)r->cooldown) return;
    g_audio.sfx[id].last_played = now;

    float pitch = r->pitchMin + (r->pitchMax - r->pitchMin) * rand01();
    play_voice(id, g_audio.sfx[id].base_volume * volumeScale, pitch, 0.5f);
}

void bp_audio_play_variant(bp_sfx_id id, Vector3 position, Vector3 listenerPos,
                           Vector3 listenerForward, float volumeScale, float pitchJitter)
{
    if (id < 0 || id >= SFX_COUNT || !g_audio.initialised) return;
    const bp_sfx_recipe *r = &s_recipes[id];
    if (!r->generate) return;

    Vector3 to = Vector3Subtract(position, listenerPos);
    float dist = Vector3Length(to);
    float falloff = r->falloff > 0.0f ? r->falloff : 60.0f;
    if (dist > falloff) return;

    /* Inverse distance with a soft knee (no blow-out at zero distance) times a linear
     * fade to the falloff radius, so sounds never pop out of existence. */
    float atten = (1.0f / (1.0f + 0.35f * dist)) * (1.0f - dist / falloff);
    atten = BP_CLAMP(atten, 0.0f, 1.0f);

    float pan = 0.5f;
    if (dist > 0.35f)
    {
        Vector3 fwd = { listenerForward.x, 0.0f, listenerForward.z };
        float fl = Vector3Length(fwd);
        if (fl > 0.001f)
        {
            fwd = Vector3Scale(fwd, 1.0f / fl);
            Vector3 right = { fwd.z, 0.0f, -fwd.x };
            pan = BP_CLAMP(0.5f + 0.5f * Vector3DotProduct(Vector3Scale(to, 1.0f / dist), right), 0.0f, 1.0f);
        }
    }

    double now = GetTime();
    if (r->cooldown > 0.0f && (now - g_audio.sfx[id].last_played) < (double)r->cooldown) return;
    g_audio.sfx[id].last_played = now;

    float lo = r->pitchMin * (1.0f - pitchJitter);
    float hi = r->pitchMax * (1.0f + pitchJitter);
    play_voice(id, g_audio.sfx[id].base_volume * volumeScale * atten, lo + (hi - lo) * rand01(), pan);
}

void bp_audio_play_at(bp_sfx_id id, Vector3 position, Vector3 listenerPos,
                      Vector3 listenerForward, float volumeScale)
{
    bp_audio_play_variant(id, position, listenerPos, listenerForward, volumeScale, 0.0f);
}

/* ====================================================================================== */
/* 7. per-frame                                                                            */
/* ====================================================================================== */
void bp_audio_set_intensity(float target01, float dt)
{
    if (!g_audio.initialised) return;

    target01 = BP_CLAMP(target01, 0.0f, 1.0f);
    g_audio.intensity = bp_damp(g_audio.intensity, target01, 1.6f, BP_MAX(dt, 0.0001f));
    float i = g_audio.intensity;

    /* Layer map:
     *   0.00-0.15  menu / exploration -> pad only
     *   0.15-0.40  tension            -> + bass
     *   0.40-0.65  combat             -> + drums
     *   0.68-0.93  peak               -> + lead
     */
    s_music.w_pad   = 1.0f;
    s_music.w_bass  = BP_CLAMP((i - 0.12f) / 0.25f, 0.0f, 1.0f);
    s_music.w_drums = BP_CLAMP((i - 0.40f) / 0.25f, 0.0f, 1.0f);
    s_music.w_lead  = BP_CLAMP((i - 0.68f) / 0.25f, 0.0f, 1.0f);
    s_music.gain    = g_audio.music_volume * g_audio.master_volume * (0.70f + 0.35f * i);
}

void bp_audio_set_volumes(float master, float sfx, float music)
{
    g_audio.master_volume = BP_CLAMP(master, 0.0f, 1.0f);
    g_audio.sfx_volume    = BP_CLAMP(sfx, 0.0f, 1.0f);
    g_audio.music_volume  = BP_CLAMP(music, 0.0f, 1.0f);
}

void bp_audio_update(float dt)
{
    BP_UNUSED(dt);
    if (!g_audio.initialised) return;
    /* Telemetry decays so the debug overlay shows a live-ish voice count. */
    g_audio.active_voices = (int)((float)g_audio.active_voices * 0.80f);
}
