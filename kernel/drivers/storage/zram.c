#include <stddef.h>
#include <stdint.h>
#include <tunix/block.h>
#include <tunix/devfs.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kprintf.h>
#include <tunix/kstring.h>
#include <tunix/lz4.h>
#include <tunix/mutex.h>
#include <tunix/pmm.h>
#include <tunix/swap.h>
#include <tunix/vfs.h>
#include <tunix/zram.h>

#define ZRAM_PAGE    4096U
#define ZRAM_SECTORS (ZRAM_PAGE / BLOCK_SECTOR_SIZE)
#define ZRAM_HUGE    (ZRAM_PAGE / 4U * 3U)
#define ZRAM_MAJOR   252U
#define ZRAM_HEADER  48U
#define ZRAM_NAME    "zram0"
#define ZRAM_SYSFS   "/sys/devices/virtual/block/zram0"

enum zram_kind { ZRAM_EMPTY, ZRAM_SAME, ZRAM_PACKED, ZRAM_RAW };

struct zram_slot {
    uint64_t value;
    uint32_t size;
    uint32_t kind;
};

static struct mutex zram_lock = MUTEX_INITIALIZER("zram", LOCK_RANK_BLOCK);
static struct zram_slot *slots;
static uint64_t page_count;
static uint64_t disk_bytes;
static int device_index = -1;
static struct vfs_node *device_node;

static uint64_t stored_pages;
static uint64_t compressed_bytes;
static uint64_t memory_used;
static uint64_t memory_used_max;
static uint64_t memory_limit;
static uint64_t same_pages;
static uint64_t huge_pages;
static uint64_t huge_pages_since;
static uint64_t failed_reads;
static uint64_t failed_writes;
static uint64_t invalid_io;
static uint64_t freed_notices;

static uint16_t hash_table[TUNIX_LZ4_TABLE_ENTRIES];
static uint8_t packed[TUNIX_LZ4_BOUND(ZRAM_PAGE)];
static uint8_t scratch[ZRAM_PAGE];

static void zram_guard_release(int *unused) {
    (void)unused;
    mutex_unlock(&zram_lock);
}

#define ZRAM_LOCKED \
    __attribute__((cleanup(zram_guard_release))) int zram_guard = (mutex_lock(&zram_lock), 0)

static uint64_t charge(uint32_t size) { return size + ZRAM_HEADER; }

static void slot_clear(struct zram_slot *slot) {
    if (slot->kind == ZRAM_PACKED || slot->kind == ZRAM_RAW) {
        kfree((void *)(uintptr_t)slot->value);
        memory_used -= charge(slot->size);
        compressed_bytes -= slot->size;
        if (slot->kind == ZRAM_RAW) huge_pages--;
    }
    if (slot->kind == ZRAM_SAME) same_pages--;
    if (slot->kind != ZRAM_EMPTY) stored_pages--;
    slot->value = 0;
    slot->size = 0;
    slot->kind = ZRAM_EMPTY;
}

static int same_filled(const uint8_t *page, uint64_t *pattern) {
    const uint64_t *words = (const uint64_t *)page;
    for (unsigned index = 1; index < ZRAM_PAGE / 8U; index++)
        if (words[index] != words[0]) return 0;
    *pattern = words[0];
    return 1;
}

static int slot_store(uint64_t page, const uint8_t *data) {
    struct zram_slot *slot = &slots[page];
    uint64_t pattern;
    if (same_filled(data, &pattern)) {
        slot_clear(slot);
        slot->value = pattern;
        slot->kind = ZRAM_SAME;
        same_pages++;
        stored_pages++;
        return 0;
    }
    size_t size = tunix_lz4_compress(data, ZRAM_PAGE, packed, sizeof(packed), hash_table);
    uint32_t kind = ZRAM_PACKED;
    const uint8_t *source = packed;
    if (!size || size > ZRAM_HUGE) {
        kind = ZRAM_RAW;
        size = ZRAM_PAGE;
        source = data;
    }
    uint64_t old_charge =
        slot->kind == ZRAM_PACKED || slot->kind == ZRAM_RAW ? charge(slot->size) : 0;
    if (memory_limit && memory_used - old_charge + charge((uint32_t)size) > memory_limit)
        return -ENOSPC;
    void *blob = kmalloc(size);
    if (!blob) return -ENOMEM;
    memcpy(blob, source, size);
    slot_clear(slot);
    slot->value = (uint64_t)(uintptr_t)blob;
    slot->size = (uint32_t)size;
    slot->kind = kind;
    stored_pages++;
    compressed_bytes += size;
    memory_used += charge((uint32_t)size);
    if (memory_used > memory_used_max) memory_used_max = memory_used;
    if (kind == ZRAM_RAW) {
        huge_pages++;
        huge_pages_since++;
    }
    return 0;
}

static int slot_load(uint64_t page, uint8_t *out) {
    const struct zram_slot *slot = &slots[page];
    switch (slot->kind) {
    case ZRAM_EMPTY: memset(out, 0, ZRAM_PAGE); return 0;
    case ZRAM_SAME:  {
        uint64_t *words = (uint64_t *)out;
        for (unsigned index = 0; index < ZRAM_PAGE / 8U; index++) words[index] = slot->value;
        return 0;
    }
    case ZRAM_RAW: memcpy(out, (const void *)(uintptr_t)slot->value, ZRAM_PAGE); return 0;
    default:
        return tunix_lz4_decompress((const uint8_t *)(uintptr_t)slot->value, slot->size, out,
                                    ZRAM_PAGE) == 0
            ? 0
            : -EIO;
    }
}

static int range_valid(uint64_t lba, uint32_t count) {
    return slots && count && lba < page_count * ZRAM_SECTORS &&
        count <= page_count * ZRAM_SECTORS - lba;
}

static int zram_read(void *context, uint64_t lba, uint32_t count, void *destination) {
    (void)context;
    ZRAM_LOCKED;
    if (!range_valid(lba, count)) {
        invalid_io++;
        return -1;
    }
    uint8_t *out = (uint8_t *)destination;
    while (count) {
        uint64_t page = lba / ZRAM_SECTORS;
        uint32_t within = (uint32_t)(lba % ZRAM_SECTORS);
        uint32_t take = ZRAM_SECTORS - within;
        if (take > count) take = count;
        if (!within && take == ZRAM_SECTORS) {
            if (slot_load(page, out) != 0) {
                failed_reads++;
                return -1;
            }
        } else {
            if (slot_load(page, scratch) != 0) {
                failed_reads++;
                return -1;
            }
            memcpy(out, scratch + within * BLOCK_SECTOR_SIZE, take * BLOCK_SECTOR_SIZE);
        }
        out += take * BLOCK_SECTOR_SIZE;
        lba += take;
        count -= take;
    }
    return 0;
}

static int zram_write(void *context, uint64_t lba, uint32_t count, const void *source) {
    (void)context;
    ZRAM_LOCKED;
    if (!range_valid(lba, count)) {
        invalid_io++;
        return -1;
    }
    const uint8_t *in = (const uint8_t *)source;
    while (count) {
        uint64_t page = lba / ZRAM_SECTORS;
        uint32_t within = (uint32_t)(lba % ZRAM_SECTORS);
        uint32_t take = ZRAM_SECTORS - within;
        if (take > count) take = count;
        const uint8_t *whole = in;
        if (within || take != ZRAM_SECTORS) {
            if (slot_load(page, scratch) != 0) {
                failed_writes++;
                return -1;
            }
            memcpy(scratch + within * BLOCK_SECTOR_SIZE, in, take * BLOCK_SECTOR_SIZE);
            whole = scratch;
        }
        if (slot_store(page, whole) != 0) {
            failed_writes++;
            return -1;
        }
        in += take * BLOCK_SECTOR_SIZE;
        lba += take;
        count -= take;
    }
    return 0;
}

static int zram_discard(void *context, uint64_t lba, uint32_t count) {
    (void)context;
    ZRAM_LOCKED;
    if (!range_valid(lba, count)) return -1;
    uint64_t first = (lba + ZRAM_SECTORS - 1U) / ZRAM_SECTORS;
    uint64_t end = (lba + count) / ZRAM_SECTORS;
    for (uint64_t page = first; page < end; page++) {
        if (slots[page].kind != ZRAM_EMPTY) freed_notices++;
        slot_clear(&slots[page]);
    }
    return 0;
}

static int zram_flush(void *context) {
    (void)context;
    return 0;
}

static int64_t text_reply(const char *text, size_t length, uint64_t offset, size_t size,
                          void *output) {
    if (offset >= length) return 0;
    if (size > length - offset) size = length - (size_t)offset;
    memcpy(output, text + offset, size);
    return (int64_t)size;
}

static size_t put_number(char *out, size_t at, size_t room, uint64_t value) {
    char digits[24];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (count && at + 1 < room) out[at++] = digits[--count];
    return at;
}

static size_t put_text(char *out, size_t at, size_t room, const char *text) {
    while (*text && at + 1 < room) out[at++] = *text++;
    return at;
}

static int parse_size(const void *input, size_t size, uint64_t *out) {
    const char *text = (const char *)input;
    size_t at = 0;
    uint64_t value = 0;
    while (at < size && (text[at] == ' ' || text[at] == '\t')) at++;
    size_t first = at;
    while (at < size && text[at] >= '0' && text[at] <= '9') {
        if (value > (UINT64_MAX - 9U) / 10U) return -1;
        value = value * 10U + (uint64_t)(text[at++] - '0');
    }
    if (at == first) return -1;
    unsigned shift = 0;
    if (at < size) {
        switch (text[at]) {
        case 'k':
        case 'K':
            shift = 10;
            at++;
            break;
        case 'm':
        case 'M':
            shift = 20;
            at++;
            break;
        case 'g':
        case 'G':
            shift = 30;
            at++;
            break;
        case 't':
        case 'T':
            shift = 40;
            at++;
            break;
        default: break;
        }
    }
    while (at < size && (text[at] == '\n' || text[at] == ' ')) at++;
    if (at != size) return -1;
    if (shift && value > (UINT64_MAX >> shift)) return -1;
    *out = value << shift;
    return 0;
}

static int device_busy(void) {
    if (device_index < 0) return 0;
    return swap_block_in_use(device_index);
}

static void publish_size(void) {
    block_set_sectors(device_index, page_count * ZRAM_SECTORS);
    if (device_node) device_node->length = page_count * ZRAM_PAGE;
}

static void release_all(void) {
    if (slots)
        for (uint64_t page = 0; page < page_count; page++) slot_clear(&slots[page]);
    kfree(slots);
    slots = NULL;
    page_count = 0;
    disk_bytes = 0;
    huge_pages_since = 0;
    memory_used_max = memory_used;
}

enum zram_attribute {
    ATTR_DISKSIZE,
    ATTR_INITSTATE,
    ATTR_RESET,
    ATTR_ALGORITHM,
    ATTR_STREAMS,
    ATTR_LIMIT,
    ATTR_USED_MAX,
    ATTR_MM_STAT,
    ATTR_IO_STAT,
    ATTR_COMPACT,
    ATTR_DEV,
    ATTR_SIZE,
    ATTR_UEVENT,
};

static int64_t attribute_read(struct vfs_node *node, uint64_t offset, size_t size, void *output) {
    char text[256];
    size_t at = 0;
    ZRAM_LOCKED;
    switch ((enum zram_attribute)node->inode) {
    case ATTR_DISKSIZE:  at = put_number(text, at, sizeof(text), disk_bytes); break;
    case ATTR_INITSTATE: at = put_number(text, at, sizeof(text), slots != NULL); break;
    case ATTR_ALGORITHM: at = put_text(text, at, sizeof(text), "[lz4]"); break;
    case ATTR_STREAMS:   at = put_number(text, at, sizeof(text), 1); break;
    case ATTR_LIMIT:     at = put_number(text, at, sizeof(text), memory_limit); break;
    case ATTR_USED_MAX:  at = put_number(text, at, sizeof(text), memory_used_max); break;
    case ATTR_MM_STAT:   {
        const uint64_t values[] = {
            stored_pages * ZRAM_PAGE, compressed_bytes, memory_used, memory_limit,
            memory_used_max,          same_pages,       0,           huge_pages,
            huge_pages_since};
        for (unsigned index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
            if (index) at = put_text(text, at, sizeof(text), " ");
            at = put_number(text, at, sizeof(text), values[index]);
        }
        break;
    }
    case ATTR_IO_STAT: {
        const uint64_t values[] = {failed_reads, failed_writes, invalid_io, freed_notices};
        for (unsigned index = 0; index < 4; index++) {
            if (index) at = put_text(text, at, sizeof(text), " ");
            at = put_number(text, at, sizeof(text), values[index]);
        }
        break;
    }
    case ATTR_DEV:
        at = put_number(text, at, sizeof(text), ZRAM_MAJOR);
        at = put_text(text, at, sizeof(text), ":0");
        break;
    case ATTR_SIZE: at = put_number(text, at, sizeof(text), page_count * ZRAM_SECTORS); break;
    case ATTR_UEVENT:
        at = put_text(text, at, sizeof(text), "MAJOR=252\nMINOR=0\nDEVNAME=zram0\nDEVTYPE=disk");
        break;
    default: return -EINVAL;
    }
    at = put_text(text, at, sizeof(text), "\n");
    return text_reply(text, at, offset, size, output);
}

static int64_t attribute_write(struct vfs_node *node, uint64_t offset, size_t size,
                               const void *input) {
    (void)offset;
    uint64_t value = 0;
    switch ((enum zram_attribute)node->inode) {
    case ATTR_DISKSIZE: {
        if (parse_size(input, size, &value) != 0) return -EINVAL;
        uint64_t pages = value / ZRAM_PAGE;
        if (!pages) return -EINVAL;
        if (pages > pmm_usable_page_count() * 4ULL) return -ENOMEM;
        struct zram_slot *table = (struct zram_slot *)kmalloc(pages * sizeof(*table));
        if (!table) return -ENOMEM;
        memset(table, 0, pages * sizeof(*table));
        {
            ZRAM_LOCKED;
            if (slots) {
                kfree(table);
                return -EBUSY;
            }
            slots = table;
            page_count = pages;
            disk_bytes = pages * ZRAM_PAGE;
            publish_size();
        }
        kprintf("ZRAM: zram0 is %u MiB\n", (unsigned)(disk_bytes >> 20));
        return (int64_t)size;
    }
    case ATTR_RESET: {
        if (parse_size(input, size, &value) != 0 || value == 0) return -EINVAL;
        if (device_busy()) return -EBUSY;
        ZRAM_LOCKED;
        release_all();
        publish_size();
        return (int64_t)size;
    }
    case ATTR_ALGORITHM: {
        const char *text = (const char *)input;
        size_t length = size;
        while (length && (text[length - 1] == '\n' || text[length - 1] == ' ')) length--;
        if (length != 3 || memcmp(text, "lz4", 3) != 0) return -EINVAL;
        ZRAM_LOCKED;
        return slots ? -EBUSY : (int64_t)size;
    }
    case ATTR_STREAMS: return (int64_t)size;
    case ATTR_LIMIT:   {
        if (parse_size(input, size, &value) != 0) return -EINVAL;
        ZRAM_LOCKED;
        memory_limit = value;
        return (int64_t)size;
    }
    case ATTR_USED_MAX: {
        if (parse_size(input, size, &value) != 0 || value != 0) return -EINVAL;
        ZRAM_LOCKED;
        memory_used_max = memory_used;
        return (int64_t)size;
    }
    case ATTR_COMPACT: return (int64_t)size;
    default:           return -EINVAL;
    }
}

static void attribute(struct vfs_node *directory, const char *name, enum zram_attribute which,
                      uint32_t mode) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE | VFS_UNSIZED);
    if (!node) return;
    node->mode = mode;
    node->length = 4096;
    node->inode = which;
    node->read = attribute_read;
    if (mode & 0200) node->write = attribute_write;
    if (vfs_attach(directory, node) != 0) vfs_free_node(node);
}

void zram_init(void) {
    struct block_device device;
    memset(&device, 0, sizeof(device));
    strncpy(device.name, "zram", sizeof(device.name) - 1);
    device.read = zram_read;
    device.write = zram_write;
    device.flush = zram_flush;
    device.discard = zram_discard;
    device_index = block_register_named(&device, ZRAM_NAME, ZRAM_MAJOR, 0);
    if (device_index < 0) {
        kprintf("ZRAM: cannot register zram0\n");
        return;
    }
    devfs_add_block(device_index);
    device_node = vfs_lookup("/dev/" ZRAM_NAME);

    struct vfs_node *directory = vfs_mkdir_p(ZRAM_SYSFS);
    if (!directory) return;
    attribute(directory, "disksize", ATTR_DISKSIZE, 0644);
    attribute(directory, "initstate", ATTR_INITSTATE, 0444);
    attribute(directory, "reset", ATTR_RESET, 0200);
    attribute(directory, "comp_algorithm", ATTR_ALGORITHM, 0644);
    attribute(directory, "max_comp_streams", ATTR_STREAMS, 0644);
    attribute(directory, "mem_limit", ATTR_LIMIT, 0644);
    attribute(directory, "mem_used_max", ATTR_USED_MAX, 0644);
    attribute(directory, "mm_stat", ATTR_MM_STAT, 0444);
    attribute(directory, "io_stat", ATTR_IO_STAT, 0444);
    attribute(directory, "compact", ATTR_COMPACT, 0200);
    attribute(directory, "dev", ATTR_DEV, 0444);
    attribute(directory, "size", ATTR_SIZE, 0444);
    attribute(directory, "uevent", ATTR_UEVENT, 0644);
    (void)vfs_mkdir_p("/sys/block");
    (void)vfs_create_symlink("/sys/block/zram0", "../devices/virtual/block/zram0", 0);
    (void)vfs_mkdir_p("/sys/dev/block");
    (void)vfs_create_symlink("/sys/dev/block/252:0", "../../devices/virtual/block/zram0", 0);
}
