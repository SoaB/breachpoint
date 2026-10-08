/******************************************************************************************
 * bp_config.c
 ******************************************************************************************/
#include "bp_config.h"
#include <ctype.h>

/* -------------------------------------------------------------------------------------- */
/* portable helpers (strcasecmp is POSIX, _stricmp is MSVC)                                */
/* -------------------------------------------------------------------------------------- */
static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* -------------------------------------------------------------------------------------- */
/* line splitting                                                                          */
/* -------------------------------------------------------------------------------------- */
static void split_lines(bp_config *cfg)
{
    cfg->line_count = 0;
    int i = 0;
    while (i < cfg->length && cfg->line_count < BP_CFG_MAX_LINES)
    {
        int start = i;
        while (i < cfg->length && cfg->buffer[i] != '\n') i++;
        int end = i;
        if (i < cfg->length) i++;                 /* skip '\n' */
        while (end > start && (cfg->buffer[end - 1] == '\r')) end--;
        cfg->line_start[cfg->line_count] = start;
        cfg->line_len[cfg->line_count]   = end - start;
        cfg->line_count++;
    }
}

/* Get line `idx` as a NUL-terminated, whitespace/comment-stripped string. */
static char *fetch_line(bp_config *cfg, int idx, char *out, int outSize)
{
    out[0] = '\0';
    if (idx < 0 || idx >= cfg->line_count) return out;

    int len = cfg->line_len[idx];
    if (len > outSize - 1) len = outSize - 1;
    memcpy(out, cfg->buffer + cfg->line_start[idx], (size_t)len);
    out[len] = '\0';

    char *hash = strchr(out, '#');
    if (hash) *hash = '\0';

    char *s = out;
    while (*s && isspace((unsigned char)*s)) s++;
    if (s != out) memmove(out, s, strlen(s) + 1);

    char *e = out + strlen(out);
    while (e > out && isspace((unsigned char)e[-1])) *--e = '\0';
    return out;
}

/* -------------------------------------------------------------------------------------- */
/* loading                                                                                 */
/* -------------------------------------------------------------------------------------- */
bool bp_config_load_mem(bp_config *cfg, const char *data, int length)
{
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));
    if (!data || length <= 0) { cfg->ok = false; return false; }
    if (length > BP_CFG_MAX_FILE - 1) length = BP_CFG_MAX_FILE - 1;

    memcpy(cfg->buffer, data, (size_t)length);
    cfg->buffer[length] = '\0';
    cfg->length = length;
    split_lines(cfg);
    cfg->cursor = 0;
    cfg->ok = true;
    return true;
}

bool bp_config_load(bp_config *cfg, const char *path)
{
    memset(cfg, 0, sizeof(*cfg));

    if (!FileExists(path))
    {
        BP_LOGW("config: '%s' not found", path);
        cfg->ok = false;
        return false;
    }

    int len = 0;
    unsigned char *raw = LoadFileData(path, &len);
    if (!raw || len <= 0)
    {
        BP_LOGE("config: failed to read '%s'", path);
        if (raw) UnloadFileData(raw);
        cfg->ok = false;
        return false;
    }

    bool ok = bp_config_load_mem(cfg, (const char *)raw, len);
    UnloadFileData(raw);
    BP_LOGI("config: loaded '%s' (%d bytes, %d lines)", path, len, cfg->line_count);
    return ok;
}

void bp_config_rewind(bp_config *cfg) { cfg->cursor = 0; }

/* -------------------------------------------------------------------------------------- */
/* section walking                                                                         */
/* -------------------------------------------------------------------------------------- */
static bool line_is_section(bp_config *cfg, int idx, char *type, char *id)
{
    char line[BP_CFG_MAX_LINE];
    fetch_line(cfg, idx, line, sizeof(line));
    if (!line[0]) return false;

    /* "end" terminates, never starts */
    if (!ci_cmp(line, "end")) return false;
    if (strchr(line, '=')) return false;

    char a[32] = { 0 }, b[32] = { 0 };
    int n = sscanf(line, "%31s %31s", a, b);
    if (n != 2 || !b[0]) return false;

    if (type) snprintf(type, 32, "%s", a);
    if (id)   snprintf(id,   32, "%s", b);
    return true;
}

bool bp_config_next_section(bp_config *cfg)
{
    if (!cfg->ok) return false;

    for (int i = cfg->cursor; i < cfg->line_count; i++)
    {
        char type[32], id[32];
        if (line_is_section(cfg, i, type, id))
        {
            snprintf(cfg->section_type, sizeof(cfg->section_type), "%s", type);
            snprintf(cfg->section_id,   sizeof(cfg->section_id),   "%s", id);
            cfg->cursor = i + 1;
            return true;
        }
    }
    cfg->cursor = cfg->line_count;
    return false;
}

bool bp_config_find_section(bp_config *cfg, const char *type, const char *id)
{
    bp_config_rewind(cfg);
    while (bp_config_next_section(cfg))
    {
        if (!ci_cmp(cfg->section_type, type) && !ci_cmp(cfg->section_id, id)) return true;
    }
    return false;
}

const char *bp_config_section_type(const bp_config *cfg) { return cfg->section_type; }
const char *bp_config_section_id(const bp_config *cfg)   { return cfg->section_id; }

bool bp_config_next_key(bp_config *cfg)
{
    if (!cfg->ok) return false;

    for (int i = cfg->cursor; i < cfg->line_count; i++)
    {
        char line[BP_CFG_MAX_LINE];
        fetch_line(cfg, i, line, sizeof(line));
        if (!line[0]) continue;

        if (!ci_cmp(line, "end")) { cfg->cursor = i + 1; return false; }

        char type[32], id[32];
        if (line_is_section(cfg, i, type, id)) { cfg->cursor = i; return false; }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        while (*k && isspace((unsigned char)*k)) k++;
        char *ke = k + strlen(k);
        while (ke > k && isspace((unsigned char)ke[-1])) *--ke = '\0';
        while (*v && isspace((unsigned char)*v)) v++;
        char *ve = v + strlen(v);
        while (ve > v && isspace((unsigned char)ve[-1])) *--ve = '\0';

        snprintf(cfg->key,   sizeof(cfg->key),   "%s", k);
        snprintf(cfg->value, sizeof(cfg->value), "%s", v);
        cfg->cursor = i + 1;
        return true;
    }
    cfg->cursor = cfg->line_count;
    return false;
}

/* -------------------------------------------------------------------------------------- */
/* typed getters                                                                           */
/* -------------------------------------------------------------------------------------- */
int bp_cfg_int(const bp_config *cfg, int fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;
    return atoi(cfg->value);
}

float bp_cfg_float(const bp_config *cfg, float fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;
    return (float)atof(cfg->value);
}

bool bp_cfg_bool(const bp_config *cfg, bool fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;
    if (!ci_cmp(cfg->value, "true") || !ci_cmp(cfg->value, "yes") || !strcmp(cfg->value, "1")) return true;
    if (!ci_cmp(cfg->value, "false") || !ci_cmp(cfg->value, "no") || !strcmp(cfg->value, "0")) return false;
    return fallback;
}

const char *bp_cfg_str(const bp_config *cfg, const char *fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;
    return cfg->value;
}

Color bp_cfg_color(const bp_config *cfg, Color fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;

    if (cfg->value[0] == '#')
    {
        const char *h = cfg->value + 1;
        int n = (int)strlen(h);
        if (n >= 6)
        {
            char hex[3] = { 0 };
            unsigned int r = 255, g = 255, b = 255, a = 255;
            memcpy(hex, h + 0, 2); r = (unsigned int)strtoul(hex, NULL, 16);
            memcpy(hex, h + 2, 2); g = (unsigned int)strtoul(hex, NULL, 16);
            memcpy(hex, h + 4, 2); b = (unsigned int)strtoul(hex, NULL, 16);
            if (n >= 8) { memcpy(hex, h + 6, 2); a = (unsigned int)strtoul(hex, NULL, 16); }
            return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a };
        }
        return fallback;
    }

    unsigned int r = 255, g = 255, b = 255, a = 255;
    int got = sscanf(cfg->value, "%u %u %u %u", &r, &g, &b, &a);
    if (got >= 3) return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a };
    return fallback;
}

Vector3 bp_cfg_vec3(const bp_config *cfg, Vector3 fallback)
{
    if (!cfg->ok || !cfg->value[0]) return fallback;
    Vector3 v = fallback;
    if (sscanf(cfg->value, "%f %f %f", &v.x, &v.y, &v.z) == 3) return v;
    if (sscanf(cfg->value, "%f %f", &v.x, &v.z) == 2) { v.y = fallback.y; return v; }
    return fallback;
}
