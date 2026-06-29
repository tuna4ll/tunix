#include <stddef.h>
#include <stdint.h>
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/procfs.h"
#include "include/time.h"
#include "include/vfs.h"
#include "include/vmm.h"
#include "include/net/inet_socket.h"
#include "include/net/net.h"

#define PROC_BUFFER_SIZE 4096

struct text_buffer {
    char data[PROC_BUFFER_SIZE];
    size_t length;
};

static void text_char(struct text_buffer *buffer, char value) {
    if (buffer->length + 1 < sizeof(buffer->data)) buffer->data[buffer->length++] = value;
}

static void text_string(struct text_buffer *buffer, const char *value) {
    if (!value) return;
    while (*value) text_char(buffer, *value++);
}

static void text_unsigned(struct text_buffer *buffer, uint64_t value) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    } while (value && count < sizeof(digits));
    while (count) text_char(buffer, digits[--count]);
}

static void text_signed(struct text_buffer *buffer, int64_t value) {
    if (value < 0) {
        text_char(buffer, '-');
        text_unsigned(buffer, (uint64_t)(-(value + 1)) + 1ULL);
    } else {
        text_unsigned(buffer, (uint64_t)value);
    }
}

static int64_t text_read(const struct text_buffer *text, uint64_t offset,
                         size_t size, void *output) {
    if (!output || offset >= text->length) return 0;
    size_t available = text->length - (size_t)offset;
    if (size > available) size = available;
    memcpy(output, text->data + offset, size);
    return (int64_t)size;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static void cpu_vendor(char output[13]) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    (void)a;
    memcpy(output + 0, &b, 4);
    memcpy(output + 4, &d, 4);
    memcpy(output + 8, &c, 4);
    output[12] = '\0';
}

static void cpu_model(char output[49]) {
    uint32_t a, b, c, d;
    cpuid(0x80000000U, 0, &a, &b, &c, &d);
    if (a < 0x80000004U) {
        strncpy(output, "x86_64 processor", 49);
        return;
    }
    uint32_t *words = (uint32_t *)(void *)output;
    for (uint32_t leaf = 0; leaf < 3; leaf++) {
        cpuid(0x80000002U + leaf, 0, &a, &b, &c, &d);
        words[leaf * 4 + 0] = a;
        words[leaf * 4 + 1] = b;
        words[leaf * 4 + 2] = c;
        words[leaf * 4 + 3] = d;
    }
    output[48] = '\0';
    size_t begin = 0;
    while (output[begin] == ' ') begin++;
    if (begin) memmove(output, output + begin, strlen(output + begin) + 1);
    size_t length = strlen(output);
    while (length && output[length - 1] == ' ') output[--length] = '\0';
}

static int64_t proc_cpuinfo_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    char vendor[13];
    char model[49];
    cpu_vendor(vendor);
    cpu_model(model);
    uint64_t frequency = time_tsc_frequency();

    text_string(&text, "processor\t: 0\n");
    text_string(&text, "vendor_id\t: "); text_string(&text, vendor); text_char(&text, '\n');
    text_string(&text, "model name\t: "); text_string(&text, model); text_char(&text, '\n');
    text_string(&text, "cpu MHz\t\t: ");
    text_unsigned(&text, frequency / 1000000ULL);
    text_char(&text, '.');
    uint64_t fraction = (frequency % 1000000ULL) / 1000ULL;
    if (fraction < 100) text_char(&text, '0');
    if (fraction < 10) text_char(&text, '0');
    text_unsigned(&text, fraction);
    text_char(&text, '\n');
    text_string(&text, "cpu cores\t: 1\n");
    text_string(&text, "address sizes\t: 48 bits virtual\n");
    text_string(&text, "flags\t\t: fpu tsc msr pae apic mtrr cmov pat mmx fxsr sse sse2 syscall nx lm\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_meminfo_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    uint64_t total = pmm_total_page_count() * 4ULL;
    uint64_t free = pmm_free_page_count() * 4ULL;
    uint64_t used = total >= free ? total - free : 0;

    text_string(&text, "MemTotal:       "); text_unsigned(&text, total); text_string(&text, " kB\n");
    text_string(&text, "MemFree:        "); text_unsigned(&text, free); text_string(&text, " kB\n");
    text_string(&text, "MemAvailable:   "); text_unsigned(&text, free); text_string(&text, " kB\n");
    text_string(&text, "MemUsed:        "); text_unsigned(&text, used); text_string(&text, " kB\n");
    text_string(&text, "Buffers:        0 kB\nCached:         0 kB\n");
    text_string(&text, "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_uptime_read(struct vfs_node *node, uint64_t offset,
                                size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    uint64_t centiseconds = time_uptime_ns() / 10000000ULL;
    text_unsigned(&text, centiseconds / 100ULL);
    text_char(&text, '.');
    uint64_t fraction = centiseconds % 100ULL;
    if (fraction < 10) text_char(&text, '0');
    text_unsigned(&text, fraction);
    text_string(&text, " 0.00\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_version_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "Tunix version 0.1.0 #1 x86_64\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_mounts_read(struct vfs_node *node, uint64_t offset,
                                size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "initramfs / ramfs rw 0 0\n");
    text_string(&text, "devfs /dev devfs rw 0 0\n");
    text_string(&text, "proc /proc proc rw 0 0\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_stat_read(struct vfs_node *node, uint64_t offset,
                              size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    uint64_t uptime_ticks = time_uptime_ns() / 10000000ULL;
    uint64_t runtime_ticks = process_total_runtime_ns() / 10000000ULL;
    uint64_t idle_ticks = uptime_ticks > runtime_ticks ? uptime_ticks - runtime_ticks : 0;
    text_string(&text, "cpu  "); text_unsigned(&text, runtime_ticks);
    text_string(&text, " 0 0 "); text_unsigned(&text, idle_ticks);
    text_string(&text, " 0 0 0 0 0 0\n");
    text_string(&text, "processes "); text_unsigned(&text, process_created_count()); text_char(&text, '\n');
    text_string(&text, "procs_running "); text_unsigned(&text, process_runnable_count()); text_char(&text, '\n');
    text_string(&text, "procs_blocked "); text_unsigned(&text, process_blocked_count()); text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_loadavg_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    uint64_t runnable = process_runnable_count();
    uint64_t total = process_count();
    text_unsigned(&text, runnable); text_string(&text, ".00 ");
    text_unsigned(&text, runnable); text_string(&text, ".00 ");
    text_unsigned(&text, runnable); text_string(&text, ".00 ");
    text_unsigned(&text, runnable); text_char(&text, '/'); text_unsigned(&text, total);
    text_char(&text, ' '); text_unsigned(&text, process_created_count()); text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_net_dev_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "Inter-|   Receive                                                |  Transmit\n");
    text_string(&text, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    text_string(&text, "  eth0: 0 "); text_unsigned(&text, net_rx_packets());
    text_string(&text, " 0 "); text_unsigned(&text, net_rx_dropped());
    text_string(&text, " 0 0 0 0 0 0 "); text_unsigned(&text, net_tx_packets());
    text_string(&text, " 0 0 0 0 0 0 0\n");
    return text_read(&text, offset, size, output);
}

static void text_hex32(struct text_buffer *text, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4)
        text_char(text, digits[(value >> shift) & 15U]);
}

static void text_hex8_lower(struct text_buffer *text, uint8_t value) {
    static const char digits[] = "0123456789abcdef";
    text_char(text, digits[value >> 4]);
    text_char(text, digits[value & 15U]);
}

static void text_ipv4(struct text_buffer *text, uint32_t address) {
    const uint8_t *bytes = (const uint8_t *)&address;
    for (unsigned i = 0; i < 4; i++) {
        if (i) text_char(text, '.');
        text_unsigned(text, bytes[i]);
    }
}

static int64_t proc_net_route_read(struct vfs_node *node, uint64_t offset,
                                    size_t size, void *output) {
    (void)node;
    const struct net_config *cfg = net_get_config();
    struct text_buffer text = {{0}, 0};
    text_string(&text, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    text_string(&text, "eth0\t00000000\t"); text_hex32(&text, cfg->gateway);
    text_string(&text, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
    text_string(&text, "eth0\t"); text_hex32(&text, cfg->address & cfg->netmask);
    text_string(&text, "\t00000000\t0001\t0\t0\t0\t"); text_hex32(&text, cfg->netmask);
    text_string(&text, "\t0\t0\t0\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_net_arp_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "IP address       HW type     Flags       HW address            Mask     Device\n");
    struct net_arp_record records[16];
    size_t count = net_arp_snapshot(records, sizeof(records) / sizeof(records[0]));
    if (count > sizeof(records) / sizeof(records[0])) count = sizeof(records) / sizeof(records[0]);
    for (size_t i = 0; i < count; i++) {
        text_ipv4(&text, records[i].address);
        text_string(&text, "       0x1         0x2         ");
        for (unsigned byte = 0; byte < 6; byte++) {
            if (byte) text_char(&text, ':');
            text_hex8_lower(&text, records[i].mac[byte]);
        }
        text_string(&text, "     *        eth0\n");
    }
    return text_read(&text, offset, size, output);
}

static int64_t proc_net_udp_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    inet_socket_proc_udp(text.data, sizeof(text.data), &text.length);
    return text_read(&text, offset, size, output);
}

static int64_t proc_net_raw_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    inet_socket_proc_raw(text.data, sizeof(text.data), &text.length);
    return text_read(&text, offset, size, output);
}

static int64_t proc_net_tcp_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "  sl  local_address rem_address   st\n");
    return text_read(&text, offset, size, output);
}

static uint64_t node_pid(struct vfs_node *node) {
    return (uint64_t)(uintptr_t)node->data;
}

static const char *state_name(const struct process *process) {
    switch (process->state) {
        case PROCESS_READY: return "R (runnable)";
        case PROCESS_RUNNING: return "R (running)";
        case PROCESS_BLOCKED: return "S (sleeping)";
        case PROCESS_ZOMBIE: return "Z (zombie)";
        default: return "X (dead)";
    }
}

static char state_code(const struct process *process) {
    if (process->state == PROCESS_BLOCKED) return 'S';
    if (process->state == PROCESS_ZOMBIE) return 'Z';
    if (process->state == PROCESS_DEAD) return 'X';
    return 'R';
}

static int64_t proc_status_read(struct vfs_node *node, uint64_t offset,
                                size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process) return 0;
    struct text_buffer text = {{0}, 0};
    uint64_t pages = vmm_count_user_pages(process->cr3);
    uint64_t memory_kb = pages * 4ULL;

    text_string(&text, "Name:\t"); text_string(&text, process->name); text_char(&text, '\n');
    text_string(&text, "State:\t"); text_string(&text, state_name(process)); text_char(&text, '\n');
    text_string(&text, "Pid:\t"); text_unsigned(&text, process->pid); text_char(&text, '\n');
    text_string(&text, "PPid:\t"); text_unsigned(&text, process->ppid); text_char(&text, '\n');
    text_string(&text, "Pgid:\t"); text_unsigned(&text, process->pgid); text_char(&text, '\n');
    text_string(&text, "Sid:\t"); text_unsigned(&text, process->sid); text_char(&text, '\n');
    text_string(&text, "VmSize:\t"); text_unsigned(&text, memory_kb); text_string(&text, " kB\n");
    text_string(&text, "VmRSS:\t"); text_unsigned(&text, memory_kb); text_string(&text, " kB\n");
    text_string(&text, "Threads:\t1\n");
    text_string(&text, "StartTicks:\t"); text_unsigned(&text, process->start_time_ns / 10000000ULL); text_char(&text, '\n');
    text_string(&text, "CpuTicks:\t"); text_unsigned(&text, process_runtime_ns(process) / 10000000ULL); text_char(&text, '\n');
    text_string(&text, "Command:\t"); text_string(&text, process->exe_path); text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_pid_stat_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process) return 0;
    struct text_buffer text = {{0}, 0};
    uint64_t rss_pages = vmm_count_user_pages(process->cr3);
    uint64_t ticks = process_runtime_ns(process) / 10000000ULL;
    uint64_t start = process->start_time_ns / 10000000ULL;

    text_unsigned(&text, process->pid);
    text_string(&text, " ("); text_string(&text, process->name); text_string(&text, ") ");
    text_char(&text, state_code(process)); text_char(&text, ' ');
    text_unsigned(&text, process->ppid); text_char(&text, ' ');
    text_unsigned(&text, process->pgid); text_char(&text, ' ');
    text_unsigned(&text, process->sid);
    text_string(&text, " 0 0 0 0 0 0 0 0 0 ");
    text_unsigned(&text, ticks);
    text_string(&text, " 0 0 0 20 0 1 0 ");
    text_unsigned(&text, start); text_char(&text, ' ');
    text_unsigned(&text, rss_pages * 4096ULL); text_char(&text, ' ');
    text_signed(&text, (int64_t)rss_pages);
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_cmdline_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process || offset >= process->cmdline_length) return 0;
    size_t available = (size_t)(process->cmdline_length - offset);
    if (size > available) size = available;
    memcpy(output, process->cmdline + offset, size);
    return (int64_t)size;
}

static struct vfs_node *virtual_file(struct vfs_node *parent, const char *name,
                                     vfs_read_fn reader, uint64_t pid) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE);
    if (!node) return NULL;
    node->mode = 0444;
    node->length = PROC_BUFFER_SIZE;
    node->read = reader;
    node->data = (void *)(uintptr_t)pid;
    if (vfs_attach(parent, node) != 0) return NULL;
    return node;
}

static void decimal_path(uint64_t pid, const char *suffix, char output[64]) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + pid % 10ULL);
        pid /= 10ULL;
    } while (pid);
    size_t at = 0;
    output[at++] = '/'; output[at++] = 'p'; output[at++] = 'r'; output[at++] = 'o'; output[at++] = 'c'; output[at++] = '/';
    while (count) output[at++] = digits[--count];
    if (suffix) while (*suffix && at + 1 < 64) output[at++] = *suffix++;
    output[at] = '\0';
}

void procfs_init(void) {
    struct vfs_node *root = vfs_mkdir_p("/proc");
    if (!root) return;
    root->mode = 0555;
    virtual_file(root, "cpuinfo", proc_cpuinfo_read, 0);
    virtual_file(root, "meminfo", proc_meminfo_read, 0);
    virtual_file(root, "uptime", proc_uptime_read, 0);
    virtual_file(root, "version", proc_version_read, 0);
    virtual_file(root, "mounts", proc_mounts_read, 0);
    virtual_file(root, "stat", proc_stat_read, 0);
    virtual_file(root, "loadavg", proc_loadavg_read, 0);

    struct vfs_node *net = vfs_mkdir_p("/proc/net");
    if (net) {
        net->mode = 0555;
        virtual_file(net, "dev", proc_net_dev_read, 0);
        virtual_file(net, "route", proc_net_route_read, 0);
        virtual_file(net, "arp", proc_net_arp_read, 0);
        virtual_file(net, "udp", proc_net_udp_read, 0);
        virtual_file(net, "raw", proc_net_raw_read, 0);
        virtual_file(net, "tcp", proc_net_tcp_read, 0);
        virtual_file(net, "udp6", proc_net_tcp_read, 0);
        virtual_file(net, "raw6", proc_net_tcp_read, 0);
        virtual_file(net, "tcp6", proc_net_tcp_read, 0);
    }
}

void procfs_register_process(struct process *process) {
    if (!process) return;
    char path[64];
    decimal_path(process->pid, NULL, path);
    struct vfs_node *directory = vfs_mkdir_p(path);
    if (!directory) return;
    directory->mode = 0555;
    if (!vfs_find_child(directory, "status")) virtual_file(directory, "status", proc_status_read, process->pid);
    if (!vfs_find_child(directory, "stat")) virtual_file(directory, "stat", proc_pid_stat_read, process->pid);
    if (!vfs_find_child(directory, "cmdline")) virtual_file(directory, "cmdline", proc_cmdline_read, process->pid);
}

void procfs_unregister_process(uint64_t pid) {
    char path[64];
    decimal_path(pid, "/status", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/stat", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/cmdline", path); (void)vfs_remove(path, 0);
    decimal_path(pid, NULL, path); (void)vfs_remove(path, 1);
}
