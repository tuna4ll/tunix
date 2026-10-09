#include <stddef.h>
#include <stdint.h>

#include <tunix/boot.h>
#include <tunix/boot_framebuffer.h>
#include <tunix/display.h>
#include <tunix/nv50.h>
#include <tunix/pci.h>
#include <tunix/vmm.h>

#include "priv.h"

#define PCI_VENDOR_NVIDIA 0x10DEU
#define PCI_CLASS_DISPLAY 0x03U
#define NV_BAR0_BYTES     0x1000000ULL
#define VRAM_SIZE_REG     0x10020CU

extern void kprintf(const char *fmt, ...);

#define BUFFER_ALIGN 0x100000U
#define DRM_BUFFERS  2U

static struct nv50_device gpu;
static uint8_t vbios_image[VBIOS_IMAGE_BYTES];
static uint8_t gpu_bus, gpu_slot, gpu_function;
static uint8_t *buffer_map[DRM_BUFFERS];
static uint32_t buffer_vram[DRM_BUFFERS];
static unsigned back_index;

static int find_gpu(uint8_t *bus_out, uint8_t *slot_out, uint8_t *function_out) {
    for (unsigned bus = 0; bus < 256U; bus++) {
        for (uint8_t slot = 0; slot < 32U; slot++) {
            uint32_t header = pci_config_read32((uint8_t)bus, slot, 0, 0x0CU);
            uint8_t functions = (header & 0x00800000U) ? 8U : 1U;
            for (uint8_t function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, slot, function, 0);
                if ((id & 0xFFFFU) != PCI_VENDOR_NVIDIA) continue;
                uint32_t class_code = pci_config_read32((uint8_t)bus, slot, function, 0x08U);
                if ((class_code >> 24) != PCI_CLASS_DISPLAY) continue;
                *bus_out = (uint8_t)bus;
                *slot_out = slot;
                *function_out = function;
                return 0;
            }
        }
    }
    return -1;
}

static int is_nv50_family(uint32_t boot0) {
    uint32_t family = (boot0 >> 20) & 0x1F0U;
    return family == 0x50U || family == 0x80U || family == 0x90U || family == 0xA0U;
}

static int map_gpu(void) {
    uint8_t bus = 0, slot = 0, function = 0;
    if (find_gpu(&bus, &slot, &function) != 0) return -1;
    gpu_bus = bus;
    gpu_slot = slot;
    gpu_function = function;
    uint32_t bar0 = pci_config_read32(bus, slot, function, 0x10U);
    uint32_t bar1_low = pci_config_read32(bus, slot, function, 0x14U);
    uint32_t bar1_high =
        ((bar1_low >> 1) & 3U) == 2U ? pci_config_read32(bus, slot, function, 0x18U) : 0;
    gpu.bar1 = ((uint64_t)bar1_high << 32) | (bar1_low & 0xFFFFFFF0U);
    if ((bar0 & 1U) || !(bar0 & 0xFFFFFFF0U) || !gpu.bar1) {
        kprintf("NV50: the bars are not assigned\n");
        return -1;
    }
    gpu.bar0 = vmm_map_device(bar0 & 0xFFFFFFF0U, NV_BAR0_BYTES);
    if (!gpu.bar0) return -1;
    uint32_t boot0 = nv50_rd32(&gpu, 0);
    if (!is_nv50_family(boot0)) {
        kprintf("NV50: chip %x is not an nv50 family part\n", boot0);
        return -1;
    }
    gpu.vram_bytes = nv50_rd32(&gpu, VRAM_SIZE_REG) & 0xFFFFFF00U;
    kprintf("NV50: chip %x at %u:%u.%u, %u MiB vram\n", boot0, bus, slot, function,
            gpu.vram_bytes >> 20);
    return 0;
}

static int nv50_setup(const struct boot_framebuffer_info *boot, struct boot_framebuffer_info *out) {
    if (!boot_command_line_flag("nv50")) return -1;
    if (map_gpu() != 0) return -1;
    if (nv50_bios_read(&gpu, vbios_image, sizeof(vbios_image)) != 0) {
        kprintf("NV50: no video bios image\n");
        return -1;
    }
    if (nv50_disp_init(&gpu) != 0 || nv50_disp_modeset(&gpu) != 0) return -1;

    *out = *boot;
    out->physical_address = gpu.bar1 + VRAM_SURFACE;
    out->pitch = gpu.pitch;
    out->width = (uint16_t)gpu.width;
    out->height = (uint16_t)gpu.height;
    out->bits_per_pixel = 32;
    out->red_mask_size = 8;
    out->red_field_position = 16;
    out->green_mask_size = 8;
    out->green_field_position = 8;
    out->blue_mask_size = 8;
    out->blue_field_position = 0;
    out->reserved_mask_size = 8;
    out->reserved_field_position = 24;
    return 0;
}

static uint8_t *back_buffer(void) { return buffer_map[back_index]; }

static int flip_to_back(void) {
    if (nv50_disp_flip(&gpu, buffer_vram[back_index]) != 0) return -1;
    back_index = (back_index + 1U) % DRM_BUFFERS;
    return 0;
}

static int flip_to_console(void) {
    if (gpu.front == VRAM_SURFACE) return 0;
    if (!nv50_disp_flip_idle(&gpu, 1)) return -1;
    return nv50_disp_flip(&gpu, VRAM_SURFACE);
}

static int console_in_front(void) { return gpu.front == VRAM_SURFACE; }

static int idle(int may_service) { return nv50_disp_flip_idle(&gpu, may_service); }

static const struct display_flipper flipper = {
    .back_buffer = back_buffer,
    .flip_to_back = flip_to_back,
    .flip_to_console = flip_to_console,
    .console_in_front = console_in_front,
    .idle = idle,
};

static int pci_probe(const struct pci_device *device) {
    return device->bus == gpu_bus && device->slot == gpu_slot && device->function == gpu_function
        ? 0
        : -1;
}

static const struct pci_device_id pci_ids[] = {
    {PCI_VENDOR_NVIDIA, PCI_ANY_ID, PCI_CLASS_DISPLAY, PCI_ANY_ID},
};

static struct pci_driver pci_driver = {
    .name = "nv50",
    .ids = pci_ids,
    .id_count = sizeof(pci_ids) / sizeof(pci_ids[0]),
    .probe = pci_probe,
};

static void nv50_late(void) {
    (void)pci_register_driver(&pci_driver);
    uint32_t bytes = gpu.pitch * gpu.height;
    uint32_t stride = (bytes + BUFFER_ALIGN - 1U) & ~(BUFFER_ALIGN - 1U);
    for (unsigned index = 0; index < DRM_BUFFERS; index++) {
        buffer_vram[index] = VRAM_SURFACE + stride * (index + 1U);
        buffer_map[index] =
            (uint8_t *)vmm_map_device_write_combining(gpu.bar1 + buffer_vram[index], bytes);
        if (!buffer_map[index]) {
            kprintf("NV50: the flip buffers could not be mapped\n");
            return;
        }
    }
    display_register_flipper(&flipper);
    kprintf("NV50: page flipping between two buffers\n");
}

const struct display_early_driver nv50_display_driver = {
    .name = "nv50",
    .setup = nv50_setup,
    .late = nv50_late,
};
