#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/heap.h>
#include <tunix/pmm.h>
#include <tunix/time.h>
#include <tunix/vmm.h>

#include "priv.h"
#include <tunix/kprintf.h>

// VRAM the driver hands out, and the GPU's command FIFO (PFIFO): a channel
// with a virtual address space of its own, fed through an indirect buffer
// of push buffer segments, as nouveau's nvkm/engine/fifo/{nv50,g84}.c and
// nvkm/subdev/mmu/vmmnv50.c (MIT) set them up for G84 and later.

// ---- VRAM ----------------------------------------------------------------

// First fit over the free ranges, kept sorted and merged.
int nv50_vram_init(struct nv50_vram *vram, uint32_t start, uint32_t end) {
    if (start >= end) return -1;
    vram->ranges[0].start = start;
    vram->ranges[0].end = end;
    vram->count = 1;
    return 0;
}

uint32_t nv50_vram_alloc(struct nv50_vram *vram, uint32_t bytes, uint32_t align) {
    if (!bytes || !align || (align & (align - 1U))) return 0;
    for (unsigned index = 0; index < vram->count; index++) {
        struct nv50_vram_range *range = &vram->ranges[index];
        uint32_t at = (range->start + align - 1U) & ~(align - 1U);
        if (at < range->start || at >= range->end || range->end - at < bytes) continue;
        uint32_t head = range->start, tail = at + bytes, end = range->end;
        if (head == at && tail == end) {
            for (unsigned move = index; move + 1U < vram->count; move++)
                vram->ranges[move] = vram->ranges[move + 1U];
            vram->count--;
        } else if (head == at) {
            range->start = tail;
        } else if (tail == end) {
            range->end = at;
        } else {
            if (vram->count == NV50_VRAM_RANGES) return 0;
            for (unsigned move = vram->count; move > index + 1U; move--)
                vram->ranges[move] = vram->ranges[move - 1U];
            vram->count++;
            range->end = at;
            vram->ranges[index + 1U].start = tail;
            vram->ranges[index + 1U].end = end;
        }
        return at;
    }
    return 0;
}

void nv50_vram_free(struct nv50_vram *vram, uint32_t at, uint32_t bytes) {
    if (!bytes) return;
    uint32_t end = at + bytes;
    unsigned index = 0;
    while (index < vram->count && vram->ranges[index].start < at) index++;
    int before = index > 0 && vram->ranges[index - 1U].end == at;
    int after = index < vram->count && vram->ranges[index].start == end;
    if (before && after) {
        vram->ranges[index - 1U].end = vram->ranges[index].end;
        for (unsigned move = index; move + 1U < vram->count; move++)
            vram->ranges[move] = vram->ranges[move + 1U];
        vram->count--;
    } else if (before) {
        vram->ranges[index - 1U].end = end;
    } else if (after) {
        vram->ranges[index].start = at;
    } else if (vram->count < NV50_VRAM_RANGES) {
        for (unsigned move = vram->count; move > index; move--)
            vram->ranges[move] = vram->ranges[move - 1U];
        vram->ranges[index].start = at;
        vram->ranges[index].end = end;
        vram->count++;
    }
    // shortcut: a free that would need more than NV50_VRAM_RANGES holes is
    // lost; raise the count when buffers come and go by the thousand.
}

uint32_t nv50_vram_free_bytes(const struct nv50_vram *vram) {
    uint32_t total = 0;
    for (unsigned index = 0; index < vram->count; index++)
        total += vram->ranges[index].end - vram->ranges[index].start;
    return total;
}

#ifndef NV50_HOST_TEST

// ---- registers -----------------------------------------------------------

#define PMC_ENABLE          0x000200U
#define PMC_ENABLE_PFIFO    0x00000100U
#define PFIFO_INTR          0x002100U
#define PFIFO_INTR_EN       0x002140U
#define PFIFO_FREEZE        0x002500U
#define PFIFO_TIMESLICE     0x00250CU
#define PFIFO_2044          0x002044U
#define PFIFO_CHAN(id)      (0x002600U + (id) * 4U)
#define PFIFO_CHAN_ON       0x80000000U
#define PFIFO_RUNLIST_BASE  0x0032F4U
#define PFIFO_RUNLIST_COUNT 0x0032ECU
#define PFIFO_RUNLIST_BUSY  0x00000100U
#define PFIFO_CACHE1_PUSH   0x003200U
#define PFIFO_CACHE1_PULL   0x003250U
#define USERD(id)           (0xC00000U + (id) * 0x2000U)
#define USER_IB_GET         0x88U
#define USER_IB_PUT         0x8CU

// A channel's 64 KiB instance block, laid out as nouveau's g84 channel
// allocates it; the page directory is where the hardware reads it.
#define INST_BYTES    0x10000U
#define INST_ENG      0x0000U
#define INST_PD       0x0200U // g84_mmu: G84 and later read it here, G80 at 0x1400
#define INST_CACHE    0x4400U
#define INST_RAMFC    0x5400U
#define INST_DMA_VM   0x5500U
#define INST_CE_CTX   0x5600U // the copy engine's 256 bytes, as nvkm_falcon_cclass allocates
#define ENG_CE        0x00C0U // where g84_ectx_bind points the copy engine at it
#define INST_RAMHT    0x8000U
#define RAMHT_BYTES   0x8000U
#define RAMHT_BITS    12U
#define CHANNEL       1U
#define HANDLE_VM     0xF0000010U
#define HANDLE_COPY   0xF0000020U
#define ENGINE_CE     3U // the copy engine's place on g98's runlist

// 64 KiB pages, each PDE covering 512 MiB with a 64 KiB table of 8192 PTEs.
#define PAGE_SHIFT_64K 16U
#define PDE_SPAN       0x20000000ULL
#define PT_BYTES       0x10000U
#define PDE_64K_VRAM   0x00000001U
#define PTE_VALID      0x00000001U
#define PTE_HOST       0x00000020U // aperture 2: system memory, snooped

// System memory the GPU reads, mapped page by page in the third PDE: 512 MiB
// of 4 KiB pages, a full table of 128K PTEs (1 MiB).
#define GART_BASE      0x40000000ULL
#define GART_BYTES     0x20000000U
#define GART_PT_BYTES  0x100000U
#define PDE_4K_VRAM    0x00000003U
#define PMMU_FLUSH     0x100C80U
#define PMMU_ENGINE_CE 0x0DU

// The push buffer and, after it, the indirect buffer of 1024 segments.
#define PUSH_BYTES 0x10000U
#define IB_BYTES   0x2000U
#define IB_ENTRIES (IB_BYTES / 8U)

#define DMA_OBJECT_VM 0x7FC0003DU // comp, kind and priv from the VM, class 0x3d
#define SUBC_SEMAPHORE_A   0x0010U
#define SET_DMA_SEMAPHORE  0x0060U
#define SEMAPHORE_RELEASE  0x00000002U

static volatile uint32_t *map_vram(const struct nv50_device *gpu, uint32_t at, uint32_t bytes) {
    return (volatile uint32_t *)vmm_map_device(gpu->bar1 + at, bytes);
}

static int bar_flush(const struct nv50_device *gpu) {
    nv50_wr32(gpu, PBUS_FLUSH, 0x00000001U);
    return nv50_wait_clear(gpu, PBUS_FLUSH, 0x00000002U);
}

static void wr64(volatile uint32_t *map, uint32_t offset, uint64_t value) {
    map[offset / 4U] = (uint32_t)value;
    map[offset / 4U + 1U] = (uint32_t)(value >> 32);
}

static uint32_t ramht_hash(uint32_t handle) {
    uint32_t hash = 0;
    while (handle) {
        hash ^= handle & ((1U << RAMHT_BITS) - 1U);
        handle >>= RAMHT_BITS;
    }
    return hash;
}

// The MMU's last fault, as nv50_fb_intr reads it: six words behind 0x100c90.
static void report_trap(const struct nv50_device *gpu) {
    uint32_t index = nv50_rd32(gpu, 0x100C90U);
    if (!(index & 0x80000000U)) return;
    index &= 0x00FFFFFFU;
    uint32_t trap[6];
    for (uint32_t word = 0; word < 6U; word++) {
        nv50_wr32(gpu, 0x100C90U, index | word << 24);
        trap[word] = nv50_rd32(gpu, 0x100C94U);
    }
    nv50_wr32(gpu, 0x100C90U, index | 0x80000000U);
    kprintf("NV50: mmu fault %x %x %x %x %x %x\n", trap[0], trap[1], trap[2], trap[3], trap[4],
            trap[5]);
}

// RAMHT entries as nvkm_ramht_insert places them: hashed, then the next free slot.
static void ramht_add(volatile uint32_t *inst, uint32_t handle, uint32_t context) {
    uint32_t slot = ramht_hash(handle);
    while (inst[(INST_RAMHT + slot * 8U) / 4U]) slot = (slot + 1U) % (RAMHT_BYTES / 8U);
    inst[(INST_RAMHT + slot * 8U) / 4U] = handle;
    inst[(INST_RAMHT + slot * 8U + 4U) / 4U] = context;
}

static void report_fifo(const struct nv50_device *gpu, const char *what) {
    report_trap(gpu);
    kprintf("NV50: fifo %s: intr %x, chan %x, runlist %x, push %x state %x, dma put %x get %x,"
            " ib put %x get %x, user ib %x/%x\n",
            what, nv50_rd32(gpu, PFIFO_INTR), nv50_rd32(gpu, PFIFO_CHAN(CHANNEL)),
            nv50_rd32(gpu, PFIFO_RUNLIST_COUNT), nv50_rd32(gpu, 0x003220U),
            nv50_rd32(gpu, 0x003228U), nv50_rd32(gpu, 0x003240U), nv50_rd32(gpu, 0x003244U),
            nv50_rd32(gpu, 0x003330U), nv50_rd32(gpu, 0x003334U),
            nv50_rd32(gpu, USERD(CHANNEL) + USER_IB_PUT),
            nv50_rd32(gpu, USERD(CHANNEL) + USER_IB_GET));
}

// The engines are reset and the FIFO's own interrupts left off: the display
// alone interrupts, and FIFO errors are read from PFIFO_INTR.
static void fifo_init(const struct nv50_device *gpu) {
    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_PFIFO, 0);
    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_PFIFO, PMC_ENABLE_PFIFO);
    nv50_wr32(gpu, PFIFO_TIMESLICE, 0x6F3CFC34U);
    nv50_wr32(gpu, PFIFO_2044, 0x01003FFFU);
    nv50_wr32(gpu, 0x100C90U, 0x000D0FFFU); // GT215's: MMU faults readable, as nv50_fb_init
    nv50_wr32(gpu, PFIFO_INTR, 0xFFFFFFFFU);
    nv50_wr32(gpu, PFIFO_INTR_EN, 0);
    for (uint32_t id = 0; id < 128U; id++) nv50_wr32(gpu, PFIFO_CHAN(id), 0);
    nv50_wr32(gpu, PFIFO_CACHE1_PUSH, 1);
    nv50_wr32(gpu, PFIFO_CACHE1_PULL, 1);
    nv50_wr32(gpu, PFIFO_FREEZE, 1);
}

// The whole of VRAM at the same address in the channel's space, in 64 KiB
// pages of kind 0 (pitch), contiguous in blocks of 128.
// shortcut: no contiguous-block hint (PTE bits 7-9); with one, every PTE of a
// block holds the block's first address, as nv50_vmm_pgt_pte writes them.
// Add it when TLB misses show up in profiles.
static int build_vm(struct nv50_device *gpu, volatile uint32_t *inst) {
    unsigned pdes = (unsigned)((gpu->vram_bytes + PDE_SPAN - 1U) / PDE_SPAN);
    if (pdes > 2U) pdes = 2U;
    for (unsigned pde = 0; pde < pdes; pde++) {
        uint32_t pt = gpu->fifo.pt[pde];
        volatile uint32_t *map = pt ? map_vram(gpu, pt, PT_BYTES) : NULL;
        if (!map) return -1;
        for (uint32_t pte = 0; pte < PT_BYTES / 8U; pte++) {
            uint64_t page = (uint64_t)pde * PDE_SPAN + ((uint64_t)pte << PAGE_SHIFT_64K);
            wr64(map, pte * 8U, page < gpu->vram_bytes ? page | PTE_VALID : 0);
        }
        wr64(inst, INST_PD + pde * 8U, (uint64_t)pt | PDE_64K_VRAM);
    }
    return 0;
}

int nv50_fifo_start(struct nv50_device *gpu) {
    struct nv50_fifo *fifo = &gpu->fifo;
    fifo->inst = nv50_vram_alloc(&gpu->vram, INST_BYTES, 0x10000U);
    fifo->runlist = nv50_vram_alloc(&gpu->vram, 0x1000U, 0x1000U);
    fifo->ring = nv50_vram_alloc(&gpu->vram, PUSH_BYTES + IB_BYTES, 0x1000U);
    fifo->sema = nv50_vram_alloc(&gpu->vram, 0x1000U, 0x1000U);
    fifo->pt[0] = nv50_vram_alloc(&gpu->vram, PT_BYTES, 0x1000U);
    fifo->pt[1] = nv50_vram_alloc(&gpu->vram, PT_BYTES, 0x1000U);
    fifo->gart_pt = nv50_vram_alloc(&gpu->vram, GART_PT_BYTES, 0x1000U);
    if (!fifo->inst || !fifo->runlist || !fifo->ring || !fifo->sema || !fifo->pt[1] ||
        !fifo->gart_pt)
        return -1;
    fifo->gart_map = map_vram(gpu, fifo->gart_pt, GART_PT_BYTES);
    if (!fifo->gart_map || nv50_vram_init(&fifo->gart, 0x1000U, GART_BYTES) != 0) return -1;
    for (uint32_t at = 0; at < GART_PT_BYTES / 4U; at++) fifo->gart_map[at] = 0;
    volatile uint32_t *inst = map_vram(gpu, fifo->inst, INST_BYTES);
    volatile uint32_t *runlist = map_vram(gpu, fifo->runlist, 0x1000U);
    fifo->ring_map = map_vram(gpu, fifo->ring, PUSH_BYTES + IB_BYTES);
    fifo->sema_map = map_vram(gpu, fifo->sema, 0x1000U);
    if (!inst || !runlist || !fifo->ring_map || !fifo->sema_map) return -1;
    for (uint32_t at = 0; at < INST_BYTES / 4U; at++) inst[at] = 0;
    for (uint32_t at = 0; at < (PUSH_BYTES + IB_BYTES) / 4U; at++) fifo->ring_map[at] = 0;
    fifo->sema_map[0] = 0;
    if (build_vm(gpu, inst) != 0) return -1;
    wr64(inst, INST_PD + (uint32_t)(GART_BASE / PDE_SPAN) * 8U, (uint64_t)fifo->gart_pt | PDE_4K_VRAM);

    // A DMA object over the whole space, for the push buffer and semaphores.
    uint64_t limit = 0xFFFFFFFFFFULL; // the 40-bit space, as nouveau's
    inst[(INST_DMA_VM + 0x00U) / 4U] = DMA_OBJECT_VM;
    inst[(INST_DMA_VM + 0x04U) / 4U] = (uint32_t)limit;
    inst[(INST_DMA_VM + 0x08U) / 4U] = 0;
    inst[(INST_DMA_VM + 0x0CU) / 4U] = (uint32_t)(limit >> 32) << 24;
    inst[(INST_DMA_VM + 0x10U) / 4U] = 0;
    inst[(INST_DMA_VM + 0x14U) / 4U] = 0;
    ramht_add(inst, HANDLE_VM, INST_DMA_VM >> 4);
    // The copy object: no instance of its own (0x40, as nvkm_ramht_update has
    // it), run by the copy engine with its context bound as g84_ectx_bind does.
    ramht_add(inst, HANDLE_COPY, (ENGINE_CE << 20) | (0x40U >> 4));
    uint32_t ce_context = fifo->inst + INST_CE_CTX;
    inst[(INST_ENG + ENG_CE + 0x00U) / 4U] = 0x00190000U;
    inst[(INST_ENG + ENG_CE + 0x04U) / 4U] = ce_context + 0xFFU;
    inst[(INST_ENG + ENG_CE + 0x08U) / 4U] = ce_context;
    inst[(INST_ENG + ENG_CE + 0x0CU) / 4U] = ce_context;
    inst[(INST_ENG + ENG_CE + 0x10U) / 4U] = 0;
    inst[(INST_ENG + ENG_CE + 0x14U) / 4U] = 0;

    uint64_t ib = (uint64_t)fifo->ring + PUSH_BYTES;
    const uint32_t ramfc = INST_RAMFC / 4U;
    inst[ramfc + 0x3CU / 4U] = 0x403F6078U;
    inst[ramfc + 0x44U / 4U] = 0x01003FFFU;
    inst[ramfc + 0x48U / 4U] = INST_DMA_VM >> 4;
    inst[ramfc + 0x50U / 4U] = (uint32_t)ib;
    inst[ramfc + 0x54U / 4U] = (uint32_t)(ib >> 32) | (10U << 16); // log2(1024 segments)
    inst[ramfc + 0x60U / 4U] = 0x7FFFFFFFU;
    inst[ramfc + 0x78U / 4U] = 0;
    inst[ramfc + 0x7CU / 4U] = 0x30000FFFU;
    inst[ramfc + 0x80U / 4U] = ((RAMHT_BITS - 9U) << 27) | (4U << 24) | (INST_RAMHT >> 4);
    inst[ramfc + 0x88U / 4U] = (fifo->inst + INST_CACHE) >> 10;
    inst[ramfc + 0x98U / 4U] = fifo->inst >> 12;
    runlist[0] = CHANNEL;
    (void)inst[0];
    if (bar_flush(gpu) != 0) return -1;

    fifo_init(gpu);
    nv50_wr32(gpu, PFIFO_CHAN(CHANNEL), (fifo->inst + INST_RAMFC) >> 8);
    nv50_mask(gpu, PFIFO_CHAN(CHANNEL), PFIFO_CHAN_ON, PFIFO_CHAN_ON);
    nv50_wr32(gpu, PFIFO_RUNLIST_BASE, fifo->runlist >> 12);
    nv50_wr32(gpu, PFIFO_RUNLIST_COUNT, 1);
    if (nv50_wait_clear(gpu, PFIFO_RUNLIST_COUNT, PFIFO_RUNLIST_BUSY) != 0) {
        report_fifo(gpu, "runlist did not take");
        return -1;
    }
    fifo->put = 0;
    fifo->ib_put = 0;
    fifo->sequence = 0;
    return 0;
}

static void push_on(struct nv50_fifo *fifo, unsigned subchannel, uint32_t method, unsigned count,
                    const uint32_t *data) {
    volatile uint32_t *ring = fifo->ring_map;
    ring[fifo->put++] = (count << 18) | (subchannel << 13) | method;
    for (unsigned index = 0; index < count; index++) ring[fifo->put++] = data[index];
}

static void push(struct nv50_fifo *fifo, uint32_t method, unsigned count, const uint32_t *data) {
    push_on(fifo, 0, method, count, data);
}

// Queues the methods written since START as one indirect buffer segment.
static int submit(struct nv50_device *gpu, uint32_t start) {
    struct nv50_fifo *fifo = &gpu->fifo;
    uint64_t at = (uint64_t)fifo->ring + start * 4U;
    uint32_t bytes = (fifo->put - start) * 4U;
    volatile uint32_t *ib = fifo->ring_map + PUSH_BYTES / 4U;
    ib[fifo->ib_put * 2U] = (uint32_t)at;
    ib[fifo->ib_put * 2U + 1U] =
        (uint32_t)(at >> 32) | (bytes << 8);
    fifo->ib_put = (fifo->ib_put + 1U) % IB_ENTRIES;
    (void)ib[0];
    if (bar_flush(gpu) != 0) return -1;
    nv50_wr32(gpu, USERD(CHANNEL) + USER_IB_PUT, fifo->ib_put);
    return 0;
}

// A fence: PFIFO writes the next sequence number into the semaphore once
// everything queued before it has run.
uint32_t nv50_fifo_fence(struct nv50_device *gpu) {
    struct nv50_fifo *fifo = &gpu->fifo;
    if (fifo->put + 16U > PUSH_BYTES / 4U) fifo->put = 0;
    uint32_t start = fifo->put;
    uint32_t sequence = ++fifo->sequence;
    uint32_t handle = HANDLE_VM;
    uint32_t semaphore[4] = {0, fifo->sema, sequence, SEMAPHORE_RELEASE};
    push(fifo, SET_DMA_SEMAPHORE, 1, &handle);
    push(fifo, SUBC_SEMAPHORE_A, 4, semaphore);
    return submit(gpu, start) == 0 ? sequence : 0;
}

int nv50_fifo_wait(struct nv50_device *gpu, uint32_t sequence, uint64_t timeout_ns) {
    uint64_t deadline = time_uptime_ns() + timeout_ns;
    while ((int32_t)(gpu->fifo.sema_map[0] - sequence) < 0) {
        if (time_uptime_ns() > deadline) return -1;
        cpu_relax();
    }
    return 0;
}

// ---- the copy engine -----------------------------------------------------

#include "ce_gt215.fuc3.h"

#define CE_BASE        0x104000U
#define PMC_ENABLE_CE  0x00802000U // gt215_mc_reset's
#define COPY_SUBC      1U
#define CE_SET_OBJECT  0x0000U
#define CE_DMA_QUERY   0x0180U
#define CE_SRC_HIGH    0x030CU
#define CE_LAUNCH      0x0300U
#define CE_LAUNCH_COPY 0x00000110U // as nva3_bo_move_copy launches it
#define COPY_LINE      4096U
#define COPY_LINES_MAX 8191U

// The falcon that runs the copy engine, loaded as nvkm_falcon_init loads an
// engine whose code and data are built in.
static int copy_engine_load(const struct nv50_device *gpu) {
    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_CE, 0);
    nv50_mask(gpu, PMC_ENABLE, PMC_ENABLE_CE, PMC_ENABLE_CE);
    uint32_t caps = nv50_rd32(gpu, CE_BASE + 0x12CU);
    uint32_t version = caps & 0xFU, secret = (caps >> 4) & 3U;
    uint32_t limits = nv50_rd32(gpu, CE_BASE + 0x108U);
    uint32_t code_limit = (limits & 0x1FFU) << 8, data_limit = (limits & 0x3FE00U) >> 1;
    kprintf("NV50: copy: falcon version %u, secret %u, code %u of %u bytes, data %u of %u\n",
            version, secret, (unsigned)sizeof(gt215_ce_code), code_limit,
            (unsigned)sizeof(gt215_ce_data), data_limit);
    if (sizeof(gt215_ce_code) > code_limit || sizeof(gt215_ce_data) > data_limit || version < 3U)
        return -1;
    if (secret) {
        uint64_t deadline = time_uptime_ns() + 2000000000ULL;
        while (nv50_rd32(gpu, CE_BASE + 0x180U) & 0x80000000U)
            if (time_uptime_ns() > deadline) return -1;
        nv50_wr32(gpu, CE_BASE + 0x004U, 0x00000010U);
    }
    nv50_wr32(gpu, CE_BASE + 0x014U, 0xFFFFFFFFU); // its interrupts stay off
    nv50_wr32(gpu, CE_BASE + 0x180U, 0x01000000U);
    for (uint32_t word = 0; word < sizeof(gt215_ce_code) / 4U; word++) {
        if (!(word & 0x3FU)) nv50_wr32(gpu, CE_BASE + 0x188U, word >> 6);
        nv50_wr32(gpu, CE_BASE + 0x184U, gt215_ce_code[word]);
    }
    nv50_wr32(gpu, CE_BASE + 0x1C0U, 0x01000000U);
    uint32_t word = 0;
    for (; word < sizeof(gt215_ce_data) / 4U; word++)
        nv50_wr32(gpu, CE_BASE + 0x1C4U, gt215_ce_data[word]);
    for (; word < data_limit / 4U; word++) nv50_wr32(gpu, CE_BASE + 0x1C4U, 0);
    nv50_wr32(gpu, CE_BASE + 0x10CU, 0x00000001U); // block on FIFO
    nv50_wr32(gpu, CE_BASE + 0x104U, 0);           // entry
    nv50_wr32(gpu, CE_BASE + 0x100U, 0x00000002U); // go
    nv50_wr32(gpu, CE_BASE + 0x048U, 0x00000003U); // FIFO and channel switches
    return 0;
}

static void report_copy_engine(const struct nv50_device *gpu) {
    uint32_t status = nv50_rd32(gpu, CE_BASE + 0x040U);
    kprintf("NV50: copy engine: intr %x, dispatch %x mthd %x data %x, channel %x, ctrl %x\n",
            nv50_rd32(gpu, CE_BASE + 0x008U), status & 0xFFFFU, ((status >> 16) & 0x7FFU) << 2,
            nv50_rd32(gpu, CE_BASE + 0x044U), nv50_rd32(gpu, CE_BASE + 0x050U),
            nv50_rd32(gpu, CE_BASE + 0x100U));
}

// The copy object on its subchannel, with the whole space as its source,
// destination and query DMA, as nv50_bo_move_init sets them.
static void copy_bind(struct nv50_fifo *fifo) {
    uint32_t object = HANDLE_COPY;
    uint32_t dma[3] = {HANDLE_VM, HANDLE_VM, HANDLE_VM};
    push_on(fifo, COPY_SUBC, CE_SET_OBJECT, 1, &object);
    push_on(fifo, COPY_SUBC, CE_DMA_QUERY, 3, dma);
}

// BYTES (a multiple of COPY_LINE) from SRC to DST, as nva3_bo_move_copy does
// it: lines of a page, up to 8191 of them a launch.
static void copy_queue(struct nv50_fifo *fifo, uint64_t dst, uint64_t src, uint32_t bytes) {
    for (uint32_t lines = bytes / COPY_LINE; lines;) {
        uint32_t now = lines > COPY_LINES_MAX ? COPY_LINES_MAX : lines;
        uint32_t args[8] = {(uint32_t)(src >> 32), (uint32_t)src, (uint32_t)(dst >> 32),
                            (uint32_t)dst,         COPY_LINE,     COPY_LINE,
                            COPY_LINE,             now};
        uint32_t launch = CE_LAUNCH_COPY;
        push_on(fifo, COPY_SUBC, CE_SRC_HIGH, 8, args);
        push_on(fifo, COPY_SUBC, CE_LAUNCH, 1, &launch);
        src += (uint64_t)now * COPY_LINE;
        dst += (uint64_t)now * COPY_LINE;
        lines -= now;
    }
}

int nv50_copy(struct nv50_device *gpu, uint32_t dst, uint32_t src, uint32_t bytes) {
    struct nv50_fifo *fifo = &gpu->fifo;
    if (fifo->put + 64U > PUSH_BYTES / 4U) fifo->put = 0;
    uint32_t start = fifo->put;
    copy_queue(fifo, dst, src, bytes);
    return submit(gpu, start);
}

// LINES of LINE_BYTES from SRC to DST, each with a pitch of its own.
uint32_t nv50_copy_rect(struct nv50_device *gpu, uint64_t dst, uint32_t dst_pitch, uint64_t src,
                        uint32_t src_pitch, uint32_t line_bytes, uint32_t lines) {
    struct nv50_fifo *fifo = &gpu->fifo;
    if (!fifo->copy_ready || !lines || lines > COPY_LINES_MAX) return 0;
    if (fifo->put + 32U > PUSH_BYTES / 4U) fifo->put = 0;
    uint32_t start = fifo->put;
    uint32_t args[8] = {(uint32_t)(src >> 32), (uint32_t)src, (uint32_t)(dst >> 32),
                        (uint32_t)dst,         src_pitch,     dst_pitch,
                        line_bytes,            lines};
    uint32_t launch = CE_LAUNCH_COPY;
    push_on(fifo, COPY_SUBC, CE_SRC_HIGH, 8, args);
    push_on(fifo, COPY_SUBC, CE_LAUNCH, 1, &launch);
    if (submit(gpu, start) != 0) return 0;
    return nv50_fifo_fence(gpu);
}

int nv50_fifo_done(const struct nv50_device *gpu, uint32_t sequence) {
    return (int32_t)(gpu->fifo.sema_map[0] - sequence) >= 0;
}

int nv50_copy_start(struct nv50_device *gpu) {
    if (copy_engine_load(gpu) != 0) return -1;
    struct nv50_fifo *fifo = &gpu->fifo;
    uint32_t start = fifo->put;
    copy_bind(fifo);
    if (submit(gpu, start) != 0) return -1;
    fifo->copy_ready = 1;
    return 0;
}

static void tlb_flush(const struct nv50_device *gpu) {
    nv50_wr32(gpu, PMMU_FLUSH, (PMMU_ENGINE_CE << 16) | 1U);
    (void)nv50_wait_clear(gpu, PMMU_FLUSH, 1U);
}

// COUNT system pages at consecutive GPU addresses; 0 when there is no room.
uint64_t nv50_gart_map(struct nv50_device *gpu, const uint64_t *pages, uint32_t count) {
    struct nv50_fifo *fifo = &gpu->fifo;
    if (!fifo->gart_map || !count || count > GART_BYTES / 4096U) return 0;
    uint32_t at = nv50_vram_alloc(&fifo->gart, count * 4096U, 0x1000U);
    if (!at) return 0;
    for (uint32_t page = 0; page < count; page++)
        wr64(fifo->gart_map, (at / 4096U + page) * 8U, (pages[page] & ~0xFFFULL) | PTE_HOST | PTE_VALID);
    (void)fifo->gart_map[0];
    if (bar_flush(gpu) != 0) return 0;
    return GART_BASE + at;
}

// No copy may still read the pages: their PTEs go and the copy engine forgets them.
void nv50_gart_unmap(struct nv50_device *gpu, uint64_t address, uint32_t count) {
    struct nv50_fifo *fifo = &gpu->fifo;
    uint32_t at = (uint32_t)(address - GART_BASE);
    for (uint32_t page = 0; page < count; page++) wr64(fifo->gart_map, (at / 4096U + page) * 8U, 0);
    (void)fifo->gart_map[0];
    (void)bar_flush(gpu);
    tlb_flush(gpu);
    nv50_vram_free(&fifo->gart, at, count * 4096U);
}

#define COPY_TEST_BYTES 0x400000U

static unsigned copy_mismatches(volatile const uint32_t *src, volatile const uint32_t *dst,
                                uint32_t *first) {
    unsigned wrong = 0;
    for (uint32_t word = 0; word < COPY_TEST_BYTES / 4U; word += word < 1024U ? 1U : 61U) {
        if (dst[word] == src[word]) continue;
        if (!wrong) *first = word;
        wrong++;
    }
    return wrong;
}

static void copy_selftest(struct nv50_device *gpu) {
    if (!gpu->fifo.copy_ready) {
        kprintf("NV50: copy: the engine is not running\n");
        return;
    }
    uint32_t src = nv50_vram_alloc(&gpu->vram, COPY_TEST_BYTES, 0x10000U);
    uint32_t dst = nv50_vram_alloc(&gpu->vram, COPY_TEST_BYTES, 0x10000U);
    volatile uint32_t *from = src ? map_vram(gpu, src, COPY_TEST_BYTES) : NULL;
    volatile uint32_t *to = dst ? map_vram(gpu, dst, COPY_TEST_BYTES) : NULL;
    if (!from || !to) return;
    for (uint32_t word = 0; word < COPY_TEST_BYTES / 4U; word++) {
        from[word] = word * 0x9E3779B1U + 0x54554E49U;
        to[word] = 0;
    }
    (void)to[0];
    if (bar_flush(gpu) != 0) return;
    uint64_t started = time_uptime_ns();
    if (nv50_copy(gpu, dst, src, COPY_TEST_BYTES) != 0) return;
    uint32_t sequence = nv50_fifo_fence(gpu);
    int fenced = sequence && nv50_fifo_wait(gpu, sequence, 1000000000ULL) == 0;
    uint64_t took = time_uptime_ns() - started;
    uint32_t first = 0;
    unsigned wrong = copy_mismatches(from, to, &first);
    if (fenced && !wrong) {
        kprintf("NV50: copy: %u KiB copied by the gpu in %u us (%u MB/s), all checked words match\n",
                COPY_TEST_BYTES >> 10, (unsigned)(took / 1000ULL),
                took ? (unsigned)((uint64_t)COPY_TEST_BYTES * 1000ULL / took) : 0);
        return;
    }
    kprintf("NV50: copy: fence %s after %u us, %u checked words differ, the first at %u: %x for %x\n",
            fenced ? "signalled" : "missing", (unsigned)(took / 1000ULL), wrong, first,
            to[first], from[first]);
    uint64_t until = time_uptime_ns() + 100000000ULL;
    while (time_uptime_ns() < until) cpu_relax();
    wrong = copy_mismatches(from, to, &first);
    kprintf("NV50: copy: 100 ms later %u differ\n", wrong);
    report_copy_engine(gpu);
    report_fifo(gpu, "after the copy");
}

// The present path's copy: system pages, mapped into the GPU's space, into
// VRAM with a pitch of its own, checked word for word against the pages.
#define GART_TEST_PAGES 1024U

static void gart_selftest(struct nv50_device *gpu) {
    uint64_t *pages = kmalloc(GART_TEST_PAGES * sizeof(uint64_t));
    uint32_t dst = nv50_vram_alloc(&gpu->vram, GART_TEST_PAGES * 4096U, 0x10000U);
    volatile uint32_t *to = dst ? map_vram(gpu, dst, GART_TEST_PAGES * 4096U) : NULL;
    if (!pages || !to) return;
    uint32_t got = 0;
    for (; got < GART_TEST_PAGES; got++) {
        void *page = pmm_alloc_page();
        if (!page) break;
        pages[got] = (uint64_t)page;
        uint32_t *words = vmm_phys_to_virt(pages[got]);
        for (uint32_t word = 0; word < 1024U; word++) words[word] = (got << 10 | word) ^ 0xA5A5A5A5U;
    }
    if (got == GART_TEST_PAGES) {
        for (uint32_t word = 0; word < GART_TEST_PAGES * 1024U; word++) to[word] = 0;
        (void)to[0];
        uint64_t at = nv50_gart_map(gpu, pages, GART_TEST_PAGES);
        uint64_t started = time_uptime_ns();
        uint32_t sequence = at ? nv50_copy_rect(gpu, dst, 4096U, at, 4096U, 4096U, GART_TEST_PAGES) : 0;
        int fenced = sequence && nv50_fifo_wait(gpu, sequence, 1000000000ULL) == 0;
        uint64_t took = time_uptime_ns() - started;
        unsigned wrong = 0;
        uint32_t first = 0;
        for (uint32_t word = 0; word < GART_TEST_PAGES * 1024U; word += 7U) {
            uint32_t want = ((word >> 10) << 10 | (word & 1023U)) ^ 0xA5A5A5A5U;
            if (to[word] == want) continue;
            if (!wrong) first = word;
            wrong++;
        }
        kprintf("NV50: gart: %u KiB of system memory copied into vram in %u us (%u MB/s), fence %s, "
                "%u checked words differ%s\n",
                GART_TEST_PAGES * 4U, (unsigned)(took / 1000ULL),
                took ? (unsigned)((uint64_t)GART_TEST_PAGES * 4096ULL * 1000ULL / took) : 0,
                fenced ? "signalled" : "missing", wrong, wrong ? ", see below" : "");
        if (wrong) {
            kprintf("NV50: gart: word %u holds %x\n", first, to[first]);
            report_trap(gpu);
            report_copy_engine(gpu);
        }
        if (at) nv50_gart_unmap(gpu, at, GART_TEST_PAGES);
    }
    for (uint32_t page = 0; page < got; page++) pmm_free_page((void *)pages[page]);
    kfree(pages);
    nv50_vram_free(&gpu->vram, dst, GART_TEST_PAGES * 4096U);
}

// The channel brought up and fenced: the first commands the GPU runs.
void nv50_fifo_selftest(struct nv50_device *gpu) {
    kprintf("NV50: fifo: %u MiB of vram free for the driver\n",
            nv50_vram_free_bytes(&gpu->vram) >> 20);
    uint64_t started = time_uptime_ns();
    uint32_t first = nv50_fifo_fence(gpu);
    if (!first || nv50_fifo_wait(gpu, first, 200000000ULL) != 0) {
        kprintf("NV50: fifo: the first fence did not signal\n");
        report_fifo(gpu, "stuck");
        return;
    }
    uint64_t took = time_uptime_ns() - started;
    started = time_uptime_ns();
    unsigned fences = 0;
    for (; fences < 1000U; fences++) {
        uint32_t sequence = nv50_fifo_fence(gpu);
        if (!sequence || nv50_fifo_wait(gpu, sequence, 100000000ULL) != 0) break;
    }
    uint64_t total = time_uptime_ns() - started;
    kprintf("NV50: fifo: channel %u fenced in %u us, then %u fences at %u us each\n", CHANNEL,
            (unsigned)(took / 1000ULL), fences, fences ? (unsigned)(total / 1000ULL / fences) : 0);
    if (fences < 1000U) {
        report_fifo(gpu, "a fence did not signal");
        return;
    }
    copy_selftest(gpu);
    if (gpu->fifo.copy_ready) gart_selftest(gpu);
}

#endif
