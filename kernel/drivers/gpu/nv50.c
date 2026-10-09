#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/framebuffer.h>
#include <tunix/nv50.h>
#include <tunix/pci.h>
#include <tunix/time.h>
#include <tunix/vmm.h>

#define PMC_INTR            0x000100U
#define PMC_INTR_EN         0x000140U
#define PBUS_PRAMIN_BASE    0x001700U
#define PBUS_BAR1_VM        0x001704U
#define PBUS_FLUSH          0x070000U
#define PRAMIN_WINDOW       0x700000U
#define PRAMIN_WINDOW_BYTES 0x100000U
#define PDISP_OBJECTS       0x610010U
#define PDISP_INTR_0        0x610020U
#define PDISP_INTR_1        0x610024U
#define PDISP_INTR_EN_0     0x610028U
#define PDISP_INTR_EN_1     0x61002CU
#define PDISP_SUPERVISOR    0x610030U
#define PDISP_ERROR_ADDR    0x610080U
#define PDISP_ERROR_DATA    0x610084U
#define PDISP_CAP           0x610184U
#define PDISP_CORE_CTRL     0x610200U
#define PDISP_CORE_PUSH     0x610204U
#define PDISP_CORE_UNK08    0x610208U
#define PDISP_CORE_UNK0C    0x61020CU
#define PDISP_VGA_OWNER     0x6194E8U
#define PDISP_CORE_PUT      0x640000U
#define PDISP_CORE_GET      0x640004U
#define PDISP_VBIOS_OWNS    0x00000100U

#define VRAM_INSTANCE       0x00800000U
#define VRAM_INSTANCE_BYTES 0x10000U
#define VRAM_PUSH           0x00810000U
#define VRAM_SYNC           0x00811000U
#define VRAM_TEST           0x00820000U
#define RAMHT_BYTES         0x1000U
#define RAMHT_BITS          9U
#define OBJECT_VRAM         0x1000U
#define OBJECT_SYNC         0x1020U
#define HANDLE_SYNC         0xF0000000U
#define HANDLE_VRAM         0xF0000001U
#define DMA_IN_MEMORY       0x3DU
#define DMA_TARGET_VRAM     0x00010000U
#define DMA_ACCESS_RW       0x00080000U
#define DMA_PRIV_US         (1U << 20)
#define DMA_PART_256        (1U << 16)

#define CORE_UPDATE                 0x0080U
#define CORE_SET_NOTIFIER_CONTROL   0x0084U
#define CORE_SET_CONTEXT_DMA_NOTIFY 0x0088U
#define CORE_GET_CAPABILITIES       0x008CU
#define NOTIFY_ENABLE               0x80000000U
#define TIMEOUT_NS                  2000000000ULL

struct probe {
    uint64_t bar0;
    uint64_t bar1;
    uint32_t vram_bytes;
    uint32_t pramin_saved;
    char *log;
    size_t used;
    size_t capacity;
};

static void say(struct probe *probe, const char *text) {
    while (*text && probe->used + 1 < probe->capacity) probe->log[probe->used++] = *text++;
    probe->log[probe->used] = '\0';
}

static void say_hex(struct probe *probe, uint64_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    char out[20];
    unsigned count = 0;
    out[count++] = '0';
    out[count++] = 'x';
    while (digits--) out[count++] = alphabet[(value >> (digits * 4U)) & 0xFU];
    out[count] = '\0';
    say(probe, out);
}

static void say_reg(struct probe *probe, const char *name, uint32_t offset, uint32_t value) {
    say(probe, "  ");
    say(probe, name);
    say(probe, " [");
    say_hex(probe, offset, 6);
    say(probe, "] = ");
    say_hex(probe, value, 8);
    say(probe, "\n");
}

static uint32_t rd32(struct probe *probe, uint32_t offset) {
    return *(volatile const uint32_t *)(probe->bar0 + offset);
}

static void wr32(struct probe *probe, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(probe->bar0 + offset) = value;
}

static void mask32(struct probe *probe, uint32_t offset, uint32_t mask, uint32_t value) {
    wr32(probe, offset, (rd32(probe, offset) & ~mask) | value);
}

static void show(struct probe *probe, const char *name, uint32_t offset) {
    say_reg(probe, name, offset, rd32(probe, offset));
}

static int wait_clear(struct probe *probe, uint32_t offset, uint32_t mask) {
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (rd32(probe, offset) & mask) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    return 0;
}

static void pramin_select(struct probe *probe, uint32_t vram) {
    wr32(probe, PBUS_PRAMIN_BASE, vram >> 16);
}

static uint32_t vram_rd32(struct probe *probe, uint32_t vram) {
    return rd32(probe, PRAMIN_WINDOW + (vram - VRAM_INSTANCE));
}

static void vram_wr32(struct probe *probe, uint32_t vram, uint32_t value) {
    wr32(probe, PRAMIN_WINDOW + (vram - VRAM_INSTANCE), value);
}

static int vram_flush(struct probe *probe) {
    wr32(probe, PBUS_FLUSH, 0x00000001U);
    return wait_clear(probe, PBUS_FLUSH, 0x00000002U);
}

static void state(struct probe *probe, const char *when) {
    say(probe, when);
    say(probe, "\n");
    show(probe, "pmc intr       ", PMC_INTR);
    show(probe, "pmc intr en    ", PMC_INTR_EN);
    show(probe, "pramin base    ", PBUS_PRAMIN_BASE);
    show(probe, "bar1 vm        ", PBUS_BAR1_VM);
    show(probe, "disp objects   ", PDISP_OBJECTS);
    show(probe, "disp intr 0    ", PDISP_INTR_0);
    show(probe, "disp intr 1    ", PDISP_INTR_1);
    show(probe, "disp intr en 0 ", PDISP_INTR_EN_0);
    show(probe, "disp intr en 1 ", PDISP_INTR_EN_1);
    show(probe, "supervisor     ", PDISP_SUPERVISOR);
    show(probe, "core ctrl      ", PDISP_CORE_CTRL);
    show(probe, "core push      ", PDISP_CORE_PUSH);
    show(probe, "core 208       ", PDISP_CORE_UNK08);
    show(probe, "core 20c       ", PDISP_CORE_UNK0C);
    show(probe, "vga owner      ", PDISP_VGA_OWNER);
    show(probe, "core put       ", PDISP_CORE_PUT);
    show(probe, "core get       ", PDISP_CORE_GET);
    show(probe, "core error adr ", PDISP_ERROR_ADDR);
    show(probe, "core error data", PDISP_ERROR_DATA);
}

static int test_vram(struct probe *probe) {
    say(probe, "vram access\n");
    static const uint32_t pattern[4] = {0x54554E49U, 0x58000001U, 0xA5A55A5AU, 0x0BADF00DU};
    for (unsigned index = 0; index < 4; index++)
        vram_wr32(probe, VRAM_TEST + index * 4U, pattern[index]);
    if (vram_flush(probe) != 0) say(probe, "  flush did not finish\n");
    int pramin_ok = 1;
    for (unsigned index = 0; index < 4; index++)
        pramin_ok &= vram_rd32(probe, VRAM_TEST + index * 4U) == pattern[index];
    say(probe, pramin_ok ? "  pramin read back ok\n" : "  pramin read back WRONG\n");
    if (!pramin_ok) return -1;

    if (!probe->bar1) {
        say(probe, "  bar1 not assigned\n");
        return 0;
    }
    uint64_t bar1 = vmm_map_device(probe->bar1 + VRAM_TEST, 0x1000U);
    if (!bar1) {
        say(probe, "  bar1 not mapped\n");
        return 0;
    }
    volatile uint32_t *linear = (volatile uint32_t *)bar1;
    int bar1_sees = 1;
    for (unsigned index = 0; index < 4; index++) bar1_sees &= linear[index] == pattern[index];
    linear[4] = 0x0D15EA5EU;
    (void)linear[4];
    int pramin_sees = vram_rd32(probe, VRAM_TEST + 16U) == 0x0D15EA5EU;
    say(probe,
        bar1_sees ? "  bar1 reads what pramin wrote: linear\n"
                  : "  bar1 does not read what pramin wrote\n");
    say(probe,
        pramin_sees ? "  pramin reads what bar1 wrote\n"
                    : "  pramin does not read what bar1 wrote\n");
    say_reg(probe, "bar1 word 0    ", VRAM_TEST, linear[0]);
    return 0;
}

static uint32_t ramht_hash(uint32_t handle, uint32_t channel) {
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1U << RAMHT_BITS) - 1U);
        handle >>= RAMHT_BITS;
    }
    return hash ^ (channel << (RAMHT_BITS - 4U));
}

static void write_ctxdma(struct probe *probe, uint32_t object, uint32_t start, uint32_t limit) {
    uint32_t base = VRAM_INSTANCE + object;
    vram_wr32(probe, base + 0x00U, DMA_PRIV_US | DMA_ACCESS_RW | DMA_TARGET_VRAM | DMA_IN_MEMORY);
    vram_wr32(probe, base + 0x04U, limit);
    vram_wr32(probe, base + 0x08U, start);
    vram_wr32(probe, base + 0x0CU, 0);
    vram_wr32(probe, base + 0x10U, 0);
    vram_wr32(probe, base + 0x14U, DMA_PART_256);
}

static void bind_object(struct probe *probe, uint32_t handle, uint32_t object) {
    uint32_t slot = ramht_hash(handle, 0);
    while (vram_rd32(probe, VRAM_INSTANCE + slot * 8U)) slot = (slot + 1U) % (RAMHT_BYTES / 8U);
    vram_wr32(probe, VRAM_INSTANCE + slot * 8U, handle);
    vram_wr32(probe, VRAM_INSTANCE + slot * 8U + 4U, object << 10);
    say(probe, "  handle ");
    say_hex(probe, handle, 8);
    say(probe, " -> ramht slot ");
    say_hex(probe, slot, 3);
    say(probe, " object ");
    say_hex(probe, object, 4);
    say(probe, "\n");
}

static int build_objects(struct probe *probe) {
    say(probe, "display objects\n");
    for (uint32_t offset = 0; offset < VRAM_INSTANCE_BYTES; offset += 4U)
        vram_wr32(probe, VRAM_INSTANCE + offset, 0);
    for (uint32_t offset = 0; offset < 0x2000U; offset += 4U)
        vram_wr32(probe, VRAM_PUSH + offset, 0);
    write_ctxdma(probe, OBJECT_VRAM, 0, probe->vram_bytes - 1U);
    write_ctxdma(probe, OBJECT_SYNC, VRAM_SYNC, VRAM_SYNC + 0xFFFU);
    bind_object(probe, HANDLE_VRAM, OBJECT_VRAM);
    bind_object(probe, HANDLE_SYNC, OBJECT_SYNC);
    if (vram_flush(probe) != 0) {
        say(probe, "  flush did not finish\n");
        return -1;
    }
    int ok = vram_rd32(probe, VRAM_INSTANCE + OBJECT_SYNC + 0x08U) == VRAM_SYNC;
    say(probe, ok ? "  objects read back ok\n" : "  objects read back WRONG\n");
    return ok ? 0 : -1;
}

static void take_over(struct probe *probe) {
    say(probe, "take over from the vbios\n");
    wr32(probe, PDISP_CAP, rd32(probe, 0x614004U));
    for (uint32_t head = 0; head < 2; head++)
        for (uint32_t word = 0; word < 4; word++)
            wr32(probe, 0x610190U + head * 0x10U + word * 4U,
                 rd32(probe, 0x616100U + head * 0x800U + word * 4U));
    uint32_t cap = rd32(probe, PDISP_CAP);
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x00100000U << index))
            wr32(probe, 0x6101D0U + index * 4U, rd32(probe, 0x61A000U + index * 0x800U));
    for (uint32_t index = 0; index < 4; index++)
        if (cap & (0x01000000U << index))
            wr32(probe, 0x6101E0U + index * 4U, rd32(probe, 0x61C000U + index * 0x800U));
    for (uint32_t index = 0; index < 3; index++)
        if (cap & (0x10000000U << index))
            wr32(probe, 0x6101F0U + index * 4U, rd32(probe, 0x61E000U + index * 0x800U));
    say_reg(probe, "caps           ", PDISP_CAP, cap);

    if (rd32(probe, PDISP_INTR_1) & PDISP_VBIOS_OWNS) {
        wr32(probe, PDISP_INTR_1, PDISP_VBIOS_OWNS);
        mask32(probe, PDISP_VGA_OWNER, 0x00000001U, 0);
        say(probe,
            wait_clear(probe, PDISP_VGA_OWNER, 0x00000002U) == 0
                ? "  vbios let go of the display\n"
                : "  vbios did NOT let go of the display\n");
    } else {
        say(probe, "  the vbios did not own the display\n");
    }
    show(probe, "vga owner      ", PDISP_VGA_OWNER);
    wr32(probe, PDISP_OBJECTS, (VRAM_INSTANCE >> 8) | 9U);
    show(probe, "disp objects   ", PDISP_OBJECTS);
}

static int start_core(struct probe *probe) {
    say(probe, "core channel\n");
    if ((rd32(probe, PDISP_CORE_CTRL) & 0x009F0000U) == 0x00020000U)
        mask32(probe, PDISP_CORE_CTRL, 0x00800000U, 0x00800000U);
    if ((rd32(probe, PDISP_CORE_CTRL) & 0x003F0000U) == 0x00030000U)
        mask32(probe, PDISP_CORE_CTRL, 0x00600000U, 0x00600000U);
    wr32(probe, PDISP_CORE_PUSH, (VRAM_PUSH >> 8) | 1U);
    wr32(probe, PDISP_CORE_UNK08, 0x00010000U);
    wr32(probe, PDISP_CORE_UNK0C, 0);
    mask32(probe, PDISP_CORE_CTRL, 0x00000010U, 0x00000010U);
    wr32(probe, PDISP_CORE_PUT, 0);
    wr32(probe, PDISP_CORE_CTRL, 0x01000013U);
    int ok = wait_clear(probe, PDISP_CORE_CTRL, 0x80000000U) == 0;
    say(probe, ok ? "  core channel is up\n" : "  core channel did NOT come up\n");
    show(probe, "core ctrl      ", PDISP_CORE_CTRL);
    return ok ? 0 : -1;
}

static uint32_t push_at;

static void push(struct probe *probe, uint32_t method, uint32_t data) {
    vram_wr32(probe, VRAM_PUSH + push_at, (1U << 18) | method);
    vram_wr32(probe, VRAM_PUSH + push_at + 4U, data);
    push_at += 8U;
}

static int kick(struct probe *probe) {
    if (vram_flush(probe) != 0) {
        say(probe, "  flush before kick did not finish\n");
        return -1;
    }
    wr32(probe, PDISP_CORE_PUT, push_at);
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (rd32(probe, PDISP_CORE_GET) != push_at) {
        if (time_uptime_ns() > deadline) {
            say(probe, "  the channel did not consume the push buffer\n");
            return -1;
        }
        cpu_relax();
    }
    return 0;
}

static int query_capabilities(struct probe *probe) {
    say(probe, "capabilities\n");
    for (uint32_t offset = 0; offset < 0x100U; offset += 4U)
        vram_wr32(probe, VRAM_SYNC + offset, 0);
    push_at = 0;
    push(probe, CORE_SET_CONTEXT_DMA_NOTIFY, HANDLE_SYNC);
    push(probe, CORE_SET_NOTIFIER_CONTROL, NOTIFY_ENABLE);
    push(probe, CORE_GET_CAPABILITIES, 0);
    push(probe, CORE_SET_NOTIFIER_CONTROL, 0);
    int kicked = kick(probe) == 0;
    say(probe, kicked ? "  push buffer consumed\n" : "");
    uint64_t deadline = time_uptime_ns() + TIMEOUT_NS;
    while (!(vram_rd32(probe, VRAM_SYNC + 4U) & 1U) && time_uptime_ns() < deadline) cpu_relax();
    int done = vram_rd32(probe, VRAM_SYNC + 4U) & 1U;
    say(probe,
        done ? "  capabilities notifier written\n" : "  capabilities notifier NOT written\n");
    for (uint32_t offset = 0; offset < 0x54U; offset += 4U)
        say_reg(probe, "notifier       ", offset, vram_rd32(probe, VRAM_SYNC + offset));
    return kicked && done ? 0 : -1;
}

size_t nv50_display_probe(const struct pci_device *device, uint64_t bar0, char *log,
                          size_t capacity) {
    struct probe probe = {0};
    probe.bar0 = bar0;
    probe.bar1 = pci_bar_address(device, 1);
    probe.log = log;
    probe.capacity = capacity;
    if (!capacity) return 0;
    log[0] = '\0';

    uint32_t vram = rd32(&probe, 0x10020CU);
    probe.vram_bytes = vram & 0xFFFFFF00U;
    say(&probe, "nv50 display probe\n");
    say_reg(&probe, "boot0          ", 0, rd32(&probe, 0));
    say_reg(&probe, "vram size      ", 0x10020CU, vram);
    say(&probe, "  framebuffer   ");
    say_hex(&probe, framebuffer_physical_address(), 16);
    say(&probe, " bar1 ");
    say_hex(&probe, probe.bar1, 16);
    say(&probe, "\n");

    probe.pramin_saved = rd32(&probe, PBUS_PRAMIN_BASE);
    state(&probe, "before");
    pramin_select(&probe, VRAM_INSTANCE);

    int ok = test_vram(&probe) == 0 && build_objects(&probe) == 0;
    if (ok) {
        take_over(&probe);
        ok = start_core(&probe) == 0 && query_capabilities(&probe) == 0;
    }

    state(&probe, "after");
    wr32(&probe, PBUS_PRAMIN_BASE, probe.pramin_saved);
    say(&probe, ok ? "result: the core channel works\n" : "result: stopped at the first failure\n");
    return probe.used;
}
