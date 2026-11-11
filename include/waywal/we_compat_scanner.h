// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_WE_COMPAT_SCANNER_H
#define WAYWAL_WE_COMPAT_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WE_SCAN_MAX_ISSUES  256u
#define WE_SCAN_MAX_REPAIRS 64u

typedef enum {
    WE_TIER_UNKNOWN = 0,
    WE_TIER_1_NATIVE = 1,
    WE_TIER_2_TRANSPILED = 2,
    WE_TIER_3_DEGRADED = 3,
    WE_TIER_4_BLOCKED = 4,
} we_compat_tier_t;

typedef enum {
    WE_ISSUE_SEV_INFO = 0,
    WE_ISSUE_SEV_WARNING = 1,
    WE_ISSUE_SEV_ERROR = 2,
    WE_ISSUE_SEV_FATAL = 3,
} we_issue_severity_t;

typedef struct {
    char code[64];
    we_issue_severity_t severity;
    char message[256];
    char file_path[256];
    uint32_t line_number;
    char suggested_fix[256];
} we_compat_issue_t;

typedef struct {
    char transformation_id[64];
    char target_file[256];
    char description[256];
} we_auto_repair_t;

typedef struct {
    uint32_t total_objects;
    uint32_t sprite_layers;
    uint32_t puppet_meshes;
    uint32_t particle_systems;
    uint32_t sound_layers;
    uint32_t active_scripts;
    float estimated_vram_mb;
    uint32_t estimated_fps;
} we_compat_summary_t;

/* Note: sizeof(we_compat_report_t) is 252,280 bytes (~246.4 KB).
 * MUST be dynamically allocated on the heap via we_compat_report_alloc();
 * declaring on the thread stack will cause immediate stack exhaustion and SIGSEGV. */
typedef struct {
    char workshop_id[64];
    char title[256];
    we_compat_tier_t tier;
    uint32_t compatibility_score; // 0 .. 100
    uint64_t scan_timestamp;

    we_compat_summary_t summary;

    we_compat_issue_t issues[WE_SCAN_MAX_ISSUES];
    uint32_t issue_count;

    we_auto_repair_t repairs[WE_SCAN_MAX_REPAIRS];
    uint32_t repair_count;
} we_compat_report_t;

/* Dynamic heap lifecycle management for 246 KB compatibility reports */
we_compat_report_t *we_compat_report_alloc(void);
void we_compat_report_free(we_compat_report_t *report);

typedef struct we_compat_scanner we_compat_scanner_t;

/* Creates a capability scanner configured with target GPU extensions */
we_compat_scanner_t *we_compat_scanner_create(bool supports_compute, bool supports_bc7);
void we_compat_scanner_destroy(we_compat_scanner_t *scanner);

/* Scans a Workshop directory or .pkg archive and populates out_report */
bool we_compat_scan_directory(we_compat_scanner_t *scanner, const char *workshop_dir,
                              we_compat_report_t *out_report);

/* Applies confined in-place auto-repairs strictly beneath target_dir; returns applied count */
uint32_t we_compat_apply_repairs(const we_compat_report_t *report, const char *target_dir);

/* Copies source_dir to target_dir (skipping symlinks) and applies confined auto-repairs */
bool we_compat_apply_auto_repairs(const we_compat_report_t *report, const char *source_dir,
                                  const char *target_dir);

/* Serializes a compatibility report to JSON format */
char *we_compat_report_to_json(const we_compat_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_WE_COMPAT_SCANNER_H */
