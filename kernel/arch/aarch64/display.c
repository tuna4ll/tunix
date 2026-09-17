#include <stddef.h>
#include <stdint.h>

#include "../../include/boot_framebuffer.h"
#include "../../include/vmm.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);

#define FW_CFG_DATA 0x00U
#define FW_CFG_SELECTOR 0x08U
#define FW_CFG_DMA 0x10U
#define FW_CFG_SIGNATURE 0x0000U
#define FW_CFG_ID 0x0001U
#define FW_CFG_FILE_DIR 0x0019U
#define FW_CFG_FEATURE_DMA 2U
#define FW_CFG_DMA_ERROR 0x01U
#define FW_CFG_DMA_SELECT 0x08U
#define FW_CFG_DMA_WRITE 0x10U

#define RAMFB_WIDTH 1280U
#define RAMFB_HEIGHT 720U
#define FOURCC_XRGB8888 0x34325258U
#define BLOCK_2M 0x200000ULL

struct fw_cfg_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed));

struct ramfb_config {
    uint64_t address;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

static struct boot_framebuffer_info framebuffer;
static struct fw_cfg_access access_block;
static struct ramfb_config ramfb_block;
static uint64_t fw_cfg_physical;
static uint64_t ramfb_physical;
static uint64_t ramfb_bytes;

static int text_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t swap32(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0xFF00U) | ((value << 8) & 0xFF0000U) | (value << 24);
}

static uint64_t swap64(uint64_t value) {
    return ((uint64_t)swap32((uint32_t)value) << 32) | swap32((uint32_t)(value >> 32));
}

static void fill_rgb(uint64_t physical, uint32_t width, uint32_t height, uint32_t stride,
                     int red_first) {
    framebuffer.magic = TUNIX_BOOT_FB_MAGIC;
    framebuffer.version = TUNIX_BOOT_FB_VERSION;
    framebuffer.size = sizeof(framebuffer);
    framebuffer.physical_address = physical;
    framebuffer.pitch = stride;
    framebuffer.width = (uint16_t)width;
    framebuffer.height = (uint16_t)height;
    framebuffer.bits_per_pixel = 32;
    framebuffer.red_mask_size = 8;
    framebuffer.green_mask_size = 8;
    framebuffer.blue_mask_size = 8;
    framebuffer.red_field_position = red_first ? 0 : 16;
    framebuffer.green_field_position = 8;
    framebuffer.blue_field_position = red_first ? 16 : 0;
}

static int simple_framebuffer(void) {
    struct fdt_node node;
    for (unsigned index = 0; fdt_find_compatible("simple-framebuffer", index, &node) == 0; index++) {
        const char *status = fdt_property(&node, "status", NULL);
        if (status && !text_equal(status, "okay") && !text_equal(status, "ok")) continue;
        uint64_t base, size;
        if (fdt_reg_cpu(&node, 0, &base, &size) != 0) continue;
        const void *width = fdt_property(&node, "width", NULL);
        const void *height = fdt_property(&node, "height", NULL);
        const void *stride = fdt_property(&node, "stride", NULL);
        const char *format = fdt_property(&node, "format", NULL);
        if (!width || !height || !stride || !format) continue;
        int red_first;
        if (text_equal(format, "x8r8g8b8") || text_equal(format, "a8r8g8b8")) red_first = 0;
        else if (text_equal(format, "x8b8g8r8") || text_equal(format, "a8b8g8r8")) red_first = 1;
        else continue;
        fill_rgb(base, fdt_read32(width), fdt_read32(height), fdt_read32(stride), red_first);
        return 1;
    }
    return 0;
}

static uint64_t fw_cfg_register(uint32_t offset) {
    return aarch64_platform.fw_cfg_base + offset;
}

static void fw_cfg_select(uint16_t key) {
    *(volatile uint16_t *)fw_cfg_register(FW_CFG_SELECTOR) = (uint16_t)((key >> 8) | (key << 8));
}

static void fw_cfg_read(void *buffer, uint32_t length) {
    uint8_t *bytes = buffer;
    for (uint32_t index = 0; index < length; index++)
        bytes[index] = *(volatile uint8_t *)fw_cfg_register(FW_CFG_DATA);
}

static int fw_cfg_find(const char *name, uint16_t *key, uint32_t *size) {
    fw_cfg_select(FW_CFG_FILE_DIR);
    uint32_t count;
    fw_cfg_read(&count, sizeof(count));
    count = swap32(count);
    for (uint32_t index = 0; index < count && index < 256U; index++) {
        struct {
            uint32_t size;
            uint16_t select;
            uint16_t reserved;
            char name[56];
        } __attribute__((packed)) entry;
        fw_cfg_read(&entry, sizeof(entry));
        entry.name[55] = '\0';
        if (text_equal(entry.name, name)) {
            *key = (uint16_t)((entry.select >> 8) | (entry.select << 8));
            *size = swap32(entry.size);
            return 0;
        }
    }
    return -1;
}

static int ramfb(void) {
    if (!aarch64_platform.fw_cfg_base || !ramfb_physical) return 0;
    char signature[4];
    fw_cfg_select(FW_CFG_SIGNATURE);
    fw_cfg_read(signature, sizeof(signature));
    if (signature[0] != 'Q' || signature[1] != 'E' || signature[2] != 'M' || signature[3] != 'U')
        return 0;
    uint32_t features;
    fw_cfg_select(FW_CFG_ID);
    fw_cfg_read(&features, sizeof(features));
    if (!(features & FW_CFG_FEATURE_DMA)) return 0;

    uint16_t key;
    uint32_t size;
    if (fw_cfg_find("etc/ramfb", &key, &size) != 0 || size != sizeof(ramfb_block)) return 0;

    uint8_t *pixels = (uint8_t *)(DIRECT_MAP_BASE + ramfb_physical);
    for (uint64_t index = 0; index < ramfb_bytes; index++) pixels[index] = 0;

    ramfb_block.address = swap64(ramfb_physical);
    ramfb_block.fourcc = swap32(FOURCC_XRGB8888);
    ramfb_block.flags = 0;
    ramfb_block.width = swap32(RAMFB_WIDTH);
    ramfb_block.height = swap32(RAMFB_HEIGHT);
    ramfb_block.stride = swap32(RAMFB_WIDTH * 4U);

    uint64_t offset = aarch64_platform.load_offset;
    access_block.control = swap32(((uint32_t)key << 16) | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
    access_block.length = swap32(sizeof(ramfb_block));
    access_block.address = swap64((uint64_t)&ramfb_block + offset);

    uint64_t control = (uint64_t)&access_block + offset;
    *(volatile uint32_t *)fw_cfg_register(FW_CFG_DMA) = swap32((uint32_t)(control >> 32));
    *(volatile uint32_t *)fw_cfg_register(FW_CFG_DMA + 4U) = swap32((uint32_t)control);
    if (swap32(access_block.control) & FW_CFG_DMA_ERROR) return 0;

    fill_rgb(ramfb_physical, RAMFB_WIDTH, RAMFB_HEIGHT, RAMFB_WIDTH * 4U, 0);
    return 1;
}

void aarch64_display_reserve(void) {
    struct fdt_node node;
    uint64_t base, size;
    if (fdt_find_compatible("simple-framebuffer", 0, &node) == 0 &&
        fdt_reg_cpu(&node, 0, &base, &size) == 0) {
        aarch64_platform.display_hole_base = base & ~(BLOCK_2M - 1U);
        aarch64_platform.display_hole_size =
            ((base + size + BLOCK_2M - 1U) & ~(BLOCK_2M - 1U)) - aarch64_platform.display_hole_base;
        return;
    }
    if (aarch64_platform.uefi_system_table ||
        fdt_find_compatible("qemu,fw-cfg-mmio", 0, &node) != 0 ||
        fdt_reg_cpu(&node, 0, &fw_cfg_physical, &size) != 0 || !aarch64_platform.ram_count)
        return;
    aarch64_platform.fw_cfg_base = aarch64_early_map_device(fw_cfg_physical, 0x1000ULL);

    const struct aarch64_range *top = &aarch64_platform.ram[0];
    for (unsigned index = 1; index < aarch64_platform.ram_count; index++)
        if (aarch64_platform.ram[index].base > top->base) top = &aarch64_platform.ram[index];
    ramfb_bytes = (uint64_t)RAMFB_WIDTH * RAMFB_HEIGHT * 4U;
    uint64_t hole = (ramfb_bytes + BLOCK_2M - 1U) & ~(BLOCK_2M - 1U);
    uint64_t end = (top->base + top->size) & ~(BLOCK_2M - 1U);
    if (end - top->base < hole * 4U) return;
    ramfb_physical = end - hole;
    aarch64_platform.display_hole_base = ramfb_physical;
    aarch64_platform.display_hole_size = hole;
}

const struct boot_framebuffer_info *aarch64_display_setup(void) {
    const struct boot_framebuffer_info *firmware = uefi_framebuffer();
    if (!simple_framebuffer() && firmware) framebuffer = *firmware;
    if (framebuffer.magic || ramfb()) {
        kprintf("TUNIX: framebuffer %ux%u at %p\n", framebuffer.width, framebuffer.height,
                (void *)framebuffer.physical_address);
        return &framebuffer;
    }
    return NULL;
}
