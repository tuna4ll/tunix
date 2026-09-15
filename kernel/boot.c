#include <stddef.h>

#include "include/boot.h"

const char *boot_command_line_value(const char *key) {
    const char *line = boot_info()->command_line;
    const char *at = line;
    if (!at) return NULL;

    for (; *at != '\0'; at++) {
        if (at != line && at[-1] != ' ') continue;

        size_t index = 0;
        while (key[index] != '\0' && at[index] == key[index]) index++;
        if (key[index] == '\0' && at[index] == '=') return at + index + 1;
    }
    return NULL;
}

int boot_command_line_flag(const char *key) {
    const char *line = boot_info()->command_line;
    const char *at = line;
    if (!at) return 0;

    for (; *at != '\0'; at++) {
        if (at != line && at[-1] != ' ') continue;
        size_t index = 0;
        while (key[index] != '\0' && at[index] == key[index]) index++;
        if (key[index] != '\0') continue;
        if (at[index] == '\0' || at[index] == ' ') return 1;
    }
    return 0;
}

int boot_verbose(void) {
    static int cached = -1;
    if (cached < 0) cached = boot_command_line_flag("verbose");
    return cached;
}
