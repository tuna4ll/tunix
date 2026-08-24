#include <stddef.h>
#include <stdint.h>
#include "../include/block.h"
#include "../include/kstring.h"
#include "../include/nvme.h"
#include "../include/pci.h"
#include "../include/vmm.h"

/*
 * NVMe, enough of it to be a disk.
 *
 * Two queues: the admin pair the controller starts with, and one I/O pair
 * created on top of it. Commands are issued one at a time and polled for, the
 * same choice as the AHCI driver and for the same reason.
 *
 * Two details are worth knowing before reading this, because both are quiet
 * when they are wrong:
 *
 *  - A completion is *not* announced by a value appearing. The controller
 *    flips a phase bit on every wrap of the queue, so "done" means the entry's
 *    phase differs from the last pass, and an implementation that waits for a
 *    non-zero word works exactly once and then hangs. The command id is checked
 *    too, so a stale entry cannot be mistaken for the answer.
 *
 *  - A namespace picks its own block size. A drive formatted with 4 KiB blocks
 *    reports a quarter as many of them as the block layer counts in 512-byte
 *    sectors, and a read of one 512-byte sector is a read of the 4 KiB block it
 *    sits inside. Everything below translates; everything above counts in 512.
 */

extern void kprintf(const char *fmt, ...);

#define NVME_CLASS 0x01U
#define NVME_SUBCLASS 0x08U

#define REG_CAP 0x00U
#define REG_CC 0x14U
#define REG_CSTS 0x1CU
#define REG_AQA 0x24U
#define REG_ASQ 0x28U
#define REG_ACQ 0x30U

#define CC_ENABLE 0x00000001U
#define CSTS_READY 0x00000001U
#define CSTS_FATAL 0x00000002U

#define ADMIN_DELETE_SQ 0x00U
#define ADMIN_CREATE_SQ 0x01U
#define ADMIN_CREATE_CQ 0x05U
#define ADMIN_IDENTIFY 0x06U

#define IO_FLUSH 0x00U
#define IO_WRITE 0x01U
#define IO_READ 0x02U

#define QUEUE_ENTRIES 32U
#define NVME_WAIT_SPINS 40000000U
/* One PRP list page addresses 512 pages; the cap here is what the driver is
   willing to stage, and 128 KiB matches the AHCI side. */
#define NVME_MAX_PAGES 32U

struct nvme_command {
    uint32_t dword0;
    uint32_t nsid;
    uint32_t reserved[2];
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t dword10;
    uint32_t dword11;
    uint32_t dword12;
    uint32_t dword13;
    uint32_t dword14;
    uint32_t dword15;
} __attribute__((packed));

struct nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;   /* bit 0 is the phase */
} __attribute__((packed));

struct nvme_queue {
    struct nvme_command *submission;
    struct nvme_completion *completion;
    uint64_t submission_physical;
    uint64_t completion_physical;
    uint32_t submission_tail;
    uint32_t completion_head;
    uint32_t phase;            /* the value that means "this entry is new" */
    uint64_t submission_doorbell;
    uint64_t completion_doorbell;
};

static uint64_t registers;        /* how the window is reached now */
static uint64_t registers_physical;
static uint32_t doorbell_stride;

/*
 * Four queue pages, a pointer-list page and a scratch page, static rather than
 * allocated: this driver is probed before the allocator exists. Each queue has
 * to start on a page boundary because the registers holding their addresses
 * reserve the low twelve bits.
 */
#define NVME_DMA_PAGES 6U
static uint8_t nvme_dma[NVME_DMA_PAGES][4096] __attribute__((aligned(4096)));

#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL

static uint64_t static_physical(const void *address) {
    return (uint64_t)(uintptr_t)address - KERNEL_IMAGE_BASE;
}
static struct nvme_queue admin_queue;
static struct nvme_queue io_queue;
static uint64_t prp_list_physical;
static uint64_t *prp_list;
static uint16_t next_command_id = 1;

static uint64_t namespace_blocks;
static uint32_t namespace_block_bytes;
static uint32_t sectors_per_block;


/*
 * The physical address of a buffer the block layer handed down.
 *
 * Before vmm_init there is no page table to walk and no direct map to subtract,
 * so the two cases the early boot path actually uses are answered by
 * arithmetic: a static inside the kernel image, and a raw physical address
 * reached through the identity map the loader left behind -- which is how the
 * initramfs is staged. Everything else is a heap or direct-map address and only
 * exists once the page tables do.
 */
static uint64_t buffer_physical(uint64_t address) {
    if (address >= KERNEL_IMAGE_BASE &&
        address - KERNEL_IMAGE_BASE < 0x40000000ULL) {
        return address - KERNEL_IMAGE_BASE;
    }
    if (address < 0x100000000ULL) return address;
    uint64_t cr3 = vmm_kernel_cr3();
    uint64_t physical = 0;
    if (!cr3 || vmm_translate(cr3, address, &physical, NULL) != 0) return 0;
    return physical;
}

static uint32_t read32(uint64_t address) { return *(volatile uint32_t *)address; }
static void write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

/* 64-bit registers are written as two halves: the specification allows it, and
   not every platform lets a single 64-bit store reach a device. */
static void write64(uint64_t address, uint64_t value) {
    write32(address, (uint32_t)value);
    write32(address + 4U, (uint32_t)(value >> 32));
}

static void pause_cpu(void) { __asm__ volatile("pause"); }

/* The offset of a doorbell inside the window, not its address. */
static uint64_t doorbell_of(uint32_t queue, int completion) {
    uint32_t index = queue * 2U + (completion ? 1U : 0U);
    return 0x1000U + (uint64_t)index * (4ULL << doorbell_stride);
}

static int allocate_queue(struct nvme_queue *queue, uint32_t id,
                          unsigned submission_page, unsigned completion_page) {
    memset(queue, 0, sizeof(*queue));
    queue->submission = (struct nvme_command *)nvme_dma[submission_page];
    queue->completion = (struct nvme_completion *)nvme_dma[completion_page];
    queue->submission_physical = static_physical(queue->submission);
    queue->completion_physical = static_physical(queue->completion);
    memset(queue->submission, 0, 4096);
    memset(queue->completion, 0, 4096);
    /* The queue starts empty, so the first entry the controller writes will
       carry phase 1. */
    queue->phase = 1;
    /* Offsets rather than addresses: the window moves once, in nvme_remap(),
       and a doorbell recorded as an absolute address would not move with it. */
    queue->submission_doorbell = doorbell_of(id, 0);
    queue->completion_doorbell = doorbell_of(id, 1);
    return 0;
}

/*
 * Submit one command and wait for its completion. Returns the status field
 * with the phase bit removed, so zero is success.
 */
static int submit(struct nvme_queue *queue, struct nvme_command *command) {
    uint16_t id = next_command_id++;
    if (!next_command_id) next_command_id = 1;
    command->dword0 = (command->dword0 & 0xFFFFU) | ((uint32_t)id << 16);

    queue->submission[queue->submission_tail] = *command;
    queue->submission_tail = (queue->submission_tail + 1U) % QUEUE_ENTRIES;
    write32(registers + queue->submission_doorbell, queue->submission_tail);

    for (uint32_t spin = 0; spin < NVME_WAIT_SPINS; spin++) {
        volatile struct nvme_completion *entry = &queue->completion[queue->completion_head];
        uint16_t status = entry->status;
        if ((status & 1U) == queue->phase && entry->command_id == id) {
            queue->completion_head = (queue->completion_head + 1U) % QUEUE_ENTRIES;
            if (!queue->completion_head) queue->phase ^= 1U;
            write32(registers + queue->completion_doorbell, queue->completion_head);
            return (int)(status >> 1);
        }
        pause_cpu();
    }
    return -1;
}

/*
 * Describe a kernel virtual buffer to the controller. PRP1 may start part-way
 * into a page; every entry after it must be page aligned, which the pages of a
 * virtual range naturally are. Two pages fit in PRP1/PRP2; more need a list.
 */
static int build_prp(struct nvme_command *command, const void *buffer, uint32_t bytes) {
    uint64_t address = (uint64_t)(uintptr_t)buffer;
    uint64_t physical = buffer_physical(address);
    if (!physical) return -1;
    command->prp1 = physical;
    command->prp2 = 0;

    uint32_t first = 4096U - (uint32_t)(address & 0xFFFULL);
    if (bytes <= first) return 0;

    uint64_t next = address + first;
    uint32_t remaining = bytes - first;
    uint32_t pages = (remaining + 4095U) / 4096U;
    if (pages > NVME_MAX_PAGES) return -1;

    if (pages == 1U) {
        physical = buffer_physical(next);
        if (!physical) return -1;
        command->prp2 = physical;
        return 0;
    }

    for (uint32_t index = 0; index < pages; index++) {
        physical = buffer_physical(next + (uint64_t)index * 4096ULL);
        if (!physical) return -1;
        prp_list[index] = physical;
    }
    command->prp2 = prp_list_physical;
    return 0;
}

/* --- the block layer's view ---------------------------------------------- */

static int transfer(uint64_t lba, uint32_t count, void *buffer, int write) {
    /* The namespace counts in its own block size; the caller counts in 512s. */
    if (count % sectors_per_block || lba % sectors_per_block) return -1;
    uint64_t block = lba / sectors_per_block;
    uint32_t blocks = count / sectors_per_block;

    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = write ? IO_WRITE : IO_READ;
    command.nsid = 1;
    if (build_prp(&command, buffer, count * BLOCK_SECTOR_SIZE) != 0) return -1;
    command.dword10 = (uint32_t)block;
    command.dword11 = (uint32_t)(block >> 32);
    command.dword12 = blocks - 1U;   /* zero based */
    return submit(&io_queue, &command) == 0 ? 0 : -1;
}

/*
 * A namespace formatted with blocks larger than a sector cannot read or write
 * part of one, so a request that does not line up is staged through a whole
 * block: read it, patch it, write it back.
 */
static int nvme_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    (void)context;
    uint8_t *out = (uint8_t *)destination;
    while (count) {
        uint64_t aligned = lba - (lba % sectors_per_block);
        uint32_t within = (uint32_t)(lba - aligned);
        uint32_t chunk = count;
        uint32_t limit = NVME_MAX_PAGES * 4096U / BLOCK_SECTOR_SIZE;
        if (chunk > limit) chunk = limit;

        if (!within && chunk % sectors_per_block == 0) {
            if (transfer(lba, chunk, out, 0) != 0) return -1;
        } else {
            /* The pointer-list page is idle for a single-block transfer. */
            uint8_t *staging = (uint8_t *)prp_list;
            if (namespace_block_bytes > 4096U) return -1;
            if (transfer(aligned, sectors_per_block, staging, 0) != 0) return -1;
            uint32_t available = sectors_per_block - within;
            if (chunk > available) chunk = available;
            memcpy(out, staging + (size_t)within * BLOCK_SECTOR_SIZE,
                   (size_t)chunk * BLOCK_SECTOR_SIZE);
        }
        out += (size_t)chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int nvme_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    (void)context;
    const uint8_t *in = (const uint8_t *)source;
    while (count) {
        uint64_t aligned = lba - (lba % sectors_per_block);
        uint32_t within = (uint32_t)(lba - aligned);
        uint32_t chunk = count;
        uint32_t limit = NVME_MAX_PAGES * 4096U / BLOCK_SECTOR_SIZE;
        if (chunk > limit) chunk = limit;

        if (!within && chunk % sectors_per_block == 0) {
            if (transfer(lba, chunk, (void *)(uintptr_t)in, 1) != 0) return -1;
        } else {
            uint8_t *staging = (uint8_t *)prp_list;
            if (namespace_block_bytes > 4096U) return -1;
            if (transfer(aligned, sectors_per_block, staging, 0) != 0) return -1;
            uint32_t available = sectors_per_block - within;
            if (chunk > available) chunk = available;
            memcpy(staging + (size_t)within * BLOCK_SECTOR_SIZE, in,
                   (size_t)chunk * BLOCK_SECTOR_SIZE);
            if (transfer(aligned, sectors_per_block, staging, 1) != 0) return -1;
        }
        in += (size_t)chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int nvme_flush(void *context) {
    (void)context;
    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = IO_FLUSH;
    command.nsid = 1;
    return submit(&io_queue, &command) == 0 ? 0 : -1;
}

/* --- bring-up ------------------------------------------------------------ */

static int wait_ready(int wanted) {
    for (uint32_t spin = 0; spin < NVME_WAIT_SPINS; spin++) {
        uint32_t status = read32(registers + REG_CSTS);
        if (status & CSTS_FATAL) return -1;
        if (!!(status & CSTS_READY) == !!wanted) return 0;
        pause_cpu();
    }
    return -1;
}

static int identify_namespace(void) {
    uint8_t *data = nvme_dma[5];
    uint64_t page = static_physical(data);
    memset(data, 0, 4096);

    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_IDENTIFY;
    command.nsid = 1;
    command.prp1 = page;
    command.dword10 = 0;             /* CNS 0: this namespace */
    if (submit(&admin_queue, &command) != 0) return -1;

    uint64_t size = 0;
    memcpy(&size, data, sizeof(size));
    uint8_t formatted = data[26] & 0x0FU;      /* FLBAS: which format is in use */
    uint32_t format = 0;
    memcpy(&format, data + 128 + (size_t)formatted * 4U, sizeof(format));
    uint8_t shift = (uint8_t)((format >> 16) & 0xFFU);   /* LBADS */

    if (shift < 9U || shift > 12U || !size) return -1;
    namespace_blocks = size;
    namespace_block_bytes = 1U << shift;
    sectors_per_block = namespace_block_bytes / BLOCK_SECTOR_SIZE;
    return 0;
}

static int create_io_queues(void) {
    struct nvme_command command;

    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_CREATE_CQ;
    command.prp1 = io_queue.completion_physical;
    command.dword10 = ((QUEUE_ENTRIES - 1U) << 16) | 1U;   /* size, queue id 1 */
    command.dword11 = 1U;                                   /* physically contiguous */
    if (submit(&admin_queue, &command) != 0) return -1;

    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_CREATE_SQ;
    command.prp1 = io_queue.submission_physical;
    command.dword10 = ((QUEUE_ENTRIES - 1U) << 16) | 1U;
    command.dword11 = (1U << 16) | 1U;   /* completion queue 1, contiguous */
    return submit(&admin_queue, &command) == 0 ? 0 : -1;
}

void nvme_init(void) {
    struct pci_device pci;
    if (pci_find_class(NVME_CLASS, NVME_SUBCLASS, &pci) != 0) return;

    /* BAR0 is a 64-bit memory BAR, so the upper half lives in BAR1. */
    uint64_t base = ((uint64_t)pci.bar[0] & ~0xFULL);
    if ((pci.bar[0] & 0x6U) == 0x4U) base |= (uint64_t)pci.bar[1] << 32;
    if (!base) return;

    pci_enable_bus_mastering(&pci);
    /* Reachable where PCI says it is: the loader's identity map is still in
       place this early. nvme_remap() moves it once ours replaces it. */
    registers_physical = base;
    registers = base;

    uint32_t capability_high = read32(registers + REG_CAP + 4U);
    doorbell_stride = capability_high & 0x0FU;

    write32(registers + REG_CC, read32(registers + REG_CC) & ~CC_ENABLE);
    if (wait_ready(0) != 0) return;

    if (allocate_queue(&admin_queue, 0, 0, 1) != 0) return;
    if (allocate_queue(&io_queue, 1, 2, 3) != 0) return;

    prp_list = (uint64_t *)nvme_dma[4];
    prp_list_physical = static_physical(prp_list);
    memset(prp_list, 0, 4096);

    write32(registers + REG_AQA, ((QUEUE_ENTRIES - 1U) << 16) | (QUEUE_ENTRIES - 1U));
    write64(registers + REG_ASQ, admin_queue.submission_physical);
    write64(registers + REG_ACQ, admin_queue.completion_physical);

    /* 4 KiB pages, NVM command set, 64-byte submission and 16-byte completion
       entries -- the sizes are given as their base-2 logarithms. */
    uint32_t configuration = CC_ENABLE | (6U << 16) | (4U << 20);
    write32(registers + REG_CC, configuration);
    if (wait_ready(1) != 0) {
        kprintf("NVME: controller did not become ready\n");
        return;
    }

    if (create_io_queues() != 0) {
        kprintf("NVME: I/O queue creation failed\n");
        return;
    }
    if (identify_namespace() != 0) {
        kprintf("NVME: no usable namespace\n");
        return;
    }

    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.name[0] = 'n'; device.name[1] = 'v'; device.name[2] = 'm';
    device.name[3] = 'e'; device.name[4] = '0';
    device.sectors = namespace_blocks * sectors_per_block;
    device.read = nvme_read;
    device.write = nvme_write;
    device.flush = nvme_flush;
    device.context = NULL;
    block_register(&device);
    if (namespace_block_bytes != BLOCK_SECTOR_SIZE)
        kprintf("NVME: %u byte blocks, staged through as 512 byte sectors\n",
                (unsigned)namespace_block_bytes);
}

void nvme_remap(void) {
    if (!registers_physical || !namespace_blocks) return;
    uint64_t mapped = vmm_map_device(registers_physical, 0x2000U);
    if (!mapped) {
        kprintf("NVME: register window unavailable, namespace lost\n");
        return;
    }
    /* Doorbells are held as offsets, so nothing else has to move. */
    registers = mapped;
}
