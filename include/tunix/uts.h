#ifndef TUNIX_UTS_H
#define TUNIX_UTS_H

#include <stddef.h>

#define UTS_NAME_MAX 64
#define UTS_SYSNAME  "Tunix"
#define UTS_RELEASE  "0.1.0"
// The commit the kernel was built from, so that uname -v and /proc/version say which
// kernel is running; the build passes it to the files that print them.
#ifdef TUNIX_COMMIT
#define UTS_BUILD TUNIX_COMMIT
#else
#define UTS_BUILD "unknown"
#endif
#define UTS_VERSION "Tunix Kernel " UTS_BUILD

const char *uts_hostname(void);
const char *uts_domainname(void);
void uts_set_hostname(const char *name, size_t length);
void uts_set_domainname(const char *name, size_t length);

#endif
