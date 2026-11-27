#include <stddef.h>
#include <stdint.h>

#include <tunix/backlight.h>
#include <tunix/boot.h>
#include <tunix/boot_framebuffer.h>
#include <tunix/cpu.h>
#include <tunix/display.h>
#include <tunix/irq.h>
#include <tunix/lock.h>
#include <tunix/nv50.h>
#include <tunix/pci.h>
#include <tunix/process.h>
#include <tunix/time.h>
#include <tunix/vmm.h>
#include <tunix/workqueue.h>

#include "priv.h"
#include <tunix/kprintf.h>

#define PCI_VENDOR_NVIDIA 0x10DEU
#define PCI_CLASS_DISPLAY 0x03U
#define NV_BAR0_BYTES     0x1000000ULL
#define VRAM_SIZE_REG     0x10020CU

#define BUFFER_ALIGN    0x100000U
#define BACKLIGHT_SCALE 100U
#define DRM_BUFFERS     2U

static struct nv50_device gpu;
static uint8_t vbios_image[VBIOS_IMAGE_BYTES];
static uint8_t gpu_bus, gpu_slot, gpu_function;
static uint8_t *buffer_map[DRM_BUFFERS];
static uint32_t buffer_vram[DRM_BUFFERS];
static unsigned back_index;
static struct pci_device pci_device;
static int pci_bound;
static int vblank_live;
static uint64_t vblank_count;
static uint64_t vblank_ns;

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
    if (nv50_dcb_parse(&gpu) == 0) nv50_check_scripts(&gpu);
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

// Whatever pushes to the core channel -- flips, the cursor's image, gamma --
// goes one at a time: DRM's ioctls, its polls and the vblank work all may.
static struct lock display_lock = LOCK_INITIALIZER("nv50 display", LOCK_RANK_DEVICE);

// A present the copy engine is still copying into the back buffer: once its
// fence signals, the next look at idle() flips to it.
static uint32_t copy_fence;
static uint32_t copy_target;

static void finish_copy_locked(void) {
    if (!copy_fence || !nv50_fifo_done(&gpu, copy_fence)) return;
    copy_fence = 0;
    (void)nv50_disp_flip(&gpu, copy_target);
}

static int flip_to_back(void) {
    lock_acquire(&display_lock);
    int status = nv50_disp_flip(&gpu, buffer_vram[back_index]);
    if (!status) back_index = (back_index + 1U) % DRM_BUFFERS;
    lock_release(&display_lock);
    return status ? -1 : 0;
}

static int flip_to_console(void) {
    lock_acquire(&display_lock);
    int status = 0;
    if (gpu.front != VRAM_SURFACE) {
        finish_copy_locked();
        status = copy_fence || !nv50_disp_flip_idle(&gpu, 1) ? -1 : nv50_disp_flip(&gpu, VRAM_SURFACE);
    }
    lock_release(&display_lock);
    return status ? -1 : 0;
}

static int console_in_front(void) { return gpu.front == VRAM_SURFACE; }

static int idle(int may_service) {
    lock_acquire(&display_lock);
    finish_copy_locked();
    int done = !copy_fence && nv50_disp_flip_idle(&gpu, may_service);
    lock_release(&display_lock);
    return done;
}

// The pages of DRM's buffers the copy engine has been given, kept until
// DRM lets a buffer go.
#define MAPPINGS 8U
static struct {
    const uint64_t *pages;
    uint64_t count;
    uint64_t address;
} mappings[MAPPINGS];
static unsigned next_mapping;
static int accelerated;

static uint64_t mapping_for(const uint64_t *pages, uint64_t count) {
    for (unsigned index = 0; index < MAPPINGS; index++)
        if (mappings[index].pages == pages && mappings[index].count == count)
            return mappings[index].address;
    // No copy is in flight here (present waits for idle), so any can go.
    unsigned slot = next_mapping++ % MAPPINGS;
    if (mappings[slot].pages)
        nv50_gart_unmap(&gpu, mappings[slot].address, (uint32_t)mappings[slot].count);
    mappings[slot].pages = NULL;
    uint64_t address = nv50_gart_map(&gpu, pages, (uint32_t)count);
    if (!address) return 0;
    mappings[slot].pages = pages;
    mappings[slot].count = count;
    mappings[slot].address = address;
    return address;
}

// The copy engine copies ROWS of ROW_BYTES from the pages into the back
// buffer, and idle() flips once it is done; -1 leaves the copy to DRM.
static int present_pages(const uint64_t *pages, uint64_t page_count, uint32_t pitch,
                         uint32_t row_bytes, uint32_t rows) {
    if (!accelerated || !pages || !page_count || !rows || page_count > 0x20000U) return -1;
    if ((uint64_t)pitch * (rows - 1U) + row_bytes > page_count * 4096ULL) return -1;
    lock_acquire(&display_lock);
    int status = -1;
    uint64_t source = copy_fence ? 0 : mapping_for(pages, page_count);
    if (source) {
        uint32_t target = buffer_vram[back_index];
        uint32_t fence = nv50_copy_rect(&gpu, target, gpu.pitch, source, pitch, row_bytes, rows);
        if (fence) {
            copy_fence = fence;
            copy_target = target;
            back_index = (back_index + 1U) % DRM_BUFFERS;
            status = 0;
        }
    }
    lock_release(&display_lock);
    return status;
}

static void forget_pages(const uint64_t *pages) {
    lock_acquire(&display_lock);
    for (unsigned index = 0; index < MAPPINGS; index++) {
        if (mappings[index].pages != pages) continue;
        // The engine may still be reading them for the last present.
        uint64_t deadline = time_uptime_ns() + 100000000ULL;
        while (copy_fence && !nv50_fifo_done(&gpu, copy_fence) && time_uptime_ns() < deadline)
            cpu_relax();
        finish_copy_locked();
        nv50_gart_unmap(&gpu, mappings[index].address, (uint32_t)mappings[index].count);
        mappings[index].pages = NULL;
    }
    lock_release(&display_lock);
}

// The cursor image and the gamma ramp, written through BAR1 into VRAM the
// display reads them from.
#define CURSOR_PIXELS (64U * 64U)
#define LUT_ENTRIES   257U
static volatile uint32_t *cursor_map;
static volatile uint16_t *lut_map;
static int cursor_channel = -1;
static int cursor_on;

static int cursor_set_locked(const uint32_t *image) {
    if (!cursor_map) return -1;
    if (cursor_channel < 0) cursor_channel = nv50_disp_cursor_start(&gpu) == 0;
    if (!cursor_channel) return -1;
    if (!image) {
        if (!cursor_on) return 0;
        cursor_on = 0;
        return nv50_disp_cursor_show(&gpu, 0);
    }
    for (uint32_t index = 0; index < CURSOR_PIXELS; index++) cursor_map[index] = image[index];
    (void)cursor_map[CURSOR_PIXELS - 1U];
    if (cursor_on) return 0;
    cursor_on = 1;
    return nv50_disp_cursor_show(&gpu, VRAM_CURSOR);
}

static int cursor_set(const uint32_t *image) {
    lock_acquire(&display_lock);
    int first = cursor_channel < 0;
    int status = cursor_set_locked(image);
    lock_release(&display_lock);
    if (first && !cursor_channel) kprintf("NV50: the cursor channel did not start\n");
    return status;
}

static int cursor_move(int32_t x, int32_t y) {
    return cursor_channel > 0 ? nv50_disp_cursor_move(&gpu, x, y) : -1;
}

// 16-bit DRM gamma to the LUT's 11 bits, rounded, in its top bits.
static uint16_t lut_value(uint16_t value) {
    return (uint16_t)((((uint32_t)value * 0x7FFU + 0x7FFFU) / 0xFFFFU) << 3);
}

static int gamma_set_locked(const uint16_t *red, const uint16_t *green, const uint16_t *blue) {
    if (!lut_map) return -1;
    if (!red) return nv50_disp_gamma(&gpu, 0);
    for (uint32_t index = 0; index < LUT_ENTRIES; index++) {
        uint32_t from = index < 256U ? index : 255U;
        lut_map[index * 4U + 0U] = lut_value(red[from]);
        lut_map[index * 4U + 1U] = lut_value(green[from]);
        lut_map[index * 4U + 2U] = lut_value(blue[from]);
    }
    (void)lut_map[0];
    return nv50_disp_gamma(&gpu, VRAM_LUT);
}


// Tunix takes interrupts only while a processor is in user mode or idle, so a
// vblank's may land long after it: vblank is the hardware's while the last one
// is recent, and DRM paces by its own clock while it is not.
#define VBLANK_STALE_NS 100000000ULL

static int vblank(uint64_t *sequence, uint64_t *time_ns) {
    if (!__atomic_load_n(&vblank_live, __ATOMIC_ACQUIRE)) return -1;
    uint64_t at = __atomic_load_n(&vblank_ns, __ATOMIC_ACQUIRE);
    if (!at || time_uptime_ns() - at > VBLANK_STALE_NS) return -1;
    *sequence = __atomic_load_n(&vblank_count, __ATOMIC_ACQUIRE);
    *time_ns = at;
    return 0;
}

static int gamma_set(const uint16_t *red, const uint16_t *green, const uint16_t *blue) {
    lock_acquire(&display_lock);
    int status = gamma_set_locked(red, green, blue);
    lock_release(&display_lock);
    return status;
}

static const uint8_t *edid(void) {
    if (gpu.lvds_output < 0 || !gpu.outputs[gpu.lvds_output].edid_ok) return NULL;
    return gpu.outputs[gpu.lvds_output].edid;
}

static const struct display_flipper flipper = {
    .back_buffer = back_buffer,
    .flip_to_back = flip_to_back,
    .flip_to_console = flip_to_console,
    .console_in_front = console_in_front,
    .idle = idle,
    .vblank = vblank,
    .edid = edid,
    .cursor_set = cursor_set,
    .cursor_move = cursor_move,
    .gamma_set = gamma_set,
    .present_pages = present_pages,
    .forget_pages = forget_pages,
};

static int pci_probe(const struct pci_device *device) {
    if (device->bus != gpu_bus || device->slot != gpu_slot || device->function != gpu_function)
        return -1;
    pci_device = *device;
    pci_bound = 1;
    return 0;
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

static void vblank_wake(void *unused) {
    (void)unused;
    (void)process_wake_io();
    process_io_recheck();
}

static struct work vblank_work = WORK_INITIALIZER(vblank_wake, NULL);

// The supervisor's steps, answered in process context as the display raises
// them (a flip through the core channel raises all three), and whoever waits
// for the flip woken once the last is answered.
static uint32_t supervisor_steps;

static void supervise(void *unused) {
    (void)unused;
    uint32_t steps = __atomic_exchange_n(&supervisor_steps, 0, __ATOMIC_ACQ_REL);
    for (uint32_t step = 0x10U; step <= 0x40U; step <<= 1)
        if (steps & step) nv50_disp_supervise(&gpu, step);
    vblank_wake(NULL);
}

static struct work supervisor_work = WORK_INITIALIZER(supervise, NULL);

static void interrupt(void *context) {
    (void)context;
    // Always answered and re-armed: an MSI left unanswered is the last one.
    if (!__atomic_load_n(&vblank_live, __ATOMIC_ACQUIRE)) return;
    uint32_t steps = 0;
    int status = nv50_disp_interrupt(&gpu, &steps);
    if (status < 0) {
        nv50_disp_vblank_stop(&gpu);
        __atomic_store_n(&vblank_live, 0, __ATOMIC_RELEASE);
        kprintf("NV50: an unexpected interrupt, vblank goes back to the timer\n");
        return;
    }
    if (status & NV50_INTR_SUPERVISOR) {
        __atomic_or_fetch(&supervisor_steps, steps, __ATOMIC_ACQ_REL);
        work_queue(&supervisor_work);
    }
    if (!(status & NV50_INTR_VBLANK)) return;
    __atomic_store_n(&vblank_ns, time_uptime_ns(), __ATOMIC_RELEASE);
    __atomic_add_fetch(&vblank_count, 1, __ATOMIC_ACQ_REL);
    if (gpu.flip_pending || copy_fence) work_queue(&vblank_work);
}

static void start_vblank_interrupt(void) {
    if (!pci_bound) return;
    unsigned vector = irq_request("nv50", "PCI-MSI", interrupt, NULL);
    if (!vector) return;
    pci_enable_bus_mastering(&pci_device);
    if (pci_msi_bind(&pci_device, vector) != 0) {
        irq_release(vector);
        kprintf("NV50: no msi, vblank stays on the timer\n");
        return;
    }
    __atomic_store_n(&vblank_live, 1, __ATOMIC_RELEASE);
    nv50_disp_vblank_start(&gpu);
    // MSI is sent again only once rearmed: an interrupt already pending as it
    // was bound would otherwise be the last (nouveau rearms here too).
    nv50_disp_msi_rearm(&gpu);
    kprintf("NV50: vblank interrupt on vector %u\n", vector);
}

static uint32_t backlight_get(void) { return nv50_disp_backlight_get(&gpu, BACKLIGHT_SCALE); }

static int backlight_set(uint32_t level) {
    return nv50_disp_backlight_set(&gpu, level, BACKLIGHT_SCALE);
}

static const struct backlight_device backlight = {
    .name = "nv_backlight",
    .type = "raw",
    .max_brightness = BACKLIGHT_SCALE,
    .get = backlight_get,
    .set = backlight_set,
};

static void start_backlight(void) {
    if (!nv50_disp_backlight_present(&gpu)) {
        kprintf("NV50: the panel backlight is not on a gpu pwm\n");
        return;
    }
    sysfs_publish_backlight(&backlight, pci_bound ? &pci_device : NULL);
    kprintf("NV50: backlight at %u of %u\n", backlight_get(), BACKLIGHT_SCALE);
}

// The GT215 family's copy engine takes DRM's present copies; nv50fifo first
// runs the channel's and the engine's selftest.
static void start_acceleration(void) {
    uint32_t chipset = (nv50_rd32(&gpu, 0) >> 20) & 0x1FFU;
    if (chipset != 0xA3U && chipset != 0xA5U && chipset != 0xA8U) return;
    if (nv50_fifo_start(&gpu) != 0 || nv50_copy_start(&gpu) != 0) {
        kprintf("NV50: the copy engine did not start; DRM copies with the cpu\n");
        return;
    }
    int graphics = nv50_gr_start(&gpu) == 0;
    if (!graphics) kprintf("NV50: the graphics engine did not start\n");
    if (boot_command_line_flag("nv50fifo")) {
        nv50_fifo_selftest(&gpu);
        if (graphics) nv50_gr_selftest(&gpu);
    }
    accelerated = 1;
    kprintf("NV50: the copy engine takes DRM's present copies\n");
}

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
    // The driver's VRAM: what lies above the flip buffers, short of the
    // last MiB, where the firmware keeps its own.
    if (nv50_vram_init(&gpu.vram, VRAM_SURFACE + stride * (DRM_BUFFERS + 1U),
                       gpu.vram_bytes - 0x100000U) != 0)
        kprintf("NV50: no vram above the flip buffers\n");
    else
        start_acceleration();
    cursor_map = (volatile uint32_t *)vmm_map_device_write_combining(gpu.bar1 + VRAM_CURSOR,
                                                                     CURSOR_PIXELS * 4U);
    lut_map = (volatile uint16_t *)vmm_map_device_write_combining(gpu.bar1 + VRAM_LUT,
                                                                  LUT_ENTRIES * 8U);
    start_vblank_interrupt();
    start_backlight();
    nv50_probe_outputs(&gpu);
    display_register_flipper(&flipper);
    kprintf("NV50: page flipping between two buffers\n");
}

const struct display_early_driver nv50_display_driver = {
    .name = "nv50",
    .setup = nv50_setup,
    .late = nv50_late,
};
