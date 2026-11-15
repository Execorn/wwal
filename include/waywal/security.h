// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SECURITY_H
#define WAYWAL_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct waywal_config;

/* Registers parsed configuration prior to applying Landlock rules (for template paths) */
void security_sandbox_register_config(const struct waywal_config *cfg);

/* Returns runtime kernel Landlock ABI version (>0) or -1 if unavailable */
int security_landlock_probe_abi(void);

/* Computes Landlock handled_access_fs bitmask negotiated for the given kernel ABI version */
uint64_t security_landlock_handled_fs_mask(int abi);

/* Computes Landlock access rights for writable directories (never includes execute permission) */
uint64_t security_landlock_writable_fs_mask(int abi);

/* Applies PR_SET_NO_NEW_PRIVS, Landlock LSM filesystem sandboxing, and Seccomp-BPF */
bool security_sandbox_apply(const char *runtime_dir);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SECURITY_H */
