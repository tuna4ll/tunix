#ifndef TUNIX_PROCUTIL_H
#define TUNIX_PROCUTIL_H

#include <stddef.h>
#include <stdint.h>

#define PROC_MAX 64

struct proc_info {
    uint64_t pid;
    uint64_t ppid;
    uint64_t pgid;
    uint64_t sid;
    uint64_t rss_kb;
    uint64_t start_ticks;
    uint64_t cpu_ticks;
    char state;
    char name[64];
    char command[256];
};

struct memory_info {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t used_kb;
};

int proc_list(struct proc_info *items, size_t capacity);
int proc_memory(struct memory_info *memory);
uint64_t proc_uptime_centiseconds(void);
void out_text(const char *text);
void out_number(uint64_t value);
void out_pad_number(uint64_t value, size_t width);
void out_pad_text(const char *text, size_t width);
void out_time_ticks(uint64_t ticks);

#endif
