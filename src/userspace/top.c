#include "tunix_libc.h"
#include "procutil.h"

struct sample {
    uint64_t pid;
    uint64_t ticks;
};

static uint64_t previous_ticks(struct sample *samples, int count, uint64_t pid) {
    for (int index = 0; index < count; index++) if (samples[index].pid == pid) return samples[index].ticks;
    return 0;
}

static void copy_samples(struct sample *samples, struct proc_info *items, int count) {
    for (int index = 0; index < count; index++) {
        samples[index].pid = items[index].pid;
        samples[index].ticks = items[index].cpu_ticks;
    }
}

int main(int argc, char **argv) {
    int one_shot = argc == 3 && t_strcmp(argv[1], "-n") == 0 && t_strcmp(argv[2], "1") == 0;
    struct sample previous[PROC_MAX];
    int previous_count = 0;
    uint64_t previous_uptime = 0;
    t_memset(previous, 0, sizeof(previous));

    for (;;) {
        struct proc_info items[PROC_MAX];
        struct memory_info memory;
        int count = proc_list(items, PROC_MAX);
        uint64_t uptime = proc_uptime_centiseconds();
        if (count < 0 || proc_memory(&memory) != 0) {
            t_puterr("top: cannot read procfs\n");
            return 1;
        }
        uint64_t elapsed = previous_uptime && uptime > previous_uptime ? uptime - previous_uptime : 0;
        uint64_t seconds = uptime / 100ULL;
        uint64_t hours = seconds / 3600ULL;
        uint64_t minutes = (seconds / 60ULL) % 60ULL;
        uint64_t remaining = seconds % 60ULL;

        if (!one_shot) out_text("\033[H\033[2J");
        out_text("Tunix top - up ");
        if (hours < 10) out_text("0");
        out_number(hours);
        out_text(":");
        if (minutes < 10) out_text("0");
        out_number(minutes);
        out_text(":");
        if (remaining < 10) out_text("0");
        out_number(remaining);
        out_text(", tasks: "); out_number((uint64_t)count); out_text("\n");
        out_text("Memory: "); out_number(memory.used_kb); out_text("K used, ");
        out_number(memory.free_kb); out_text("K free, "); out_number(memory.total_kb); out_text("K total\n\n");
        out_text("  PID  PPID S  CPU%    RSS     TIME COMMAND\n");
        for (int index = 0; index < count; index++) {
            uint64_t old = previous_ticks(previous, previous_count, items[index].pid);
            uint64_t delta = items[index].cpu_ticks >= old ? items[index].cpu_ticks - old : 0;
            uint64_t cpu = elapsed ? (delta * 100ULL) / elapsed : 0;
            if (cpu > 999) cpu = 999;
            out_pad_number(items[index].pid, 5); out_text(" ");
            out_pad_number(items[index].ppid, 5); out_text(" ");
            t_write(1, &items[index].state, 1); out_text(" ");
            out_pad_number(cpu, 4); out_text("% ");
            out_pad_number(items[index].rss_kb, 6); out_text("K ");
            out_pad_text("", 1); out_time_ticks(items[index].cpu_ticks); out_text(" ");
            out_text(items[index].command[0] ? items[index].command : items[index].name);
            out_text("\n");
        }
        if (one_shot) return 0;
        copy_samples(previous, items, count);
        previous_count = count;
        previous_uptime = uptime;
        t_sleep_ms(1000);
    }
}
