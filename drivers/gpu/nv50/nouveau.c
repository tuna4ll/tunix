#include <stddef.h>
#include <stdint.h>

#include <tunix/cpu.h>
#include <tunix/drm.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/pmm.h>
#include <tunix/time.h>
#include <tunix/usercopy.h>
#include <tunix/vmm.h>

#include "priv.h"
#include <tunix/kprintf.h>

// nouveau's DRM interface, as Mesa's nouveau winsys (src/gallium/winsys/
// nouveau/drm/nouveau.c) drives it on Tesla: the ABI16 ioctls, the part of
// NVIF it uses, and GEM. Every client has a GPU address space of its own,
// holding only its buffers, and channels on it with their own PGRAPH and
// copy engine contexts. The structures below are nouveau_drm.h's and
// nvif's (MIT), field for field.

#define NOUVEAU_GETPARAM          0x00
#define NOUVEAU_CHANNEL_ALLOC     0x02
#define NOUVEAU_CHANNEL_FREE      0x03
#define NOUVEAU_GROBJ_ALLOC       0x04
#define NOUVEAU_NOTIFIEROBJ_ALLOC 0x05
#define NOUVEAU_GPUOBJ_FREE       0x06
#define NOUVEAU_NVIF              0x07
#define NOUVEAU_GEM_NEW           0x40
#define NOUVEAU_GEM_PUSHBUF       0x41
#define NOUVEAU_GEM_CPU_PREP      0x42
#define NOUVEAU_GEM_CPU_FINI      0x43
#define NOUVEAU_GEM_INFO          0x44

#define GETPARAM_PCI_VENDOR    3
#define GETPARAM_PCI_DEVICE    4
#define GETPARAM_BUS_TYPE      5
#define GETPARAM_FB_SIZE       8
#define GETPARAM_AGP_SIZE      9
#define GETPARAM_CHIPSET_ID    11
#define GETPARAM_VM_VRAM_BASE  12
#define GETPARAM_GRAPH_UNITS   13
#define GETPARAM_PTIMER_TIME   14
#define GETPARAM_HAS_BO_USAGE  15
#define GETPARAM_HAS_PAGEFLIP  16
#define GETPARAM_VRAM_BAR_SIZE 18
#define GETPARAM_VRAM_USED     19

#define DOMAIN_VRAM 0x2U
#define DOMAIN_GART 0x4U
#define TILE_NONCONTIG 0x00000008U
#define CPU_PREP_NOWAIT 0x1U
#define RELOC_LOW  0x1U
#define RELOC_HIGH 0x2U
#define RELOC_OR   0x4U
#define PUSH_NO_PREFETCH (1U << 23)
#define MAX_BUFFERS 1024U
#define MAX_RELOCS  1024U
#define MAX_PUSH    512U

struct getparam {
    uint64_t param;
    uint64_t value;
};

struct channel_alloc {
    uint32_t fb_ctxdma_handle;
    uint32_t tt_ctxdma_handle;
    int32_t channel;
    uint32_t pushbuf_domains;
    uint32_t notifier_handle;
    struct {
        uint32_t handle;
        uint32_t grclass;
    } subchan[8];
    uint32_t nr_subchan;
};

struct notifier_alloc {
    uint32_t channel;
    uint32_t handle;
    uint32_t size;
    uint32_t offset;
};

struct gpuobj_free {
    int32_t channel;
    uint32_t handle;
};

struct gem_info {
    uint32_t handle;
    uint32_t domain;
    uint64_t size;
    uint64_t offset;
    uint64_t map_handle;
    uint32_t tile_mode;
    uint32_t tile_flags;
};

struct gem_new {
    struct gem_info info;
    uint32_t channel_hint;
    uint32_t align;
};

struct pushbuf_bo {
    uint64_t user_priv;
    uint32_t handle;
    uint32_t read_domains;
    uint32_t write_domains;
    uint32_t valid_domains;
    struct {
        uint32_t valid;
        uint32_t domain;
        uint64_t offset;
    } presumed;
};

struct pushbuf_reloc {
    uint32_t reloc_bo_index;
    uint32_t reloc_bo_offset;
    uint32_t bo_index;
    uint32_t flags;
    uint32_t data;
    uint32_t vor;
    uint32_t tor;
};

struct pushbuf_push {
    uint32_t bo_index;
    uint32_t pad;
    uint64_t offset;
    uint64_t length;
};

struct pushbuf {
    uint32_t channel;
    uint32_t nr_buffers;
    uint64_t buffers;
    uint32_t nr_relocs;
    uint32_t nr_push;
    uint64_t relocs;
    uint64_t push;
    uint32_t suffix0;
    uint32_t suffix1;
    uint64_t vram_available;
    uint64_t gart_available;
};

struct cpu_prep {
    uint32_t handle;
    uint32_t flags;
};

_Static_assert(sizeof(struct channel_alloc) == 88, "drm_nouveau_channel_alloc");
_Static_assert(sizeof(struct gem_info) == 40, "drm_nouveau_gem_info");
_Static_assert(sizeof(struct gem_new) == 48, "drm_nouveau_gem_new");
_Static_assert(sizeof(struct pushbuf_bo) == 40, "drm_nouveau_gem_pushbuf_bo");
_Static_assert(sizeof(struct pushbuf_reloc) == 28, "drm_nouveau_gem_pushbuf_reloc");
_Static_assert(sizeof(struct pushbuf_push) == 24, "drm_nouveau_gem_pushbuf_push");
_Static_assert(sizeof(struct pushbuf) == 64, "drm_nouveau_gem_pushbuf");

// NVIF, as nvif/ioctl.h and nvif/cl0080.h have it.
#define NVIF_SCLASS 0x01
#define NVIF_NEW    0x02
#define NVIF_DEL    0x03
#define NVIF_MTHD   0x04
#define NVIF_ROUTE_HIDDEN 0xFF
#define NV_DEVICE 0x0080
#define NV_DEVICE_INFO 0x00
#define NV_DEVICE_TIME 0x01

struct nvif_ioctl {
    uint8_t version;
    uint8_t type;
    uint8_t pad02[4];
    uint8_t owner;
    uint8_t route;
    uint64_t token;
    uint64_t object;
};

struct nvif_new {
    uint8_t version;
    uint8_t pad01[6];
    uint8_t route;
    uint64_t token;
    uint64_t object;
    uint32_t handle;
    int32_t oclass;
};

struct nvif_sclass {
    uint8_t version;
    uint8_t count;
    uint8_t pad02[6];
};

struct nvif_sclass_oclass {
    int32_t oclass;
    int16_t minver;
    int16_t maxver;
};

struct nvif_mthd {
    uint8_t version;
    uint8_t method;
    uint8_t pad02[6];
};

struct nv_device_info {
    uint8_t version;
    uint8_t platform;
    uint16_t chipset;
    uint8_t revision;
    uint8_t family;
    uint8_t pad06[2];
    uint64_t ram_size;
    uint64_t ram_user;
    char chip[16];
    char name[64];
};

_Static_assert(sizeof(struct nvif_ioctl) == 24, "nvif_ioctl_v0");
_Static_assert(sizeof(struct nvif_new) == 32, "nvif_ioctl_new_v0");
_Static_assert(sizeof(struct nv_device_info) == 104, "nv_device_info_v0");

// ---- the driver's state ----------------------------------------------------

// A client's address space: 4 KiB pages from 512 MiB up to 3.5 GiB, a 1 MiB
// table for each 512 MiB it uses.
#define VA_BASE   0x20000000U
#define VA_END    0xE0000000U
#define VA_PDES   7U
#define PT_BYTES  0x100000U
#define PDE_4K    0x00000003U
#define PTE_VALID 0x1U
#define PTE_RO    0x8U
#define PTE_HOST  0x20U

// shortcut: BAR1 is taken to be 256 MiB, as on the GT 335M; read its size
// from PCI when boards with less turn up.
#define BAR1_BYTES 0x10000000U

#define CLIENTS     16U
#define CHANNELS    32U
#define FIRST_CHID  2U // 1 is the kernel's, 0 unusable
#define PUSH_BYTES  0x1000U
#define IB_BYTES    0x2000U
#define IB_ENTRIES  (IB_BYTES / 8U)
#define MAPS        4U

struct client {
    struct file *file;
    uint32_t pt[VA_PDES];
    volatile uint32_t *pt_map[VA_PDES];
    struct nv50_vram space;
};

struct channel {
    struct client *client;
    uint32_t id;
    uint32_t inst;
    volatile uint32_t *inst_map;
    uint32_t ring; // the kernel's methods, then the indirect buffer
    volatile uint32_t *ring_map;
    uint64_t ring_va;
    uint32_t fence; // its semaphore, and after it the notifiers
    volatile uint32_t *fence_map;
    uint64_t fence_va;
    uint32_t notifier_used;
    uint32_t put;
    uint32_t ib_put;
    uint32_t sequence;
    uint32_t gr_context;
    uint32_t next_object;
};

struct bo {
    uint32_t handle;
    int vram;
    uint32_t vram_addr;
    uint64_t size;
    uint32_t kind;
    uint32_t align;
    uint32_t tile_mode;
    uint32_t tile_flags;
    const uint64_t *pages;
    // The last submission that used it.
    struct channel *busy_channel;
    uint32_t busy_sequence;
    struct {
        struct client *client;
        uint64_t va;
    } maps[MAPS];
};

static struct nv50_device *gpu;
static struct client clients[CLIENTS];
static struct channel channels[CHANNELS];
static struct bo **bos;
static uint32_t bo_capacity;

// BAR1, mapped once write-combined: all the CPU writes of this file go
// through it, and are fenced before the GPU is told of them.
static volatile uint8_t *bar1;

static volatile uint32_t *map(uint32_t at, uint32_t bytes) {
    if (!bar1 || !at || (uint64_t)at + bytes > BAR1_BYTES) return NULL;
    return (volatile uint32_t *)(bar1 + at);
}

static int flush(void) {
#if defined(__x86_64__)
    __asm__ volatile("sfence" ::: "memory");
#else
    __sync_synchronize();
#endif
    return nv50_bar_flush(gpu);
}

static void wr64(volatile uint32_t *where, uint32_t offset, uint64_t value) {
    where[offset / 4U] = (uint32_t)value;
    where[offset / 4U + 1U] = (uint32_t)(value >> 32);
}

static struct client *client_of(struct file *file, int create) {
    struct client *free_slot = NULL;
    for (unsigned index = 0; index < CLIENTS; index++) {
        if (clients[index].file == file) return &clients[index];
        if (!clients[index].file && !free_slot) free_slot = &clients[index];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    if (nv50_vram_init(&free_slot->space, VA_BASE, VA_END) != 0) return NULL;
    free_slot->file = file;
    return free_slot;
}

static struct channel *channel_of(struct client *client, uint32_t id) {
    if (!client || id < FIRST_CHID || id >= FIRST_CHID + CHANNELS) return NULL;
    struct channel *channel = &channels[id - FIRST_CHID];
    return channel->client == client ? channel : NULL;
}

// ---- address spaces --------------------------------------------------------

static int fence_done(const struct channel *channel, uint32_t sequence) {
    return !channel || !channel->client || (int32_t)(channel->fence_map[0] - sequence) >= 0;
}

static int write_pdes(struct client *client, unsigned pde) {
    for (unsigned index = 0; index < CHANNELS; index++)
        if (channels[index].client == client)
            wr64(channels[index].inst_map, NV50_INST_PD + (1U + pde) * 8U,
                 (uint64_t)client->pt[pde] | PDE_4K);
    return 0;
}

// Maps BYTES at VA to VRAM at VRAM (with KIND) or to system PAGES.
static int map_range(struct client *client, uint64_t va, uint64_t bytes, int vram,
                     uint32_t vram_addr, uint32_t kind, const uint64_t *pages, int read_only) {
    for (uint64_t done = 0; done < bytes; done += 4096U) {
        uint64_t at = va + done;
        unsigned pde = (unsigned)(at / 0x20000000ULL) - 1U;
        if (pde >= VA_PDES) return -EINVAL;
        if (!client->pt[pde]) {
            client->pt[pde] = nv50_vram_alloc(&gpu->vram, PT_BYTES, 0x1000U);
            client->pt_map[pde] = client->pt[pde] ? map(client->pt[pde], PT_BYTES) : NULL;
            if (!client->pt_map[pde]) return -ENOMEM;
            for (uint32_t word = 0; word < PT_BYTES / 4U; word++) client->pt_map[pde][word] = 0;
            write_pdes(client, pde);
        }
        uint64_t pte = vram ? ((uint64_t)(vram_addr + done) | ((uint64_t)kind << 40))
                            : ((pages[done / 4096U] & ~0xFFFULL) | PTE_HOST);
        pte |= PTE_VALID | (read_only ? PTE_RO : 0);
        wr64(client->pt_map[pde], (uint32_t)((at % 0x20000000ULL) / 4096ULL) * 8U, pte);
    }
    return flush();
}

static void unmap_range(struct client *client, uint64_t va, uint64_t bytes) {
    for (uint64_t done = 0; done < bytes; done += 4096U) {
        uint64_t at = va + done;
        unsigned pde = (unsigned)(at / 0x20000000ULL) - 1U;
        if (pde < VA_PDES && client->pt_map[pde])
            wr64(client->pt_map[pde], (uint32_t)((at % 0x20000000ULL) / 4096ULL) * 8U, 0);
    }
    (void)flush();
    nv50_mmu_flush(gpu);
    nv50_vram_free(&client->space, (uint32_t)va, (uint32_t)bytes);
}

static uint64_t va_alloc(struct client *client, uint64_t bytes, uint32_t align) {
    if (align < 4096U) align = 4096U;
    if (bytes > VA_END - VA_BASE) return 0;
    return nv50_vram_alloc(&client->space, (uint32_t)bytes, align);
}

// ---- buffers --------------------------------------------------------------

static struct bo *bo_find(uint32_t handle) {
    return handle < bo_capacity ? bos[handle] : NULL;
}

static int bo_track(struct bo *bo) {
    if (bo->handle >= bo_capacity) {
        uint32_t capacity = bo_capacity ? bo_capacity : 64U;
        while (capacity <= bo->handle) capacity *= 2U;
        struct bo **grown = kmalloc(capacity * sizeof(*grown));
        if (!grown) return -ENOMEM;
        memset(grown, 0, capacity * sizeof(*grown));
        if (bos) {
            memcpy(grown, bos, bo_capacity * sizeof(*grown));
            kfree(bos);
        }
        bos = grown;
        bo_capacity = capacity;
    }
    bos[bo->handle] = bo;
    return 0;
}

static uint64_t bo_va(struct bo *bo, struct client *client) {
    for (unsigned index = 0; index < MAPS; index++)
        if (bo->maps[index].client == client) return bo->maps[index].va;
    for (unsigned index = 0; index < MAPS; index++) {
        if (bo->maps[index].client) continue;
        uint32_t align = bo->align > (bo->vram ? 0x10000U : 0x1000U) ? bo->align
                                                                       : (bo->vram ? 0x10000U : 0x1000U);
        uint64_t va = va_alloc(client, bo->size, align);
        if (!va) return 0;
        if (map_range(client, va, bo->size, bo->vram, bo->vram_addr, bo->kind, bo->pages, 0) != 0) {
            nv50_vram_free(&client->space, (uint32_t)va, (uint32_t)bo->size);
            return 0;
        }
        bo->maps[index].client = client;
        bo->maps[index].va = va;
        return va;
    }
    return 0;
}

// A buffer DRM has, made one of ours: a dumb buffer or one shared in, which
// lives in system pages.
static struct bo *bo_adopt(struct file *file, uint32_t handle) {
    struct bo *bo = bo_find(handle);
    if (bo) return bo;
    struct drm_buffer_view view;
    if (drm_buffer_lookup(file, handle, &view) != 0 || !view.pages) return NULL;
    bo = kmalloc(sizeof(*bo));
    if (!bo) return NULL;
    memset(bo, 0, sizeof(*bo));
    bo->handle = handle;
    bo->size = view.page_count * 4096ULL;
    bo->pages = view.pages;
    bo->tile_flags = TILE_NONCONTIG;
    if (bo_track(bo) != 0) {
        kfree(bo);
        return NULL;
    }
    return bo;
}

static void bo_wait(struct bo *bo) {
    uint64_t deadline = time_uptime_ns() + 2000000000ULL;
    while (!fence_done(bo->busy_channel, bo->busy_sequence) && time_uptime_ns() < deadline)
        cpu_relax();
}

// What KMS needs to show one of the clients' VRAM buffers: its layout.
int nv50_nouveau_layout(uint32_t vram, uint32_t *kind, uint32_t *tile_mode, uint32_t *bytes) {
    for (uint32_t handle = 0; handle < bo_capacity; handle++) {
        struct bo *bo = bos[handle];
        if (!bo || !bo->vram || bo->vram_addr != vram) continue;
        *kind = bo->kind;
        *tile_mode = bo->tile_mode;
        *bytes = (uint32_t)bo->size;
        return 0;
    }
    return -1;
}

static void buffer_gone(uint32_t handle) {
    struct bo *bo = bo_find(handle);
    if (!bo) return;
    bo_wait(bo);
    if (bo->vram) nv50_present_forget_vram(bo->vram_addr);
    for (unsigned index = 0; index < MAPS; index++)
        if (bo->maps[index].client) unmap_range(bo->maps[index].client, bo->maps[index].va, bo->size);
    if (bo->vram) nv50_vram_free(&gpu->vram, bo->vram_addr, (uint32_t)bo->size);
    bos[handle] = NULL;
    kfree(bo);
}

static void bo_report(struct bo *bo, struct client *client, struct gem_info *info) {
    info->handle = bo->handle;
    info->domain = bo->vram ? DOMAIN_VRAM : DOMAIN_GART;
    info->size = bo->size;
    info->offset = bo_va(bo, client);
    info->map_handle = drm_buffer_map_offset(bo->handle);
    info->tile_mode = bo->tile_mode;
    info->tile_flags = bo->tile_flags;
}

static int64_t gem_new(struct file *file, uint64_t argument) {
    struct gem_new request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    struct client *client = client_of(file, 1);
    if (!client) return -ENOMEM;
    uint64_t size = (request.info.size + 4095ULL) & ~4095ULL;
    if (!size || size > 0x40000000ULL) return -EINVAL;
    uint32_t align = request.align > 0x1000U ? request.align : 0x1000U;
    if (align & (align - 1U)) return -EINVAL;
    uint32_t vram_align = align < 0x10000U ? 0x10000U : align;
    // A tiled kind's VRAM keeps whole row blocks to itself, or the memory
    // controller's partition swizzle mixes it with its neighbours'.
    if ((request.info.tile_flags & 0x7F00U) &&
        ((request.info.domain & DOMAIN_VRAM) || !(request.info.domain & DOMAIN_GART))) {
        size = (size + gpu->rblock - 1U) / gpu->rblock * gpu->rblock;
        uint32_t a = vram_align, b = gpu->rblock;
        while (b) { uint32_t r = a % b; a = b; b = r; }
        vram_align = vram_align / a * gpu->rblock; // their least common multiple
    }
    struct bo *bo = kmalloc(sizeof(*bo));
    if (!bo) return -ENOMEM;
    memset(bo, 0, sizeof(*bo));
    bo->size = size;
    bo->align = align;
    // nv50's memtype: bits 8-14 its kind, 16-17 compression, which is not done here.
    bo->kind = (request.info.tile_flags >> 8) & 0x7FU;
    bo->tile_mode = request.info.tile_mode;
    bo->tile_flags = request.info.tile_flags & ~0x00030000U;
    int status = -ENOMEM;
    bo->vram = (request.info.domain & DOMAIN_VRAM) || !(request.info.domain & DOMAIN_GART);
    if (bo->vram) {
        bo->vram_addr = nv50_vram_alloc(&gpu->vram, (uint32_t)size, vram_align);
        // shortcut: clients' VRAM stays inside BAR1, which every buffer is
        // mapped through; buffers beyond it want a CPU map of their own.
        volatile uint32_t *words = bo->vram_addr ? map(bo->vram_addr, (uint32_t)size) : NULL;
        if (!words) {
            if (bo->vram_addr) nv50_vram_free(&gpu->vram, bo->vram_addr, (uint32_t)size);
            bo->vram_addr = 0;
            bo->vram = 0;
            if (!(request.info.domain & DOMAIN_GART)) {
                kfree(bo);
                return -ENOMEM;
            }
        } else {
            // Fresh VRAM holds what its last owner left in it.
            for (uint64_t word = 0; word < size / 4U; word++) words[word] = 0;
            status = drm_buffer_new_device(file, gpu->bar1 + bo->vram_addr, size, &bo->handle);
        }
    }
    if (!bo->vram) {
        // System pages are untiled: whatever kind was asked for is not theirs.
        bo->kind = 0;
        bo->tile_mode = 0;
        bo->tile_flags &= ~0x7F00U;
        status = drm_buffer_new_pages(file, size, &bo->handle);
        struct drm_buffer_view view;
        if (status == 0 && drm_buffer_lookup(file, bo->handle, &view) == 0) bo->pages = view.pages;
    }
    if (status == 0) status = bo_track(bo);
    if (status != 0) {
        if (bo->vram) nv50_vram_free(&gpu->vram, bo->vram_addr, (uint32_t)size);
        kfree(bo);
        return status;
    }
    bo_report(bo, client, &request.info);
    if (!request.info.offset) return -ENOMEM;
    return copy_to_user(argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t gem_info(struct file *file, uint64_t argument) {
    struct gem_info info;
    if (copy_from_user(&info, argument, sizeof(info)) != 0) return -EFAULT;
    struct client *client = client_of(file, 1);
    struct bo *bo = client ? bo_adopt(file, info.handle) : NULL;
    if (!bo) return -ENOENT;
    bo_report(bo, client, &info);
    return copy_to_user(argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

// What the engines say when a fence never comes: PGRAPH's interrupt, trap
// and status, PFIFO's interrupt and the MMU's last fault. PGRAPH's are
// cleared, as nv50_gr_intr acknowledges them, so later work can go on.
static void report_stall(const struct channel *channel) {
    uint32_t index = nv50_rd32(gpu, 0x100C90U), trap[6] = {0};
    if (index & 0x80000000U) {
        for (uint32_t word = 0; word < 6U; word++) {
            nv50_wr32(gpu, 0x100C90U, (index & 0x00FFFFFFU) | word << 24);
            trap[word] = nv50_rd32(gpu, 0x100C94U);
        }
        nv50_wr32(gpu, 0x100C90U, (index & 0x00FFFFFFU) | 0x80000000U);
    }
    uint32_t gr_intr = nv50_rd32(gpu, 0x400100U);
    kprintf("NV50: channel %u stalled at %u of %u: gr intr %x trap %x status %x, method %x data %x,"
            " fifo intr %x, mmu %x %x %x %x %x %x\n",
            channel->id, channel->fence_map[0], channel->sequence, gr_intr,
            nv50_rd32(gpu, 0x400108U), nv50_rd32(gpu, 0x400700U), nv50_rd32(gpu, 0x400704U),
            nv50_rd32(gpu, 0x400708U), nv50_rd32(gpu, 0x002100U), trap[0], trap[1], trap[2],
            trap[3], trap[4], trap[5]);
    if (gr_intr) {
        nv50_wr32(gpu, 0x400108U, nv50_rd32(gpu, 0x400108U));
        nv50_wr32(gpu, 0x400100U, gr_intr);
        nv50_wr32(gpu, 0x400500U, 0x00010001U);
    }
}

static int64_t gem_cpu_prep(struct file *file, uint64_t argument) {
    struct cpu_prep request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_buffer_view view;
    if (drm_buffer_lookup(file, request.handle, &view) != 0) return -ENOENT;
    struct bo *bo = bo_find(request.handle);
    if (!bo || fence_done(bo->busy_channel, bo->busy_sequence)) return 0;
    if (request.flags & CPU_PREP_NOWAIT) return -EBUSY;
    // The channel may go while DRM's lock is let go of; its semaphore's page
    // stays mapped, and a channel freed has signalled all it was given.
    struct channel *channel = bo->busy_channel;
    volatile uint32_t *semaphore = channel->fence_map;
    uint32_t sequence = bo->busy_sequence;
    drm_unlock_for_wait();
    uint64_t deadline = time_uptime_ns() + 15000000000ULL;
    int done;
    while (!(done = (int32_t)(semaphore[0] - sequence) >= 0) && time_uptime_ns() < deadline)
        cpu_relax();
    drm_relock_after_wait();
    // The buffer may have gone meanwhile; the channel, a static slot, has not.
    if (!done && channel->client) report_stall(channel);
    return done ? 0 : -EBUSY;
}

// ---- channels -------------------------------------------------------------

#define HANDLE_FENCE_DMA 0xBEEF0F00U

static void channel_free(struct channel *channel) {
    if (!channel->client) return;
    uint64_t deadline = time_uptime_ns() + 2000000000ULL;
    while (!fence_done(channel, channel->sequence) && time_uptime_ns() < deadline) cpu_relax();
    nv50_channel_off(gpu, channel->id, channel->inst);
    unmap_range(channel->client, channel->ring_va, PUSH_BYTES + IB_BYTES);
    unmap_range(channel->client, channel->fence_va, 0x1000U);
    nv50_gr_context_free(gpu, channel->gr_context);
    nv50_vram_free(&gpu->vram, channel->ring, PUSH_BYTES + IB_BYTES);
    nv50_vram_free(&gpu->vram, channel->fence, 0x1000U);
    nv50_vram_free(&gpu->vram, channel->inst, NV50_INST_BYTES);
    for (uint32_t handle = 0; handle < bo_capacity; handle++)
        if (bos[handle] && bos[handle]->busy_channel == channel) bos[handle]->busy_channel = NULL;
    memset(channel, 0, sizeof(*channel));
}

// A DMA object over [START, START + BYTES) of the channel's space, in the
// instance's object area.
static int dma_object(struct channel *channel, uint32_t handle, uint64_t start, uint64_t bytes) {
    if (channel->next_object + 0x20U > NV50_INST_RAMHT) return -ENOSPC;
    uint32_t at = channel->next_object;
    channel->next_object += 0x20U;
    uint64_t limit = start + bytes - 1U;
    volatile uint32_t *inst = channel->inst_map;
    inst[(at + 0x00U) / 4U] = 0x7FC0003DU; // as the channel's own VM DMA object
    inst[(at + 0x04U) / 4U] = (uint32_t)limit;
    inst[(at + 0x08U) / 4U] = (uint32_t)start;
    inst[(at + 0x0CU) / 4U] = ((uint32_t)(limit >> 32) << 24) | (uint32_t)(start >> 32);
    inst[(at + 0x10U) / 4U] = 0;
    inst[(at + 0x14U) / 4U] = 0;
    nv50_channel_ramht_add(inst, handle, at >> 4);
    return flush();
}

static int64_t channel_alloc(struct file *file, uint64_t argument) {
    struct channel_alloc request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    struct client *client = client_of(file, 1);
    if (!client) return -ENOMEM;
    struct channel *channel = NULL;
    for (unsigned index = 0; index < CHANNELS && !channel; index++)
        if (!channels[index].client) channel = &channels[index];
    if (!channel) return -ENOSPC;
    memset(channel, 0, sizeof(*channel));
    channel->id = FIRST_CHID + (uint32_t)(channel - channels);
    channel->inst = nv50_vram_alloc(&gpu->vram, NV50_INST_BYTES, 0x10000U);
    channel->ring = nv50_vram_alloc(&gpu->vram, PUSH_BYTES + IB_BYTES, 0x1000U);
    channel->fence = nv50_vram_alloc(&gpu->vram, 0x1000U, 0x1000U);
    channel->gr_context = nv50_gr_context_new(gpu);
    channel->inst_map = channel->inst ? map(channel->inst, NV50_INST_BYTES) : NULL;
    channel->ring_map = channel->ring ? map(channel->ring, PUSH_BYTES + IB_BYTES) : NULL;
    channel->fence_map = channel->fence ? map(channel->fence, 0x1000U) : NULL;
    channel->ring_va = va_alloc(client, PUSH_BYTES + IB_BYTES, 0x1000U);
    channel->fence_va = va_alloc(client, 0x1000U, 0x1000U);
    int status = -ENOMEM;
    if (channel->inst_map && channel->ring_map && channel->fence_map && channel->gr_context &&
        channel->ring_va && channel->fence_va) {
        channel->client = client;
        for (uint32_t word = 0; word < NV50_INST_BYTES / 4U; word++) channel->inst_map[word] = 0;
        for (uint32_t word = 0; word < (PUSH_BYTES + IB_BYTES) / 4U; word++)
            channel->ring_map[word] = 0;
        for (uint32_t word = 0; word < 1024U; word++) channel->fence_map[word] = 0;
        for (unsigned pde = 0; pde < VA_PDES; pde++)
            if (client->pt[pde]) write_pdes(client, pde);
        status = map_range(client, channel->ring_va, PUSH_BYTES + IB_BYTES, 1, channel->ring, 0,
                           NULL, 1);
        if (status == 0)
            status = map_range(client, channel->fence_va, 0x1000U, 1, channel->fence, 0, NULL, 0);
    }
    if (status == 0) {
        volatile uint32_t *inst = channel->inst_map;
        nv50_channel_layout(inst, channel->inst, channel->ring_va + PUSH_BYTES);
        nv50_gr_context_bind(inst, channel->gr_context);
        channel->next_object = NV50_INST_OBJECTS;
        // The VRAM and GART DMA objects Mesa names, both the whole space.
        nv50_channel_ramht_add(inst, request.fb_ctxdma_handle, NV50_INST_DMA_VM >> 4);
        if (request.tt_ctxdma_handle != request.fb_ctxdma_handle)
            nv50_channel_ramht_add(inst, request.tt_ctxdma_handle, NV50_INST_DMA_VM >> 4);
        nv50_channel_ramht_add(inst, HANDLE_FENCE_DMA, NV50_INST_DMA_VM >> 4);
        channel->notifier_used = 64U; // the fence's own semaphore first
        status = flush();
        if (status == 0) status = nv50_channel_on(gpu, channel->id, channel->inst);
    }
    if (status != 0) {
        if (channel->client) {
            channel_free(channel);
        } else {
            if (channel->ring_va) nv50_vram_free(&client->space, (uint32_t)channel->ring_va, PUSH_BYTES + IB_BYTES);
            if (channel->fence_va) nv50_vram_free(&client->space, (uint32_t)channel->fence_va, 0x1000U);
            nv50_gr_context_free(gpu, channel->gr_context);
            if (channel->ring) nv50_vram_free(&gpu->vram, channel->ring, PUSH_BYTES + IB_BYTES);
            if (channel->fence) nv50_vram_free(&gpu->vram, channel->fence, 0x1000U);
            if (channel->inst) nv50_vram_free(&gpu->vram, channel->inst, NV50_INST_BYTES);
            memset(channel, 0, sizeof(*channel));
        }
        return status;
    }
    request.channel = (int32_t)channel->id;
    kprintf("NV50: client channel %u, instance %x, %u KiB PGRAPH context\n", channel->id,
            channel->inst, nv50_gr_context_bytes() >> 10);
    request.pushbuf_domains = DOMAIN_VRAM | DOMAIN_GART;
    request.notifier_handle = 0;
    request.nr_subchan = 0;
    return copy_to_user(argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t channel_free_ioctl(struct file *file, uint64_t argument) {
    int32_t id;
    if (copy_from_user(&id, argument, sizeof(id)) != 0) return -EFAULT;
    struct channel *channel = channel_of(client_of(file, 0), (uint32_t)id);
    if (!channel) return -ENOENT;
    channel_free(channel);
    return 0;
}

// Notifiers: stretches of the page after the channel's semaphore, each with a
// DMA object of its own, as nouveau_abi16_ioctl_notifierobj_alloc makes them.
static int64_t notifier_alloc(struct file *file, uint64_t argument) {
    struct notifier_alloc request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    struct channel *channel = channel_of(client_of(file, 0), request.channel);
    if (!channel) return -ENOENT;
    uint32_t size = (request.size + 15U) & ~15U;
    if (!size || channel->notifier_used + size > 0x1000U) return -ENOSPC;
    uint32_t offset = channel->notifier_used;
    int status = dma_object(channel, request.handle, channel->fence_va + offset, size);
    if (status) return status;
    channel->notifier_used += size;
    request.offset = offset;
    return copy_to_user(argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

// ---- objects --------------------------------------------------------------

// What a Tesla GT215 channel offers, and the engine each runs on.
static const struct {
    int32_t oclass;
    uint32_t engine;
} classes[] = {
    {0x5039, NV50_ENGINE_GR}, // NV50_M2MF
    {0x502D, NV50_ENGINE_GR}, // NV50_2D
    {0x8597, NV50_ENGINE_GR}, // NVA3_3D (GT214_TESLA)
    {0x50C0, NV50_ENGINE_GR}, // NV50_COMPUTE
    {0x85C0, NV50_ENGINE_GR}, // NVA3_COMPUTE
    {0x85B5, NV50_ENGINE_CE}, // GT212_DMA
};

static int object_new(struct channel *channel, uint32_t handle, int32_t oclass) {
    unsigned index = 0;
    while (index < sizeof(classes) / sizeof(classes[0]) && classes[index].oclass != oclass) index++;
    if (index == sizeof(classes) / sizeof(classes[0])) return -ENODEV;
    if (classes[index].engine == NV50_ENGINE_CE) {
        // No instance of its own, as nvkm_ramht_update has it.
        nv50_channel_ramht_add(channel->inst_map, handle, (NV50_ENGINE_CE << 20) | (0x40U >> 4));
        return flush();
    }
    if (channel->next_object + 0x10U > NV50_INST_RAMHT) return -ENOSPC;
    uint32_t at = channel->next_object;
    channel->next_object += 0x10U;
    volatile uint32_t *inst = channel->inst_map;
    inst[(at + 0x00U) / 4U] = (uint32_t)oclass; // as nv50_gr_object_bind writes one
    inst[(at + 0x04U) / 4U] = 0;
    inst[(at + 0x08U) / 4U] = 0;
    inst[(at + 0x0CU) / 4U] = 0;
    nv50_channel_ramht_add(inst, handle, (NV50_ENGINE_GR << 20) | (at >> 4));
    return flush();
}

#define NVIF_MAX 512U

static int64_t nvif(struct file *file, unsigned size, uint64_t argument) {
    uint8_t buffer[NVIF_MAX];
    if (size < sizeof(struct nvif_ioctl) || size > NVIF_MAX) return -EINVAL;
    if (copy_from_user(buffer, argument, size) != 0) return -EFAULT;
    struct nvif_ioctl *header = (struct nvif_ioctl *)buffer;
    uint8_t *data = buffer + sizeof(*header);
    unsigned data_bytes = size - (unsigned)sizeof(*header);
    struct client *client = client_of(file, 1);
    if (!client) return -ENOMEM;

    if (header->type == NVIF_NEW) {
        if (data_bytes < sizeof(struct nvif_new)) return -EINVAL;
        struct nvif_new *new_object = (struct nvif_new *)data;
        if (header->route == NVIF_ROUTE_HIDDEN) {
            struct channel *channel = channel_of(client, (uint32_t)header->token);
            return channel ? object_new(channel, new_object->handle, new_object->oclass) : -ENOENT;
        }
        return new_object->oclass == NV_DEVICE ? 0 : -ENODEV;
    }
    if (header->type == NVIF_DEL) return 0;
    if (header->type == NVIF_SCLASS) {
        if (data_bytes < sizeof(struct nvif_sclass)) return -EINVAL;
        struct nvif_sclass *sclass = (struct nvif_sclass *)data;
        struct nvif_sclass_oclass *list = (struct nvif_sclass_oclass *)(sclass + 1);
        unsigned room = (data_bytes - (unsigned)sizeof(*sclass)) / sizeof(*list);
        unsigned count = sizeof(classes) / sizeof(classes[0]);
        if (header->route == NVIF_ROUTE_HIDDEN && !channel_of(client, (uint32_t)header->token))
            return -ENOENT;
        for (unsigned index = 0; index < count && index < room && index < sclass->count; index++) {
            list[index].oclass = classes[index].oclass;
            list[index].minver = 0;
            list[index].maxver = 0;
        }
        sclass->count = (uint8_t)count;
        return copy_to_user(argument, buffer, size) == 0 ? 0 : -EFAULT;
    }
    if (header->type == NVIF_MTHD) {
        if (data_bytes < sizeof(struct nvif_mthd)) return -EINVAL;
        struct nvif_mthd *method = (struct nvif_mthd *)data;
        uint8_t *arguments = data + sizeof(*method);
        unsigned argument_bytes = data_bytes - (unsigned)sizeof(*method);
        if (method->method == NV_DEVICE_INFO && argument_bytes >= sizeof(struct nv_device_info)) {
            struct nv_device_info info;
            memset(&info, 0, sizeof(info));
            uint32_t boot0 = nv50_rd32(gpu, 0);
            info.platform = 0x03; // PCIE
            info.chipset = (uint16_t)((boot0 >> 20) & 0x1FFU);
            info.revision = (uint8_t)boot0;
            info.family = 0x06; // TESLA
            info.ram_size = gpu->vram_bytes;
            info.ram_user = BAR1_BYTES;
            strncpy(info.chip, "NVA3", sizeof(info.chip) - 1);
            strncpy(info.name, "GT215", sizeof(info.name) - 1);
            memcpy(arguments, &info, sizeof(info));
            return copy_to_user(argument, buffer, size) == 0 ? 0 : -EFAULT;
        }
        if (method->method == NV_DEVICE_TIME && argument_bytes >= 16U) {
            uint64_t now = time_uptime_ns();
            memcpy(arguments + 8, &now, sizeof(now));
            return copy_to_user(argument, buffer, size) == 0 ? 0 : -EFAULT;
        }
        return -EINVAL;
    }
    return -EINVAL;
}

// ---- submission -----------------------------------------------------------

// Methods the kernel adds to a channel: its fence, a semaphore release.
static void kernel_push(struct channel *channel, const uint32_t *words, unsigned count) {
    for (unsigned index = 0; index < count; index++) channel->ring_map[channel->put++] = words[index];
}

static int ib_room(struct channel *channel) {
    uint64_t deadline = time_uptime_ns() + 2000000000ULL;
    for (;;) {
        uint32_t get = nv50_rd32(gpu, NV50_USERD(channel->id) + 0x88U);
        if (((channel->ib_put + 1U) % IB_ENTRIES) != get) return 0;
        if (time_uptime_ns() > deadline) return -EBUSY;
        cpu_relax();
    }
}

static int ib_queue(struct channel *channel, uint64_t va, uint32_t bytes, int no_prefetch) {
    if (ib_room(channel) != 0) return -EBUSY;
    volatile uint32_t *ib = channel->ring_map + PUSH_BYTES / 4U;
    ib[channel->ib_put * 2U] = (uint32_t)va;
    ib[channel->ib_put * 2U + 1U] =
        (uint32_t)(va >> 32) | (bytes << 8) | (no_prefetch ? 0x80000000U : 0);
    channel->ib_put = (channel->ib_put + 1U) % IB_ENTRIES;
    return 0;
}

static int fence_emit(struct channel *channel) {
    // The kernel's own part of the ring is reused once the GPU is past it.
    if (channel->put + 8U > PUSH_BYTES / 4U) {
        uint64_t deadline = time_uptime_ns() + 2000000000ULL;
        while (!fence_done(channel, channel->sequence) && time_uptime_ns() < deadline) cpu_relax();
        channel->put = 0;
    }
    uint32_t start = channel->put;
    uint32_t sequence = channel->sequence + 1U;
    uint32_t words[7] = {(1U << 18) | 0x0060U, HANDLE_FENCE_DMA, (4U << 18) | 0x0010U,
                         (uint32_t)(channel->fence_va >> 32), (uint32_t)channel->fence_va,
                         sequence, 2U}; // release
    kernel_push(channel, words, 7);
    int status = ib_queue(channel, channel->ring_va + start * 4U, 7U * 4U, 0);
    if (status == 0) channel->sequence = sequence;
    return status;
}

// The words of buffer BO at OFFSET, where the CPU reaches them.
static volatile uint32_t *bo_word(struct bo *bo, uint32_t offset) {
    if (offset + 4U > bo->size) return NULL;
    if (bo->vram) return map(bo->vram_addr + offset, 4U);
    return (volatile uint32_t *)((uint8_t *)vmm_phys_to_virt(bo->pages[offset / 4096U]) +
                                 (offset & 0xFFFU));
}

static int64_t gem_pushbuf(struct file *file, uint64_t argument) {
    struct pushbuf request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    struct client *client = client_of(file, 0);
    struct channel *channel = channel_of(client, request.channel);
    if (!channel) return -ENOENT;
    request.suffix0 = 0;
    request.suffix1 = 0;
    request.vram_available = nv50_vram_free_bytes(&gpu->vram);
    request.gart_available = 0x20000000ULL;
    if (request.nr_buffers > MAX_BUFFERS || request.nr_relocs > MAX_RELOCS ||
        request.nr_push > MAX_PUSH)
        return -EINVAL;
    if (!request.nr_push) return copy_to_user(argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;

    int64_t status = 0;
    struct pushbuf_bo *buffers = kmalloc((request.nr_buffers + 1U) * sizeof(*buffers));
    struct bo **list = kmalloc((request.nr_buffers + 1U) * sizeof(*list));
    uint64_t *vas = kmalloc((request.nr_buffers + 1U) * sizeof(*vas));
    if (!buffers || !list || !vas) {
        status = -ENOMEM;
        goto out;
    }
    if (copy_from_user(buffers, request.buffers, request.nr_buffers * sizeof(*buffers)) != 0) {
        status = -EFAULT;
        goto out;
    }
    int relocate = 0;
    for (uint32_t index = 0; index < request.nr_buffers; index++) {
        list[index] = bo_adopt(file, buffers[index].handle);
        vas[index] = list[index] ? bo_va(list[index], client) : 0;
        if (!vas[index]) {
            status = -ENOENT;
            goto out;
        }
        uint32_t domain = list[index]->vram ? DOMAIN_VRAM : DOMAIN_GART;
        if (!buffers[index].presumed.valid || buffers[index].presumed.offset != vas[index] ||
            buffers[index].presumed.domain != domain) {
            buffers[index].presumed.valid = 0;
            buffers[index].presumed.offset = vas[index];
            buffers[index].presumed.domain = domain;
            relocate = 1;
        }
    }
    // Relocations, as nouveau_gem_pushbuf_reloc_apply applies them, only
    // when some buffer is not where the client thought.
    for (uint32_t index = 0; relocate && index < request.nr_relocs; index++) {
        struct pushbuf_reloc reloc;
        if (copy_from_user(&reloc, request.relocs + index * sizeof(reloc), sizeof(reloc)) != 0) {
            status = -EFAULT;
            goto out;
        }
        if (reloc.bo_index >= request.nr_buffers || reloc.reloc_bo_index >= request.nr_buffers ||
            (reloc.reloc_bo_offset & 3U)) {
            status = -EINVAL;
            goto out;
        }
        if (buffers[reloc.bo_index].presumed.valid) continue;
        volatile uint32_t *word = bo_word(list[reloc.reloc_bo_index], reloc.reloc_bo_offset);
        if (!word) {
            status = -EINVAL;
            goto out;
        }
        uint64_t address = vas[reloc.bo_index] + reloc.data;
        uint32_t value = reloc.flags & RELOC_LOW ? (uint32_t)address
            : reloc.flags & RELOC_HIGH          ? (uint32_t)(address >> 32)
                                                : reloc.data;
        if (reloc.flags & RELOC_OR) value |= list[reloc.bo_index]->vram ? reloc.vor : reloc.tor;
        *word = value;
    }
    for (uint32_t index = 0; index < request.nr_push; index++) {
        struct pushbuf_push push;
        if (copy_from_user(&push, request.push + index * sizeof(push), sizeof(push)) != 0) {
            status = -EFAULT;
            goto out;
        }
        uint32_t bytes = (uint32_t)(push.length & 0x7FFFFFU);
        if (push.bo_index >= request.nr_buffers || (push.offset & 3U) || (bytes & 3U) ||
            push.offset + bytes > list[push.bo_index]->size) {
            status = -EINVAL;
            goto out;
        }
        status = ib_queue(channel, vas[push.bo_index] + push.offset, bytes,
                          (push.length & PUSH_NO_PREFETCH) != 0);
        if (status) goto out;
    }
    status = fence_emit(channel);
    if (status) goto out;
    for (uint32_t index = 0; index < request.nr_buffers; index++) {
        list[index]->busy_channel = channel;
        list[index]->busy_sequence = channel->sequence;
    }
    (void)channel->ring_map[0];
    if (flush() != 0) {
        status = -EIO;
        goto out;
    }
    nv50_wr32(gpu, NV50_USERD(channel->id) + 0x8CU, channel->ib_put);
    if (copy_to_user(request.buffers, buffers, request.nr_buffers * sizeof(*buffers)) != 0 ||
        copy_to_user(argument, &request, sizeof(request)) != 0)
        status = -EFAULT;
out:
    kfree(buffers);
    kfree(list);
    kfree(vas);
    return status;
}

// ---- the rest -------------------------------------------------------------

static int64_t getparam(uint64_t argument) {
    struct getparam request;
    if (copy_from_user(&request, argument, sizeof(request)) != 0) return -EFAULT;
    switch (request.param) {
    case GETPARAM_PCI_VENDOR:    request.value = 0x10DE; break;
    case GETPARAM_PCI_DEVICE:    request.value = gpu->pci_device_id; break;
    case GETPARAM_BUS_TYPE:      request.value = 2; break; // PCIe
    case GETPARAM_FB_SIZE:       request.value = BAR1_BYTES; break; // what clients can have
    case GETPARAM_AGP_SIZE:      request.value = 0x20000000ULL; break;
    case GETPARAM_CHIPSET_ID:    request.value = (nv50_rd32(gpu, 0) >> 20) & 0x1FFU; break;
    case GETPARAM_VM_VRAM_BASE:  request.value = 0; break;
    case GETPARAM_GRAPH_UNITS:   request.value = nv50_rd32(gpu, 0x001540U); break;
    case GETPARAM_PTIMER_TIME:   request.value = time_uptime_ns(); break;
    case GETPARAM_HAS_BO_USAGE:  request.value = 1; break;
    case GETPARAM_HAS_PAGEFLIP:  request.value = 1; break;
    case GETPARAM_VRAM_BAR_SIZE: request.value = BAR1_BYTES; break;
    case GETPARAM_VRAM_USED:
        request.value = gpu->vram_bytes - nv50_vram_free_bytes(&gpu->vram);
        break;
    default: return -EINVAL;
    }
    return copy_to_user(argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t nouveau_ioctl(struct file *file, unsigned nr, unsigned size, uint64_t argument) {
    switch (nr) {
    case NOUVEAU_GETPARAM:          return getparam(argument);
    case NOUVEAU_CHANNEL_ALLOC:     return channel_alloc(file, argument);
    case NOUVEAU_CHANNEL_FREE:      return channel_free_ioctl(file, argument);
    case NOUVEAU_NOTIFIEROBJ_ALLOC: return notifier_alloc(file, argument);
    case NOUVEAU_GPUOBJ_FREE:       return 0;
    case NOUVEAU_GROBJ_ALLOC:       return -EINVAL;
    case NOUVEAU_NVIF:              return nvif(file, size, argument);
    case NOUVEAU_GEM_NEW:           return gem_new(file, argument);
    case NOUVEAU_GEM_PUSHBUF:       return gem_pushbuf(file, argument);
    case NOUVEAU_GEM_CPU_PREP:      return gem_cpu_prep(file, argument);
    case NOUVEAU_GEM_CPU_FINI:      return 0;
    case NOUVEAU_GEM_INFO:          return gem_info(file, argument);
    default:                        return -EINVAL;
    }
}

static void file_close(struct file *file) {
    struct client *client = client_of(file, 0);
    if (!client) return;
    for (unsigned index = 0; index < CHANNELS; index++)
        if (channels[index].client == client) channel_free(&channels[index]);
    // Its buffers' mappings in this space go; the buffers go when DRM drops them.
    for (uint32_t handle = 0; handle < bo_capacity; handle++) {
        struct bo *bo = bos[handle];
        for (unsigned index = 0; bo && index < MAPS; index++) {
            if (bo->maps[index].client != client) continue;
            bo_wait(bo);
            unmap_range(client, bo->maps[index].va, bo->size);
            bo->maps[index].client = NULL;
        }
    }
    for (unsigned pde = 0; pde < VA_PDES; pde++)
        if (client->pt[pde]) nv50_vram_free(&gpu->vram, client->pt[pde], PT_BYTES);
    memset(client, 0, sizeof(*client));
}

static struct drm_render_driver nouveau_driver = {
    .name = "nouveau",
    .desc = "nVidia Riva/TNT/GeForce/Quadro/Tesla/Tegra K1+",
    .major = 1,
    .minor = 3,
    .patchlevel = 1,
    .ioctl = nouveau_ioctl,
    .file_close = file_close,
    .buffer_gone = buffer_gone,
};

void nv50_nouveau_register(struct nv50_device *device) {
    gpu = device;
    nouveau_driver.pci_bus = device->pci_bus;
    nouveau_driver.pci_slot = device->pci_slot;
    nouveau_driver.pci_function = device->pci_function;
    nouveau_driver.pci_vendor = 0x10DE;
    nouveau_driver.pci_device = device->pci_device_id;
    bar1 = (volatile uint8_t *)vmm_map_device_write_combining(gpu->bar1, BAR1_BYTES);
    if (!bar1) {
        kprintf("NV50: BAR1 could not be mapped; no nouveau interface\n");
        return;
    }
    drm_register_render_driver(&nouveau_driver);
    kprintf("NV50: DRM speaks nouveau 1.3.1 for Mesa\n");
}
