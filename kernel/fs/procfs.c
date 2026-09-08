#include <stddef.h>
#include <stdint.h>
#include "../include/ext2.h"
#include "../include/heap.h"
#include "../include/irq.h"
#include "../include/file.h"
#include "../include/kstring.h"
#include "../include/pmm.h"
#include "../include/process.h"
#include "../include/boot.h"
#include "../include/block.h"
#include "../include/input.h"
#include "../include/klock.h"
#include "../include/procfs.h"
#include "../include/random.h"
#include "../include/uts.h"
#include "../include/smp.h"
#include "../include/time.h"
#include "../include/vfs.h"
#include "../include/vmm.h"
#include "../include/net/inet_socket.h"
#include "../include/net/net.h"

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

    /* Emit one stanza per processor. */
    unsigned cpus = smp_cpu_count();
    for (unsigned index = 0; index < cpus; index++) {
        text_string(&text, "processor\t: "); text_unsigned(&text, index); text_char(&text, '\n');
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
        text_string(&text, "cpu cores\t: "); text_unsigned(&text, cpus); text_char(&text, '\n');
        text_string(&text, "address sizes\t: 48 bits virtual\n");
        text_string(&text, "flags\t\t: fpu tsc msr pae apic mtrr cmov pat mmx fxsr sse sse2 syscall nx lm\n");
        text_char(&text, '\n');
    }
    return text_read(&text, offset, size, output);
}

static int64_t proc_meminfo_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    /* Report usable memory. */
    uint64_t total = pmm_usable_page_count() * 4ULL;
    uint64_t free = pmm_free_page_count() * 4ULL;
    uint64_t used = total >= free ? total - free : 0;

    text_string(&text, "MemTotal:       "); text_unsigned(&text, total); text_string(&text, " kB\n");
    text_string(&text, "MemFree:        "); text_unsigned(&text, free); text_string(&text, " kB\n");
    text_string(&text, "MemAvailable:   "); text_unsigned(&text, free); text_string(&text, " kB\n");
    text_string(&text, "MemUsed:        "); text_unsigned(&text, used); text_string(&text, " kB\n");
    text_string(&text, "Buffers:        0 kB\nCached:         0 kB\n");
    text_string(&text, "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
    text_string(&text, "Shmem:          0 kB\nSReclaimable:   0 kB\n");

    /* Report kernel allocations as slab memory. */
    uint64_t heap_reserved = 0;
    uint64_t heap_allocated = 0;
    uint64_t heap_limit = 0;
    heap_stats(&heap_reserved, &heap_allocated, &heap_limit);
    text_string(&text, "Slab:           "); text_unsigned(&text, heap_allocated / 1024ULL); text_string(&text, " kB\n");
    text_string(&text, "SUnreclaim:     "); text_unsigned(&text, heap_reserved / 1024ULL); text_string(&text, " kB\n");
    text_string(&text, "KernelHeapMax:  "); text_unsigned(&text, heap_limit / 1024ULL); text_string(&text, " kB\n");
    return text_read(&text, offset, size, output);
}

/* Report block device costs. */
static int64_t proc_blockstat_read(struct vfs_node *node, uint64_t offset,
                                   size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    uint64_t reads = 0, sectors = 0, nanoseconds = 0, write_failures = 0;
    block_statistics(&reads, &sectors, &nanoseconds, &write_failures);
    text_string(&text, "reads ");
    text_unsigned(&text, reads);
    text_string(&text, "\nsectors ");
    text_unsigned(&text, sectors);
    text_string(&text, "\nwait_ns ");
    text_unsigned(&text, nanoseconds);
    text_string(&text, "\nwrite_failures ");
    text_unsigned(&text, write_failures);
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

/* Report recent input events. */
static int64_t proc_inputlog_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "ms code value readers\n");
    unsigned total = input_key_history_count();
    unsigned first = total > INPUT_KEY_HISTORY ? total - INPUT_KEY_HISTORY : 0;
    for (unsigned index = first; index < total; index++) {
        struct input_key_event event;
        if (input_key_history_at(index, &event) != 0) continue;
        text_unsigned(&text, event.millisecond);
        text_char(&text, ' ');
        text_unsigned(&text, event.code);
        text_char(&text, ' ');
        text_unsigned(&text, event.value);
        text_char(&text, ' ');
        text_unsigned(&text, event.readers);
        text_char(&text, '\n');
    }
    return text_read(&text, offset, size, output);
}

static void text_hex32(struct text_buffer *text, uint32_t value);

/* Report kernel lock timings. */
static int64_t proc_klock_read(struct vfs_node *node, uint64_t offset,
                               size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "note count total_us max_us\n");
    for (unsigned index = 0; index < KLOCK_HOLD_SLOTS; index++) {
        struct klock_hold hold;
        if (klock_statistics(index, &hold) != 0) continue;
        text_string(&text, "0x");
        text_hex32(&text, hold.note);
        text_char(&text, ' ');
        text_unsigned(&text, hold.count);
        text_char(&text, ' ');
        text_unsigned(&text, hold.total_ns / 1000ULL);
        text_char(&text, ' ');
        text_unsigned(&text, hold.max_ns / 1000ULL);
        text_char(&text, '\n');
    }
    return text_read(&text, offset, size, output);
}

/* Toggle lock measurement. */
static int64_t proc_klock_write(struct vfs_node *node, uint64_t offset,
                                size_t size, const void *input) {
    (void)node; (void)offset;
    const char *text = (const char *)input;
    if (size && text && text[0] == '0') klock_statistics_stop();
    else klock_statistics_start();
    return (int64_t)size;
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

/* Report registered interrupt vectors. */
static int64_t proc_interrupts_read(struct vfs_node *node, uint64_t offset,
                                    size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "           CPU0\n");
    for (unsigned slot = 0; slot < IRQ_VECTOR_COUNT; slot++) {
        unsigned vector = 0;
        uint64_t count = 0;
        const char *name = NULL;
        const char *kind = NULL;
        if (irq_describe(slot, &vector, &count, &name, &kind) != 0) break;
        if (!name) continue;
        if (vector < 100U) text_char(&text, ' ');
        text_unsigned(&text, vector);
        text_string(&text, ": ");
        text_unsigned(&text, count);
        text_string(&text, "   ");
        text_string(&text, kind);
        text_string(&text, "   ");
        text_string(&text, name);
        text_char(&text, '\n');
    }
    return text_read(&text, offset, size, output);
}

static int64_t proc_cmdline_line_read(struct vfs_node *node, uint64_t offset,
                                      size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, boot_info()->command_line);
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

/* Expose the writable hostname. */
static int64_t proc_hostname_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, uts_hostname());
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_hostname_write(struct vfs_node *node, uint64_t offset,
                                   size_t size, const void *input) {
    (void)node;
    (void)offset;
    uts_set_hostname((const char *)input, size);
    return (int64_t)size;
}

/* Expose compatibility sysctl values. */
static int64_t proc_knob_read(struct vfs_node *node, uint64_t offset,
                              size_t size, void *output) {
    struct text_buffer text = {{0}, 0};
    text_unsigned(&text, (uint64_t)(uintptr_t)node->data);
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

static int64_t proc_knob_write(struct vfs_node *node, uint64_t offset,
                               size_t size, const void *input) {
    (void)offset;
    const char *at = (const char *)input;
    uint64_t value = 0;
    for (size_t index = 0; index < size && at[index] >= '0' && at[index] <= '9';
         index++)
        value = value * 10U + (uint64_t)(at[index] - '0');
    node->data = (void *)(uintptr_t)value;
    return (int64_t)size;
}

static struct vfs_node *virtual_file(struct vfs_node *parent, const char *name,
                                     vfs_read_fn reader, uint64_t pid);

static void knob(struct vfs_node *parent, const char *name, uint64_t value) {
    struct vfs_node *node = virtual_file(parent, name, proc_knob_read, value);
    if (!node) return;
    node->mode = 0644;
    node->write = proc_knob_write;
}

static int64_t proc_ostype_read(struct vfs_node *node, uint64_t offset,
                                size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "Tunix\n");
    return text_read(&text, offset, size, output);
}

static int64_t proc_osrelease_read(struct vfs_node *node, uint64_t offset,
                                   size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, "0.1.0\n");
    return text_read(&text, offset, size, output);
}

static char boot_id[37];

/* Generate one UUID per boot. */
static void create_boot_id(void) {
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[16];
    random_get_bytes(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3FU) | 0x80U);

    size_t at = 0;
    for (size_t index = 0; index < sizeof(bytes); index++) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            boot_id[at++] = '-';
        boot_id[at++] = hex[bytes[index] >> 4];
        boot_id[at++] = hex[bytes[index] & 0x0FU];
    }
    boot_id[at] = '\0';
    memset(bytes, 0, sizeof(bytes));
}

static int64_t proc_boot_id_read(struct vfs_node *node, uint64_t offset,
                                 size_t size, void *output) {
    (void)node;
    struct text_buffer text = {{0}, 0};
    text_string(&text, boot_id);
    text_char(&text, '\n');
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
    for (const struct vfs_mount *mount = vfs_mounts(); mount; mount = mount->next) {
        text_string(&text, mount->source[0] ? mount->source : "none");
        text_char(&text, ' ');
        text_string(&text, mount->target);
        text_char(&text, ' ');
        text_string(&text, mount->type);
        text_char(&text, ' ');
        text_string(&text, (mount->flags & VFS_MS_RDONLY) ? "ro" : "rw");
        if (mount->flags & VFS_MS_NOSUID) text_string(&text, ",nosuid");
        if (mount->flags & VFS_MS_NODEV) text_string(&text, ",nodev");
        if (mount->flags & VFS_MS_NOEXEC) text_string(&text, ",noexec");
        text_string(&text, " 0 0\n");
    }
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
    /* Split aggregate time evenly across processors. */
    unsigned cpus = smp_cpu_count();
    for (unsigned index = 0; index < cpus; index++) {
        text_string(&text, "cpu"); text_unsigned(&text, index); text_char(&text, ' ');
        text_unsigned(&text, runtime_ticks / cpus);
        text_string(&text, " 0 0 "); text_unsigned(&text, idle_ticks / cpus);
        text_string(&text, " 0 0 0 0 0 0\n");
    }
    uint64_t now = time_epoch_seconds();
    uint64_t uptime_seconds = time_uptime_ns() / 1000000000ULL;
    text_string(&text, "btime ");
    text_unsigned(&text, now > uptime_seconds ? now - uptime_seconds : 0);
    text_char(&text, '\n');
    text_string(&text, "ctxt 0\n");
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
    inet_socket_proc_tcp(text.data, sizeof(text.data), &text.length);
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
    text_string(&text, "Tgid:\t"); text_unsigned(&text, process->tgid); text_char(&text, '\n');
    text_string(&text, "Pid:\t"); text_unsigned(&text, process->pid); text_char(&text, '\n');
    text_string(&text, "PPid:\t"); text_unsigned(&text, process->ppid); text_char(&text, '\n');
    text_string(&text, "Uid:\t");
    text_unsigned(&text, process->cred.uid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.euid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.suid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.fsuid); text_char(&text, '\n');
    text_string(&text, "Gid:\t");
    text_unsigned(&text, process->cred.gid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.egid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.sgid); text_char(&text, '\t');
    text_unsigned(&text, process->cred.fsgid); text_char(&text, '\n');
    text_string(&text, "Groups:\t");
    for (uint32_t index = 0; index < process->cred.group_count; index++) {
        text_unsigned(&text, process->cred.groups[index]);
        text_char(&text, ' ');
    }
    text_char(&text, '\n');
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

/* Emit all 52 Linux process stat fields. */
static int64_t proc_pid_stat_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process) return 0;
    struct text_buffer text = {{0}, 0};
    uint64_t rss_pages = vmm_count_user_pages(process->cr3);
    uint64_t ticks = process_runtime_ns(process) / 10000000ULL;
    uint64_t start = process->start_time_ns / 10000000ULL;

    text_unsigned(&text, process->pid);                                 /* (1) pid */
    text_string(&text, " ("); text_string(&text, process->name);
    text_string(&text, ") ");                                           /* (2) comm */
    text_char(&text, state_code(process)); text_char(&text, ' ');       /* (3) state */
    text_unsigned(&text, process->ppid); text_char(&text, ' ');         /* (4) ppid */
    text_unsigned(&text, process->pgid); text_char(&text, ' ');         /* (5) pgrp */
    text_unsigned(&text, process->sid); text_char(&text, ' ');          /* (6) session */
    text_string(&text, "0 ");                                           /* (7) tty_nr */
    text_signed(&text, -1); text_char(&text, ' ');                      /* (8) tpgid */
    text_string(&text, "0 ");                                           /* (9) flags */
    text_string(&text, "0 0 0 0 ");                     /* (10-13) min/maj faults */
    text_unsigned(&text, ticks); text_char(&text, ' ');                 /* (14) utime */
    text_string(&text, "0 ");                                           /* (15) stime */
    text_string(&text, "0 0 ");                                  /* (16-17) c[us]time */
    text_string(&text, "20 ");                                          /* (18) priority */
    text_string(&text, "0 ");                                           /* (19) nice */
    text_string(&text, "1 ");                                           /* (20) num_threads */
    text_string(&text, "0 ");                                           /* (21) itrealvalue */
    text_unsigned(&text, start); text_char(&text, ' ');                 /* (22) starttime */
    text_unsigned(&text, rss_pages * 4096ULL); text_char(&text, ' ');   /* (23) vsize */
    text_unsigned(&text, rss_pages); text_char(&text, ' ');             /* (24) rss */
    /* Keep unsupported stat fields zeroed. */
    text_string(&text, "0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
    text_string(&text, "0 ");                                           /* (39) processor */
    text_string(&text, "0 0 ");                          /* (40-41) rt_priority, policy */
    text_string(&text, "0 0 0 ");            /* (42-44) blkio, guest times */
    text_string(&text, "0 0 0 0 0 0 0 ");        /* (45-51) start/end data, brk, args, env */
    text_string(&text, "0");                                            /* (52) exit_code */
    text_char(&text, '\n');
    return text_read(&text, offset, size, output);
}

/* Emit Linux process memory fields. */
static int64_t proc_pid_statm_read(struct vfs_node *node, uint64_t offset,
                                   size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process) return 0;
    struct text_buffer text = {{0}, 0};
    uint64_t pages = vmm_count_user_pages(process->cr3);

    text_unsigned(&text, pages); text_char(&text, ' ');   /* size */
    text_unsigned(&text, pages); text_char(&text, ' ');   /* resident */
    text_string(&text, "0 ");                             /* shared */
    text_string(&text, "0 ");                             /* text */
    text_string(&text, "0 ");                             /* lib */
    text_unsigned(&text, pages); text_char(&text, ' ');   /* data */
    text_string(&text, "0\n");                            /* dt */
    return text_read(&text, offset, size, output);
}

static int64_t proc_pid_comm_read(struct vfs_node *node, uint64_t offset,
                                  size_t size, void *output) {
    struct process *process = process_find(node_pid(node));
    if (!process) return 0;
    struct text_buffer text = {{0}, 0};
    text_string(&text, process->name);
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

#define PROC_PATH_MAX 96

static size_t path_append_string(char *output, size_t at, const char *text) {
    while (*text && at + 1 < PROC_PATH_MAX) output[at++] = *text++;
    return at;
}

static size_t path_append_decimal(char *output, size_t at, uint64_t value) {
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10ULL);
        value /= 10ULL;
    } while (value);
    while (count && at + 1 < PROC_PATH_MAX) output[at++] = digits[--count];
    return at;
}

/* Build a process path. */
static void decimal_path(uint64_t pid, const char *suffix, char output[PROC_PATH_MAX]) {
    size_t at = path_append_string(output, 0, "/proc/");
    at = path_append_decimal(output, at, pid);
    if (suffix) at = path_append_string(output, at, suffix);
    output[at] = '\0';
}

/* Build a process task path. */
static void task_path(uint64_t pid, const char *suffix, char output[PROC_PATH_MAX]) {
    size_t at = path_append_string(output, 0, "/proc/");
    at = path_append_decimal(output, at, pid);
    at = path_append_string(output, at, "/task/");
    at = path_append_decimal(output, at, pid);
    if (suffix) at = path_append_string(output, at, suffix);
    output[at] = '\0';
}

/* Describe an open descriptor. */
static void describe_file(const struct file *file, char *output, size_t capacity) {
    output[0] = '\0';
    if (!file) return;
    if (file->kind == FILE_KIND_VFS && file->node) {
        if (vfs_node_path(file->node, output, capacity) != 0) output[0] = '\0';
        return;
    }
    const char *kind = "anon_inode:[unknown]";
    switch (file->kind) {
        case FILE_KIND_PIPE_READ:
        case FILE_KIND_PIPE_WRITE: kind = "pipe:[0]"; break;
        case FILE_KIND_SOCKET:
        case FILE_KIND_INET_SOCKET:
        case FILE_KIND_NETLINK_SOCKET: kind = "socket:[0]"; break;
        case FILE_KIND_PTY_MASTER: kind = "/dev/ptmx"; break;
        case FILE_KIND_EVENTFD: kind = "anon_inode:[eventfd]"; break;
        case FILE_KIND_TIMERFD: kind = "anon_inode:[timerfd]"; break;
        case FILE_KIND_EPOLL: kind = "anon_inode:[eventpoll]"; break;
        case FILE_KIND_INOTIFY: kind = "anon_inode:inotify"; break;
        case FILE_KIND_SIGNALFD: kind = "anon_inode:[signalfd]"; break;
        case FILE_KIND_MEMFD: kind = "/memfd:tunix"; break;
        default: break;
    }
    size_t at = 0;
    while (kind[at] && at + 1 < capacity) { output[at] = kind[at]; at++; }
    output[at] = '\0';
}

static void proc_fd_refresh(struct vfs_node *directory) {
    /* Prevent recursive refreshes. */
    static int busy;
    if (busy) return;
    busy = 1;

    while (directory->children)
        (void)vfs_detach_child(directory, directory->children);

    struct process *process = process_find(node_pid(directory));
    if (process && process->files) {
        for (int fd = 0; fd < PROCESS_MAX_FDS; fd++) {
            struct file *file = process->files->fds[fd];
            if (!file) continue;
            char target[PROC_PATH_MAX];
            describe_file(file, target, sizeof(target));
            if (!target[0]) continue;
            char name[16];
            size_t at = path_append_decimal(name, 0, (uint64_t)fd);
            name[at] = '\0';
            (void)vfs_attach_symlink(directory, name, target);
        }
    }
    busy = 0;
}

static struct vfs_node *direct_child(struct vfs_node *directory, const char *name) {
    for (struct vfs_node *child = directory->children; child; child = child->next)
        if (strcmp(child->name, name) == 0) return child;
    return NULL;
}

static int set_link_target(struct vfs_node *link, const char *target) {
    if (!link || !target || !target[0]) return -1;
    size_t length = strlen(target);
    if (length + 1 > link->capacity) {
        void *data = kmalloc(length + 1);
        if (!data) return -1;
        kfree(link->data);
        link->data = data;
        link->capacity = length + 1;
    }
    memcpy(link->data, target, length + 1);
    link->length = length;
    return 0;
}

/* Refresh mutable process links. */
static void proc_process_refresh(struct vfs_node *directory) {
    struct process *process = process_find(node_pid(directory));
    if (!process) return;

    (void)set_link_target(direct_child(directory, "exe"), process->exe_path);

    char path[256];
    if (vfs_node_path(process->cwd, path, sizeof(path)) == 0)
        (void)set_link_target(direct_child(directory, "cwd"), path);
}

/* Track the calling process links. */
static struct vfs_node *self_link;
static struct vfs_node *thread_self_link;

static void point_at_caller(struct vfs_node *link, uint64_t pid) {
    /* Reuse the preallocated link buffer. */
    if (!link || !link->data || link->capacity < 21) return;
    char *target = (char *)link->data;
    size_t at = path_append_decimal(target, 0, pid);
    target[at] = '\0';
    link->length = at;
}

static void proc_root_refresh(struct vfs_node *directory) {
    (void)directory;
    point_at_caller(self_link, process_current_pid());
    point_at_caller(thread_self_link, process_current_tid());
}

void procfs_init(void) {
    struct vfs_node *root = vfs_mkdir_p("/proc");
    if (!root) return;
    root->mode = 0555;
    vfs_mount_builtin("proc", "/proc", "proc", root);
    /* Reserve room for any process id. */
    self_link = vfs_attach_symlink(root, "self", "18446744073709551615");
    thread_self_link = vfs_attach_symlink(root, "thread-self", "18446744073709551615");
    root->refresh = proc_root_refresh;
    virtual_file(root, "cpuinfo", proc_cpuinfo_read, 0);
    virtual_file(root, "meminfo", proc_meminfo_read, 0);
    virtual_file(root, "uptime", proc_uptime_read, 0);
    virtual_file(root, "blockstat", proc_blockstat_read, 0);
    virtual_file(root, "inputlog", proc_inputlog_read, 0);
    struct vfs_node *klock = virtual_file(root, "klock", proc_klock_read, 0);
    if (klock) {
        klock->mode = 0644;
        klock->write = proc_klock_write;
    }
    virtual_file(root, "version", proc_version_read, 0);
    virtual_file(root, "mounts", proc_mounts_read, 0);
    virtual_file(root, "stat", proc_stat_read, 0);
    virtual_file(root, "loadavg", proc_loadavg_read, 0);
    virtual_file(root, "cmdline", proc_cmdline_line_read, 0);
    virtual_file(root, "interrupts", proc_interrupts_read, 0);

    struct vfs_node *kernel = vfs_mkdir_p("/proc/sys/kernel");
    if (kernel) {
        kernel->mode = 0555;
        struct vfs_node *host = virtual_file(kernel, "hostname", proc_hostname_read, 0);
        if (host) {
            host->mode = 0644;
            host->write = proc_hostname_write;
        }
        virtual_file(kernel, "ostype", proc_ostype_read, 0);
        virtual_file(kernel, "osrelease", proc_osrelease_read, 0);
        knob(kernel, "dmesg_restrict", 0);
        knob(kernel, "kptr_restrict", 0);
        knob(kernel, "core_uses_pid", 0);
        knob(kernel, "kexec_load_disabled", 0);
        knob(kernel, "perf_event_paranoid", 3);
        knob(kernel, "unprivileged_bpf_disabled", 1);
        knob(kernel, "pid_max", 32768);
        knob(kernel, "panic", 0);
    }

    struct vfs_node *yama = vfs_mkdir_p("/proc/sys/kernel/yama");
    if (yama) {
        yama->mode = 0555;
        knob(yama, "ptrace_scope", 1);
    }

    /* Expose entropy values for seedrng. */
    struct vfs_node *random = vfs_mkdir_p("/proc/sys/kernel/random");
    if (random) {
        random->mode = 0555;
        create_boot_id();
        virtual_file(random, "boot_id", proc_boot_id_read, 0);
        knob(random, "poolsize", 256);
        knob(random, "entropy_avail", 256);
    }

    struct vfs_node *fs = vfs_mkdir_p("/proc/sys/fs");
    if (fs) {
        fs->mode = 0555;
        knob(fs, "protected_hardlinks", 1);
        knob(fs, "protected_symlinks", 1);
        knob(fs, "file-max", 65536);
    }

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

static void populate_process_files(struct vfs_node *directory, uint64_t pid) {
    if (!vfs_find_child(directory, "status")) virtual_file(directory, "status", proc_status_read, pid);
    if (!vfs_find_child(directory, "stat")) virtual_file(directory, "stat", proc_pid_stat_read, pid);
    if (!vfs_find_child(directory, "cmdline")) virtual_file(directory, "cmdline", proc_cmdline_read, pid);
    if (!vfs_find_child(directory, "statm")) virtual_file(directory, "statm", proc_pid_statm_read, pid);
    if (!vfs_find_child(directory, "comm")) virtual_file(directory, "comm", proc_pid_comm_read, pid);
}

void procfs_register_process(struct process *process) {
    if (!process) return;
    char path[PROC_PATH_MAX];
    decimal_path(process->pid, NULL, path);
    struct vfs_node *directory = vfs_mkdir_p(path);
    if (!directory) return;
    directory->mode = 0555;
    directory->uid = process->cred.euid;
    directory->gid = process->cred.egid;
    directory->data = (void *)(uintptr_t)process->pid;
    populate_process_files(directory, process->pid);

    char cwd[256];
    if (vfs_node_path(process->cwd, cwd, sizeof(cwd)) != 0) {
        cwd[0] = '/';
        cwd[1] = '\0';
    }
    (void)vfs_attach_symlink(directory, "exe", process->exe_path);
    (void)vfs_attach_symlink(directory, "cwd", cwd);
    (void)vfs_attach_symlink(directory, "root", "/");

    struct vfs_node *descriptors = vfs_find_child(directory, "fd");
    if (!descriptors) {
        decimal_path(process->pid, "/fd", path);
        descriptors = vfs_mkdir_p(path);
        if (descriptors) {
            descriptors->mode = 0500;
            descriptors->uid = process->cred.euid;
            descriptors->gid = process->cred.egid;
            descriptors->data = (void *)(uintptr_t)process->pid;
            descriptors->refresh = proc_fd_refresh;
        }
    }
    directory->refresh = proc_process_refresh;

    /* Mirror each process as its main task. */
    task_path(process->pid, NULL, path);
    struct vfs_node *task = vfs_mkdir_p(path);
    if (!task) return;
    task->mode = 0555;
    populate_process_files(task, process->pid);
}

void procfs_unregister_process(uint64_t pid) {
    char path[PROC_PATH_MAX];
    decimal_path(pid, NULL, path);
    struct vfs_node *directory = vfs_lookup(path);
    if (directory) directory->refresh = NULL;
    task_path(pid, "/status", path); (void)vfs_remove(path, 0);
    task_path(pid, "/stat", path); (void)vfs_remove(path, 0);
    task_path(pid, "/cmdline", path); (void)vfs_remove(path, 0);
    task_path(pid, "/statm", path); (void)vfs_remove(path, 0);
    task_path(pid, "/comm", path); (void)vfs_remove(path, 0);
    task_path(pid, NULL, path); (void)vfs_remove(path, 1);
    decimal_path(pid, "/task", path); (void)vfs_remove(path, 1);
    decimal_path(pid, "/status", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/stat", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/cmdline", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/statm", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/comm", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/exe", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/cwd", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/root", path); (void)vfs_remove(path, 0);
    decimal_path(pid, "/fd", path);
    struct vfs_node *descriptors = vfs_lookup(path);
    if (descriptors) {
        descriptors->refresh = NULL;
        while (descriptors->children)
            (void)vfs_detach_child(descriptors, descriptors->children);
        (void)vfs_remove(path, 1);
    }
    decimal_path(pid, NULL, path); (void)vfs_remove(path, 1);
}
