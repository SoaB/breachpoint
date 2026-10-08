/******************************************************************************************
 * bp_config.h -- tiny data-driven config loader.
 *
 * Commercial teams do not want to recompile to change a damage number. This module reads
 * flat text tables (key/value blocks) and fills plain C structs, so a designer can
 * rebalance the whole weapon set with a text editor and no build step.
 *
 * Format (deliberately simple, hand-editable, diff-friendly):
 *
 *      # comment
 *      weapon shotgun
 *          display_name = SCATTER-12
 *          damage       = 11
 *          pellets      = 9
 *      end
 *
 * The parser is allocation-free: the file is read into a static buffer, split into line
 * spans once, then walked with a cursor. Unknown keys produce a warning, never an error --
 * that keeps old clients able to load new data files.
 ******************************************************************************************/
#ifndef BP_CONFIG_H
#define BP_CONFIG_H

#include "bp_common.h"

#define BP_CFG_MAX_LINE     256
#define BP_CFG_MAX_FILE     (128 * 1024)
#define BP_CFG_MAX_LINES    2048

typedef struct {
    char  buffer[BP_CFG_MAX_FILE];
    int   length;
    int   line_start[BP_CFG_MAX_LINES];   /* byte offset of each line            */
    int   line_len[BP_CFG_MAX_LINES];     /* length excluding the newline        */
    int   line_count;
    int   cursor;                         /* index into line_*                   */

    char  section_type[32];               /* "weapon", "enemy", ...              */
    char  section_id[32];                 /* "shotgun", "grunt", ...             */
    char  key[64];
    char  value[BP_CFG_MAX_LINE];
    bool  ok;
} bp_config;

bool        bp_config_load(bp_config *cfg, const char *path);
bool        bp_config_load_mem(bp_config *cfg, const char *data, int length);
void        bp_config_rewind(bp_config *cfg);

/* Advance to the next "<type> <id>" block. On success section_type/section_id are set and
 * the cursor points at the first key. Returns false at EOF. */
bool        bp_config_next_section(bp_config *cfg);

/* Find a specific block by type+id, e.g. bp_config_find_section(&c, "weapon", "railgun"). */
bool        bp_config_find_section(bp_config *cfg, const char *type, const char *id);

const char *bp_config_section_type(const bp_config *cfg);
const char *bp_config_section_id(const bp_config *cfg);

/* Advance to the next key inside the current block. Returns false at "end". */
bool        bp_config_next_key(bp_config *cfg);

/* Typed getters for the key last returned by bp_config_next_key. */
int         bp_cfg_int(const bp_config *cfg, int fallback);
float       bp_cfg_float(const bp_config *cfg, float fallback);
bool        bp_cfg_bool(const bp_config *cfg, bool fallback);
const char *bp_cfg_str(const bp_config *cfg, const char *fallback);
Color       bp_cfg_color(const bp_config *cfg, Color fallback);
Vector3     bp_cfg_vec3(const bp_config *cfg, Vector3 fallback);

#endif /* BP_CONFIG_H */
