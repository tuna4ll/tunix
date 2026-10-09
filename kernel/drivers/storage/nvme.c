#include <stddef.h>
#include <stdint.h>
#include <tunix/block.h>
#include <tunix/cpu.h>
#include <tunix/dma.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/nvme.h>
#include <tunix/pci.h>
#include <tunix/vmm.h>
#include <tunix/iowait.h>
#include <tunix/lock.h>
#include <tunix/mutex.h>

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
    uint16_t status;
} __attribute__((packed));

struct nvme_queue {
    struct nvme_command *submission;
    struct nvme_completion *completion;
    uint64_t submission_physical;
    uint64_t completion_physical;
    uint32_t submission_tail;
    uint32_t completion_head;
    uint32_t phase;
    uint64_t submission_doorbell;
    uint64_t completion_doorbell;
};

struct nvme_controller {
    struct mutex lock;
    uint64_t registers;
    uint32_t doorbell_stride;
    uint8_t *pages;
    uint64_t pages_physical;
    struct nvme_queue admin_queue;
    struct nvme_queue io_queue;
    uint64_t *prp_list;
    uint64_t prp_list_physical;
    uint16_t next_command_id;
    unsigned index;
};

struct nvme_namespace {
    struct nvme_controller *controller;
    uint32_t nsid;
    uint64_t blocks;
    uint32_t block_bytes;
    uint32_t sectors_per_block;
};

#define NVME_DMA_PAGES 6U

static unsigned controller_count;

static uint64_t buffer_physical(uint64_t address) {
    uint64_t cr3 = vmm_kernel_cr3();
    uint64_t physical = 0;
    if (!cr3 || vmm_translate(cr3, address, &physical, NULL) != 0) return 0;
    return physical;
}

static uint32_t read32(uint64_t address) { return *(volatile uint32_t *)address; }
static void write32(uint64_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}

static void write64(uint64_t address, uint64_t value) {
    write32(address, (uint32_t)value);
    write32(address + 4U, (uint32_t)(value >> 32));
}

static uint8_t *page_of(struct nvme_controller *controller, unsigned page) {
    return controller->pages + (size_t)page * 4096U;
}

static uint64_t page_physical(struct nvme_controller *controller, unsigned page) {
    return controller->pages_physical + (uint64_t)page * 4096U;
}

static uint64_t doorbell_of(struct nvme_controller *controller, uint32_t queue, int completion) {
    uint32_t index = queue * 2U + (completion ? 1U : 0U);
    return 0x1000U + (uint64_t)index * (4ULL << controller->doorbell_stride);
}

static void allocate_queue(struct nvme_controller *controller, struct nvme_queue *queue,
                           uint32_t id, unsigned submission_page, unsigned completion_page) {
    memset(queue, 0, sizeof(*queue));
    queue->submission = (struct nvme_command *)page_of(controller, submission_page);
    queue->completion = (struct nvme_completion *)page_of(controller, completion_page);
    queue->submission_physical = page_physical(controller, submission_page);
    queue->completion_physical = page_physical(controller, completion_page);
    queue->phase = 1;
    queue->submission_doorbell = doorbell_of(controller, id, 0);
    queue->completion_doorbell = doorbell_of(controller, id, 1);
}

static int completion_posted(void *context) {
    struct nvme_queue *queue = (struct nvme_queue *)context;
    volatile struct nvme_completion *entry = &queue->completion[queue->completion_head];
    return (entry->status & 1U) == queue->phase;
}

static int submit(struct nvme_controller *controller, struct nvme_queue *queue,
                  struct nvme_command *command) {
    uint16_t id = controller->next_command_id++;
    if (!controller->next_command_id) controller->next_command_id = 1;
    command->dword0 = (command->dword0 & 0xFFFFU) | ((uint32_t)id << 16);

    queue->submission[queue->submission_tail] = *command;
    queue->submission_tail = (queue->submission_tail + 1U) % QUEUE_ENTRIES;
    write32(controller->registers + queue->submission_doorbell, queue->submission_tail);

    if (io_poll(completion_posted, queue, IO_TIMEOUT_NS) != 0) return -1;
    volatile struct nvme_completion *entry = &queue->completion[queue->completion_head];
    uint16_t status = entry->status;
    if (entry->command_id != id) return -1;
    queue->completion_head = (queue->completion_head + 1U) % QUEUE_ENTRIES;
    if (!queue->completion_head) queue->phase ^= 1U;
    write32(controller->registers + queue->completion_doorbell, queue->completion_head);
    return (int)(status >> 1);
}

static int build_prp(struct nvme_controller *controller, struct nvme_command *command,
                     const void *buffer, uint32_t bytes) {
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
        controller->prp_list[index] = physical;
    }
    command->prp2 = controller->prp_list_physical;
    return 0;
}

static int transfer(struct nvme_namespace *space, uint64_t lba, uint32_t count,
                    void *buffer, int write) {
    if (count % space->sectors_per_block || lba % space->sectors_per_block) return -1;
    uint64_t block = lba / space->sectors_per_block;
    uint32_t blocks = count / space->sectors_per_block;

    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = write ? IO_WRITE : IO_READ;
    command.nsid = space->nsid;
    if (build_prp(space->controller, &command, buffer, count * BLOCK_SECTOR_SIZE) != 0) return -1;
    command.dword10 = (uint32_t)block;
    command.dword11 = (uint32_t)(block >> 32);
    command.dword12 = blocks - 1U;
    return submit(space->controller, &space->controller->io_queue, &command) == 0 ? 0 : -1;
}

static int nvme_read_unlocked(void *context, uint64_t lba, uint32_t count, void *destination) {
    struct nvme_namespace *space = context;
    uint8_t *out = (uint8_t *)destination;
    while (count) {
        uint64_t aligned = lba - (lba % space->sectors_per_block);
        uint32_t within = (uint32_t)(lba - aligned);
        uint32_t chunk = count;
        uint32_t limit = NVME_MAX_PAGES * 4096U / BLOCK_SECTOR_SIZE;
        if (chunk > limit) chunk = limit;

        if (!within && chunk % space->sectors_per_block == 0) {
            if (transfer(space, lba, chunk, out, 0) != 0) return -1;
        } else {
            uint8_t *staging = page_of(space->controller, 5);
            if (transfer(space, aligned, space->sectors_per_block, staging, 0) != 0) return -1;
            uint32_t available = space->sectors_per_block - within;
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

static int nvme_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    struct nvme_namespace *space = context;
    mutex_lock(&space->controller->lock);
    int status = nvme_read_unlocked(context, lba, count, destination);
    mutex_unlock(&space->controller->lock);
    return status;
}

static int nvme_write_unlocked(void *context, uint64_t lba, uint32_t count, const void *source) {
    struct nvme_namespace *space = context;
    const uint8_t *in = (const uint8_t *)source;
    while (count) {
        uint64_t aligned = lba - (lba % space->sectors_per_block);
        uint32_t within = (uint32_t)(lba - aligned);
        uint32_t chunk = count;
        uint32_t limit = NVME_MAX_PAGES * 4096U / BLOCK_SECTOR_SIZE;
        if (chunk > limit) chunk = limit;

        if (!within && chunk % space->sectors_per_block == 0) {
            if (transfer(space, lba, chunk, (void *)(uintptr_t)in, 1) != 0) return -1;
        } else {
            uint8_t *staging = page_of(space->controller, 5);
            if (transfer(space, aligned, space->sectors_per_block, staging, 0) != 0) return -1;
            uint32_t available = space->sectors_per_block - within;
            if (chunk > available) chunk = available;
            memcpy(staging + (size_t)within * BLOCK_SECTOR_SIZE, in,
                   (size_t)chunk * BLOCK_SECTOR_SIZE);
            if (transfer(space, aligned, space->sectors_per_block, staging, 1) != 0) return -1;
        }
        in += (size_t)chunk * BLOCK_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int nvme_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    struct nvme_namespace *space = context;
    mutex_lock(&space->controller->lock);
    int status = nvme_write_unlocked(context, lba, count, source);
    mutex_unlock(&space->controller->lock);
    return status;
}

static int nvme_flush_unlocked(void *context) {
    struct nvme_namespace *space = context;
    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = IO_FLUSH;
    command.nsid = space->nsid;
    return submit(space->controller, &space->controller->io_queue, &command) == 0 ? 0 : -1;
}

static int nvme_flush(void *context) {
    struct nvme_namespace *space = context;
    mutex_lock(&space->controller->lock);
    int status = nvme_flush_unlocked(context);
    mutex_unlock(&space->controller->lock);
    return status;
}

static int wait_ready(struct nvme_controller *controller, int wanted) {
    for (uint32_t spin = 0; spin < NVME_WAIT_SPINS; spin++) {
        uint32_t status = read32(controller->registers + REG_CSTS);
        if (status & CSTS_FATAL) return -1;
        if (!!(status & CSTS_READY) == !!wanted) return 0;
        cpu_relax();
    }
    return -1;
}

static int identify(struct nvme_controller *controller, uint32_t nsid, uint32_t structure) {
    memset(page_of(controller, 5), 0, 4096);
    struct nvme_command command;
    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_IDENTIFY;
    command.nsid = nsid;
    command.prp1 = page_physical(controller, 5);
    command.dword10 = structure;
    return submit(controller, &controller->admin_queue, &command) == 0 ? 0 : -1;
}

static int create_io_queues(struct nvme_controller *controller) {
    struct nvme_command command;

    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_CREATE_CQ;
    command.prp1 = controller->io_queue.completion_physical;
    command.dword10 = ((QUEUE_ENTRIES - 1U) << 16) | 1U;
    command.dword11 = 1U;
    if (submit(controller, &controller->admin_queue, &command) != 0) return -1;

    memset(&command, 0, sizeof(command));
    command.dword0 = ADMIN_CREATE_SQ;
    command.prp1 = controller->io_queue.submission_physical;
    command.dword10 = ((QUEUE_ENTRIES - 1U) << 16) | 1U;
    command.dword11 = (1U << 16) | 1U;
    return submit(controller, &controller->admin_queue, &command) == 0 ? 0 : -1;
}

static void name_device(struct block_device *device, unsigned controller, uint32_t nsid) {
    char text[24];
    unsigned at = 0;
    const char prefix[] = "nvme";
    for (unsigned index = 0; prefix[index]; index++) text[at++] = prefix[index];
    char digits[12];
    unsigned count = 0;
    unsigned value = controller;
    do { digits[count++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    while (count) text[at++] = digits[--count];
    text[at++] = 'n';
    value = nsid;
    do { digits[count++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    while (count) text[at++] = digits[--count];
    text[at] = '\0';
    size_t limit = sizeof(device->name) - 1;
    for (size_t index = 0; index <= at && index < limit; index++) device->name[index] = text[index];
    device->name[limit] = '\0';
}

static void register_namespace(struct nvme_controller *controller, uint32_t nsid) {
    if (identify(controller, nsid, 0) != 0) return;
    const uint8_t *data = page_of(controller, 5);
    uint64_t size = 0;
    memcpy(&size, data, sizeof(size));
    uint8_t formatted = data[26] & 0x0FU;
    uint32_t format = 0;
    memcpy(&format, data + 128 + (size_t)formatted * 4U, sizeof(format));
    uint8_t shift = (uint8_t)((format >> 16) & 0xFFU);
    if (shift < 9U || shift > 12U || !size) return;

    struct nvme_namespace *space = kmalloc(sizeof(*space));
    if (!space) return;
    space->controller = controller;
    space->nsid = nsid;
    space->blocks = size;
    space->block_bytes = 1U << shift;
    space->sectors_per_block = space->block_bytes / BLOCK_SECTOR_SIZE;

    struct block_device device;
    memset(&device, 0, sizeof(device));
    name_device(&device, controller->index, nsid);
    device.sectors = space->blocks * space->sectors_per_block;
    device.read = nvme_read;
    device.write = nvme_write;
    device.flush = nvme_flush;
    device.context = space;
    if (block_register(&device) < 0) {
        kfree(space);
        return;
    }
    if (space->block_bytes != BLOCK_SECTOR_SIZE)
        kprintf("NVME: %u byte blocks, staged through as 512 byte sectors\n",
                (unsigned)space->block_bytes);
}

static void bring_up(const struct pci_device *pci) {
    uint64_t base = ((uint64_t)pci->bar[0] & ~0xFULL);
    if ((pci->bar[0] & 0x6U) == 0x4U) base |= (uint64_t)pci->bar[1] << 32;
    if (!base) return;

    struct pci_device device = *pci;
    pci_enable_bus_mastering(&device);
    struct nvme_controller *controller = kmalloc(sizeof(*controller));
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));
    mutex_init(&controller->lock, "nvme", LOCK_RANK_BLOCK);
    controller->next_command_id = 1;
    controller->index = controller_count;
    controller->registers = vmm_map_device(base, 0x2000U);
    controller->pages = dma_alloc(NVME_DMA_PAGES * 4096ULL, 4096ULL, &controller->pages_physical);
    if (!controller->registers || !controller->pages) {
        kprintf("NVME: controller could not be mapped\n");
        if (controller->pages) dma_free(controller->pages, NVME_DMA_PAGES * 4096ULL);
        kfree(controller);
        return;
    }
    memset(controller->pages, 0, NVME_DMA_PAGES * 4096U);

    uint32_t capability_high = read32(controller->registers + REG_CAP + 4U);
    controller->doorbell_stride = capability_high & 0x0FU;

    write32(controller->registers + REG_CC, read32(controller->registers + REG_CC) & ~CC_ENABLE);
    if (wait_ready(controller, 0) != 0) return;

    allocate_queue(controller, &controller->admin_queue, 0, 0, 1);
    allocate_queue(controller, &controller->io_queue, 1, 2, 3);
    controller->prp_list = (uint64_t *)page_of(controller, 4);
    controller->prp_list_physical = page_physical(controller, 4);

    write32(controller->registers + REG_AQA, ((QUEUE_ENTRIES - 1U) << 16) | (QUEUE_ENTRIES - 1U));
    write64(controller->registers + REG_ASQ, controller->admin_queue.submission_physical);
    write64(controller->registers + REG_ACQ, controller->admin_queue.completion_physical);

    write32(controller->registers + REG_CC, CC_ENABLE | (6U << 16) | (4U << 20));
    if (wait_ready(controller, 1) != 0) {
        kprintf("NVME: controller did not become ready\n");
        return;
    }
    if (create_io_queues(controller) != 0) {
        kprintf("NVME: I/O queue creation failed\n");
        return;
    }
    uint32_t namespaces = 1;
    if (identify(controller, 0, 1) == 0) {
        memcpy(&namespaces, page_of(controller, 5) + 516, sizeof(namespaces));
        if (!namespaces) namespaces = 1;
    }
    controller_count++;
    for (uint32_t nsid = 1; nsid <= namespaces; nsid++) register_namespace(controller, nsid);
}

void nvme_init(void) {
    struct pci_device pci;
    for (unsigned nth = 0; pci_find_nth_class(NVME_CLASS, NVME_SUBCLASS, nth, &pci) == 0; nth++)
        bring_up(&pci);
}
