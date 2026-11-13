// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_MDL_WRITER_H
#define WAYWAL_WE_MDL_WRITER_H

#include "waywal/we_physics.h"
#include "waywal/we_puppet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_MDL_WRITER_VERSION_MDLV 23u
#define WE_MDL_WRITER_VERSION_MDAT 1u
#define WE_MDL_WRITER_VERSION_MDLS 2u
#define WE_MDL_WRITER_VERSION_MDLA 3u
#define WE_MDL_WRITER_MAX_BYTES    (64u * 1024u * 1024u)

typedef struct {
    bool emit_attachments;
    bool emit_skeleton;
    bool emit_animations;
    bool sanitize_ascii_strings;
    int32_t override_mdl_flag; /* <= 0 keeps puppet->mdl_flag (normalized != 9) */
} we_mdl_write_opts_t;

void we_mdl_write_opts_default(we_mdl_write_opts_t *out_opts);

/* Serializes puppet into canonical MDLV0023 / MDAT0001 / MDLS0002 / MDLA0003 bytes
 * guaranteed to round-trip losslessly through we_puppet_parse_cpu(). */
bool we_mdl_write_mem(const we_puppet_t *puppet, const we_mdl_write_opts_t *opts,
                      uint8_t **out_bytes, size_t *out_size);

bool we_mdl_write_file(const we_puppet_t *puppet, const we_mdl_write_opts_t *opts,
                       const char *path);

void we_mdl_write_free(uint8_t *bytes);

/* Formats compact locale-independent simulation JSON (< 256 bytes) compatible with
 * we_physics_parse_simulation_json(). */
bool we_physics_format_simulation_json(const we_bone_sim_config_t *cfg, char out_json[256]);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_MDL_WRITER_H */
