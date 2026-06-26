#include "tunix_libc.h"
#include "procutil.h"

#define BUFFER_SIZE 4096

static int read_file(const char *path, char *buffer, size_t capacity) {
    if (!buffer || capacity < 2) return -1;
    int fd = t_open(path, T_O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used + 1 < capacity) {
        long amount = t_read(fd, buffer + used, capacity - used - 1);
        if (amount <= 0) break;
        used += (size_t)amount;
    }
    t_close(fd);
    buffer[used] = '\0';
    return (int)used;
}

static int digit(char value) {
    return value >= '0' && value <= '9';
}

static uint64_t parse_number(const char *text) {
    uint64_t value = 0;
    while (*text == ' ' || *text == '\t') text++;
    while (digit(*text)) {
        value = value * 10ULL + (uint64_t)(*text - '0');
        text++;
    }
    return value;
}

static const char *find_field(const char *text, const char *field) {
    size_t length = t_strlen(field);
    while (*text) {
        if (t_strncmp(text, field, length) == 0) return text + length;
        while (*text && *text != '\n') text++;
        if (*text == '\n') text++;
    }
    return 0;
}

static uint64_t field_number(const char *text, const char *field) {
    const char *value = find_field(text, field);
    return value ? parse_number(value) : 0;
}

static void field_text(const char *text, const char *field, char *output, size_t capacity) {
    if (!output || !capacity) return;
    output[0] = '\0';
    const char *value = find_field(text, field);
    if (!value) return;
    while (*value == ' ' || *value == '\t') value++;
    size_t length = 0;
    while (value[length] && value[length] != '\n' && length + 1 < capacity) length++;
    t_memcpy(output, value, length);
    output[length] = '\0';
}

static void path_for_pid(char output[64], uint64_t pid) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + pid % 10ULL);
        pid /= 10ULL;
    } while (pid);
    size_t at = 0;
    const char *prefix = "/proc/";
    while (*prefix) output[at++] = *prefix++;
    while (count) output[at++] = digits[--count];
    const char *suffix = "/status";
    while (*suffix) output[at++] = *suffix++;
    output[at] = '\0';
}

static int numeric_name(const char *name, uint64_t *pid) {
    if (!name || !digit(*name)) return 0;
    uint64_t value = 0;
    while (*name) {
        if (!digit(*name)) return 0;
        value = value * 10ULL + (uint64_t)(*name - '0');
        name++;
    }
    if (!value) return 0;
    *pid = value;
    return 1;
}

static void sort_pids(uint64_t *pids, size_t count) {
    for (size_t i = 1; i < count; i++) {
        uint64_t value = pids[i];
        size_t j = i;
        while (j && pids[j - 1] > value) {
            pids[j] = pids[j - 1];
            j--;
        }
        pids[j] = value;
    }
}

int proc_list(struct proc_info *items, size_t capacity) {
    uint64_t pids[PROC_MAX];
    size_t pid_count = 0;
    int fd = t_open("/proc", T_O_RDONLY | T_O_DIRECTORY, 0);
    if (fd < 0) return -1;
    unsigned char buffer[1024];
    for (;;) {
        long amount = t_getdents64(fd, buffer, sizeof(buffer));
        if (amount <= 0) break;
        size_t at = 0;
        while (at < (size_t)amount) {
            struct t_linux_dirent64 *entry = (struct t_linux_dirent64 *)(void *)(buffer + at);
            if (!entry->reclen) break;
            uint64_t pid;
            if (pid_count < PROC_MAX && numeric_name(entry->name, &pid)) pids[pid_count++] = pid;
            at += entry->reclen;
        }
    }
    t_close(fd);
    sort_pids(pids, pid_count);

    size_t count = 0;
    for (size_t index = 0; index < pid_count && count < capacity; index++) {
        char path[64];
        char text[BUFFER_SIZE];
        path_for_pid(path, pids[index]);
        if (read_file(path, text, sizeof(text)) < 0) continue;
        struct proc_info *item = &items[count];
        t_memset(item, 0, sizeof(*item));
        item->pid = field_number(text, "Pid:");
        item->ppid = field_number(text, "PPid:");
        item->pgid = field_number(text, "Pgid:");
        item->sid = field_number(text, "Sid:");
        item->rss_kb = field_number(text, "VmRSS:");
        item->start_ticks = field_number(text, "StartTicks:");
        item->cpu_ticks = field_number(text, "CpuTicks:");
        field_text(text, "Name:", item->name, sizeof(item->name));
        field_text(text, "Command:", item->command, sizeof(item->command));
        const char *state = find_field(text, "State:");
        while (state && (*state == ' ' || *state == '\t')) state++;
        item->state = state && *state ? *state : '?';
        if (item->pid) count++;
    }
    return (int)count;
}

int proc_memory(struct memory_info *memory) {
    char text[1024];
    if (!memory || read_file("/proc/meminfo", text, sizeof(text)) < 0) return -1;
    memory->total_kb = field_number(text, "MemTotal:");
    memory->free_kb = field_number(text, "MemFree:");
    memory->available_kb = field_number(text, "MemAvailable:");
    memory->used_kb = field_number(text, "MemUsed:");
    return 0;
}

uint64_t proc_uptime_centiseconds(void) {
    char text[128];
    if (read_file("/proc/uptime", text, sizeof(text)) < 0) return 0;
    uint64_t seconds = parse_number(text);
    const char *cursor = text;
    while (*cursor && *cursor != '.') cursor++;
    uint64_t fraction = 0;
    if (*cursor == '.') {
        cursor++;
        if (digit(cursor[0])) fraction += (uint64_t)(cursor[0] - '0') * 10ULL;
        if (digit(cursor[1])) fraction += (uint64_t)(cursor[1] - '0');
    }
    return seconds * 100ULL + fraction;
}

void out_text(const char *text) {
    if (text) t_write(1, text, t_strlen(text));
}

void out_number(uint64_t value) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    } while (value);
    while (count) t_write(1, &digits[--count], 1);
}

void out_pad_number(uint64_t value, size_t width) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    } while (value);
    while (width > count) { t_write(1, " ", 1); width--; }
    while (count) t_write(1, &digits[--count], 1);
}

void out_pad_text(const char *text, size_t width) {
    size_t length = t_strlen(text);
    if (length > width) length = width;
    t_write(1, text, length);
    while (length++ < width) t_write(1, " ", 1);
}

void out_time_ticks(uint64_t ticks) {
    uint64_t total_seconds = ticks / 100ULL;
    uint64_t minutes = total_seconds / 60ULL;
    uint64_t seconds = total_seconds % 60ULL;
    out_number(minutes);
    out_text(":");
    if (seconds < 10) out_text("0");
    out_number(seconds);
}
