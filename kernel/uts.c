#include "include/uts.h"
#include "include/kstring.h"

static char hostname[UTS_NAME_MAX + 1] = "tunix";
static char domainname[UTS_NAME_MAX + 1] = "(none)";

const char *uts_hostname(void) { return hostname; }
const char *uts_domainname(void) { return domainname; }

static void set(char *destination, const char *name, size_t length) {
    if (length && name[length - 1] == '\n') length--;
    if (length > UTS_NAME_MAX) length = UTS_NAME_MAX;
    memcpy(destination, name, length);
    destination[length] = '\0';
}

void uts_set_hostname(const char *name, size_t length) {
    set(hostname, name, length);
}

void uts_set_domainname(const char *name, size_t length) {
    set(domainname, name, length);
}
