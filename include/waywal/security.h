// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_SECURITY_H
#define WAYWAL_SECURITY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct waywal_config;

/* Registers parsed configuration prior to applying Landlock rules (for template paths) */
void security_sandbox_register_config(const struct waywal_config *cfg);

/* Applies PR_SET_NO_NEW_PRIVS, Landlock LSM filesystem sandboxing, and Seccomp-BPF */
bool security_sandbox_apply(const char *runtime_dir);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SECURITY_H */
