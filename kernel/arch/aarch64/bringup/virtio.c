#include <stddef.h>
#include <stdint.h>

#include "arch.h"

#define VIRTIO_MMIO_MAGIC_VALUE 0x74726976U     // "virt"

#define VIRTIO_MMIO_MAGIC               0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03C
#define VIRTIO_MMIO_QUEUE_PFN           0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_CONFIG              0x100

#define STATUS_ACKNOWLEDGE 1U
#define STATUS_DRIVER      2U
#define STATUS_DRIVER_OK   4U

#define DESC_F_NEXT  1U
#define DESC_F_WRITE 2U

#define QUEUE_SIZE 8U
#define SECTOR_BYTES 512U

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

static const char *device_name(uint32_t id) {
    switch (id) {
    case 1:  return "net";
    case 2:  return "block";
    case 3:  return "console";
    case 4:  return "entropy";
    case 16: return "gpu";
    case 18: return "input";
    default: return "unknown";
    }
}

uint64_t virtio_mmio_slot(unsigned slot) {
    return phys_to_virt(VIRTIO_MMIO_BASE + (uint64_t)slot * VIRTIO_MMIO_STRIDE);
}

int virtio_mmio_find(uint32_t device_id) {
    for (unsigned slot = 0; slot < VIRTIO_MMIO_SLOTS; slot++) {
        uint64_t base = virtio_mmio_slot(slot);
        if (mmio_read32(base + VIRTIO_MMIO_MAGIC) != VIRTIO_MMIO_MAGIC_VALUE) continue;
        if (mmio_read32(base + VIRTIO_MMIO_DEVICE_ID) == device_id) return (int)slot;
    }
    return -1;
}

unsigned virtio_mmio_probe(void) {
    unsigned found = 0;
    for (unsigned slot = 0; slot < VIRTIO_MMIO_SLOTS; slot++) {
        uint64_t base = virtio_mmio_slot(slot);
        if (mmio_read32(base + VIRTIO_MMIO_MAGIC) != VIRTIO_MMIO_MAGIC_VALUE) continue;

        uint32_t id = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
        if (!id) continue;                      // an empty slot QEMU still maps

        kprintf("virtio-mmio slot %u: %s (id %u, version %u, irq %u)\n",
                slot, device_name(id), id,
                mmio_read32(base + VIRTIO_MMIO_VERSION),
                VIRTIO_MMIO_IRQ_BASE + slot);
        found++;
    }
    return found;
}

static uint64_t blk_base;
static uint64_t blk_capacity;
static uint64_t queue_pa;
static uint64_t buffer_pa;
static uint16_t last_used;

static struct virtq_desc *descriptors;
static volatile uint16_t *avail_idx;
static volatile uint16_t *avail_ring;
static volatile uint16_t *used_idx;

// The queue lives in two pages: descriptors and the available ring in the
// first, the used ring at the start of the second, which is what the legacy
// transport's single page-frame-number plus 4 KiB alignment asks for.
static void attach_queue(uint64_t pa) {
    uint64_t first = phys_to_virt(pa);
    descriptors = (struct virtq_desc *)first;
    avail_idx = (volatile uint16_t *)(first + QUEUE_SIZE * sizeof(struct virtq_desc) + 2);
    avail_ring = avail_idx + 1;
    used_idx = (volatile uint16_t *)(phys_to_virt(pa + 4096) + 2);
}

int virtio_blk_init(void) {
    int slot = virtio_mmio_find(2);
    if (slot < 0) return -1;

    blk_base = virtio_mmio_slot((unsigned)slot);
    if (mmio_read32(blk_base + VIRTIO_MMIO_VERSION) != 1) return -2;   // legacy only

    mmio_write32(blk_base + VIRTIO_MMIO_STATUS, 0);
    mmio_write32(blk_base + VIRTIO_MMIO_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write32(blk_base + VIRTIO_MMIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write32(blk_base + VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    (void)mmio_read32(blk_base + VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_write32(blk_base + VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(blk_base + VIRTIO_MMIO_DRIVER_FEATURES, 0);   // plain reads need none

    mmio_write32(blk_base + VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    mmio_write32(blk_base + VIRTIO_MMIO_QUEUE_SEL, 0);
    if (mmio_read32(blk_base + VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE) return -3;

    queue_pa = (uint64_t)pmm_alloc_pages(2);
    buffer_pa = (uint64_t)pmm_alloc_page();
    if (!queue_pa || !buffer_pa) return -4;
    attach_queue(queue_pa);

    mmio_write32(blk_base + VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
    mmio_write32(blk_base + VIRTIO_MMIO_QUEUE_ALIGN, 4096);
    mmio_write32(blk_base + VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(queue_pa >> 12));

    mmio_write32(blk_base + VIRTIO_MMIO_STATUS,
                 STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);

    uint64_t low = mmio_read32(blk_base + VIRTIO_MMIO_CONFIG);
    uint64_t high = mmio_read32(blk_base + VIRTIO_MMIO_CONFIG + 4);
    blk_capacity = low | (high << 32);
    last_used = *used_idx;
    return 0;
}

uint64_t virtio_blk_capacity(void) {
    return blk_capacity;
}

int virtio_blk_read(uint64_t sector, void *out) {
    if (!blk_base) return -1;

    uint64_t scratch = phys_to_virt(buffer_pa);
    uint32_t *request = (uint32_t *)scratch;
    request[0] = 0;                                     // VIRTIO_BLK_T_IN
    request[1] = 0;
    *(uint64_t *)(scratch + 8) = sector;

    uint8_t *data = (uint8_t *)(scratch + 512);
    volatile uint8_t *status = (volatile uint8_t *)(scratch + 1024);
    *status = 0xFF;

    descriptors[0].addr = buffer_pa;
    descriptors[0].len = 16;
    descriptors[0].flags = DESC_F_NEXT;
    descriptors[0].next = 1;

    descriptors[1].addr = buffer_pa + 512;
    descriptors[1].len = SECTOR_BYTES;
    descriptors[1].flags = DESC_F_NEXT | DESC_F_WRITE;
    descriptors[1].next = 2;

    descriptors[2].addr = buffer_pa + 1024;
    descriptors[2].len = 1;
    descriptors[2].flags = DESC_F_WRITE;
    descriptors[2].next = 0;

    avail_ring[*avail_idx % QUEUE_SIZE] = 0;
    dsb_sy();
    *avail_idx = (uint16_t)(*avail_idx + 1);
    dsb_sy();
    mmio_write32(blk_base + VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    for (uint64_t spin = 0; spin < 200000000UL; spin++) {
        if (*used_idx != last_used) break;
        dsb_sy();
    }
    if (*used_idx == last_used) return -2;              // the device never answered
    last_used = *used_idx;

    mmio_write32(blk_base + VIRTIO_MMIO_INTERRUPT_ACK,
                 mmio_read32(blk_base + VIRTIO_MMIO_INTERRUPT_STATUS));
    if (*status != 0) return -3;

    for (unsigned i = 0; i < SECTOR_BYTES; i++) ((uint8_t *)out)[i] = data[i];
    return 0;
}
