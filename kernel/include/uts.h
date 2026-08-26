#ifndef TUNIX_UTS_H
#define TUNIX_UTS_H

#include <stddef.h>

/*
 * The machine's own names, which two places need: uname(2) and sethostname(2)
 * on one side, /proc/sys/kernel/hostname on the other. Void's init writes the
 * proc file rather than calling the syscall, so they have to be the same
 * string or the hostname depends on which one asked.
 */
#define UTS_NAME_MAX 64

const char *uts_hostname(void);
const char *uts_domainname(void);
/* Truncated at UTS_NAME_MAX; a trailing newline is dropped, because writing
   the file with `echo` is how it is usually set. */
void uts_set_hostname(const char *name, size_t length);
void uts_set_domainname(const char *name, size_t length);

#endif
