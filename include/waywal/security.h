#ifndef WAYWAL_SECURITY_H
#define WAYWAL_SECURITY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Applies PR_SET_NO_NEW_PRIVS, Landlock LSM filesystem sandboxing, and Seccomp-BPF */
bool security_sandbox_apply(const char *runtime_dir);

#ifdef __cplusplus
}
#endif

#endif /* WAYWAL_SECURITY_H */
