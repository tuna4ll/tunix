/*
 * virtio-gpu.
 *
 * Every command is a request buffer the device reads and a response buffer it
 * writes, submitted on the control queue and waited out. The one command that
 * carries a payload is RESOURCE_ATTACH_BACKING, whose list of guest pages goes
 * in a descriptor of its own.
 *
 * The device is asked for VIRGL when it is attached. Where the host grants it
 * there is a second, much larger interface behind the same queue: contexts,
 * resources with a real format and target, and command buffers that are
 * OpenGL work for the host to do. What the host can do with them is not
 * guessed at -- it is read out of a capset, a blob virglrenderer fills in and
 * mesa parses to learn which GL version and extensions it may use.
 *
 * Where the host does not grant it, everything past the capset query is unused
 * and the display works exactly as it did. 2D is not a fallback bolted on
 * underneath; it is the same set of commands either way.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../include/heap.h"
#include "../../include/kstring.h"
#include "../../include/virtgpu.h"
#include "../../include/virtio.h"
#include "../../include/vmm.h"

extern void kprintf(const char *fmt, ...);

#define VIRTIO_GPU_DEVICE_ID 0x1050U
#define VIRTIO_GPU_CONTROL_QUEUE 0U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO 0x0100U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D 0x0101U
#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102U
#define VIRTIO_GPU_CMD_SET_SCANOUT 0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH 0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107U
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO 0x0108U
#define VIRTIO_GPU_CMD_GET_CAPSET 0x0109U

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101U
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO 0x1102U
#define VIRTIO_GPU_RESP_OK_CAPSET 0x1103U
/* Every ok response is 0x11xx and every error 0x12xx, so one comparison sorts
   them without having to name each. */
#define VIRTIO_GPU_RESP_ERR_BASE 0x1200U

/* Feature bit 0: the host will accept 3D commands, and has a capset that says
   what it can do with them. */
#define VIRTIO_GPU_F_VIRGL 0U

/* virglrenderer publishes two: the original, and the one every mesa since 2018
   actually asks for. Which exist is the host's answer, not ours. */
#define VIRTIO_GPU_CAPSET_VIRGL 1U
#define VIRTIO_GPU_CAPSET_VIRGL2 2U

/* struct virtio_gpu_config, whose fourth word is the number of capsets. */
#define VIRTIO_GPU_CONFIG_NUM_CAPSETS 12U

/* A virgl2 capset is a couple of kilobytes today. The device is asked how big
   its own is before it is fetched, so this is a ceiling on what can be
   accepted rather than a guess at the size. */
#define MAX_CAPSET_BYTES 4096U

/* XRGB8888 in memory is B, G, R, unused -- which is what this format names. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8 2U

#define VIRTIO_GPU_MAX_SCANOUTS 16U

/* 2048 pages is 8 MiB, one pixel more than a 1920x1080 scanout needs and the
   largest buffer the framebuffer layer will ever hand over. */
#define MAX_BACKING_PAGES 2048U

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
};

struct virtio_gpu_mem_entry {
    uint64_t address;
    uint32_t length;
    uint32_t padding;
};

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_index;
    uint32_t padding;
};

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
};

struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t capset_id;
    uint32_t capset_version;
};

/* The capset data follows the header with nothing between, so a single
   device-writable buffer describes the whole reply. */
struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8_t capset_data[MAX_CAPSET_BYTES];
};

/* Static, so the physical addresses handed to the device come straight out of
   the kernel window and are contiguous without an allocator that can promise
   it. Statically sized for the same reason. */
static union {
    struct virtio_gpu_resource_create_2d create;
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_attach_backing attach;
    struct virtio_gpu_set_scanout scanout;
    struct virtio_gpu_transfer_to_host_2d transfer;
    struct virtio_gpu_resource_flush flush;
    struct virtio_gpu_get_capset_info capset_info;
    struct virtio_gpu_get_capset capset;
    struct virtio_gpu_ctrl_hdr hdr;
} request;
/* Likewise one buffer for the reply, sized for the largest of them, which is a
   capset. Every reply begins with the same header, so the type can be checked
   before anything knows which shape arrived. */
static union {
    struct virtio_gpu_resp_display_info display;
    struct virtio_gpu_resp_capset_info capset_info;
    struct virtio_gpu_resp_capset capset;
    struct virtio_gpu_ctrl_hdr hdr;
} response;
static struct virtio_gpu_mem_entry backing[MAX_BACKING_PAGES];

static struct virtio_device device;
static struct virtio_queue control;
static uint32_t next_resource_id = 1;
static uint32_t scanout_resource;
static uint32_t display_width;
static uint32_t display_height;
static int ready;

/* Whether the host agreed to VIRGL, and the best capset it published. A zero
   id means there is no 3D to be had: either the device never offered the
   feature, or it offered it and then described no capset, which is a host
   built without virglrenderer. */
static int virgl;
static uint32_t capset_id;
static uint32_t capset_version;
static uint32_t capset_size;

static int submit(uint32_t request_bytes, const void *payload, uint32_t payload_bytes,
                  uint32_t response_bytes) {
    struct virtio_buffer buffers[3];
    unsigned count = 0;

    buffers[count].physical = vmm_virt_to_phys_direct(&request);
    buffers[count].length = request_bytes;
    count++;
    if (payload && payload_bytes) {
        buffers[count].physical = vmm_virt_to_phys_direct(payload);
        buffers[count].length = payload_bytes;
        count++;
    }
    unsigned write_from = count;
    buffers[count].physical = vmm_virt_to_phys_direct(&response);
    buffers[count].length = response_bytes;
    count++;

    memset(&response, 0, response_bytes);
    if (virtio_queue_submit(&control, buffers, count, write_from) != 0) return -1;
    if (response.hdr.type >= VIRTIO_GPU_RESP_ERR_BASE) return -1;
    return 0;
}

static void begin(uint32_t type) {
    memset(&request, 0, sizeof(request));
    request.hdr.type = type;
}

/*
 * Round-trip the queue and read back the size the host says the display is.
 *
 * That size is not the mode: the host answers with its own window once
 * something has told it how big that is, so it moves when a window is dragged
 * and disagrees with the mode the bootloader set. Nothing here scans out at it
 * -- the scanout rect is the framebuffer's -- so this is a probe that the
 * device answers at all, and the size is kept only to be asked for.
 */
static int query_display_info(void) {
    begin(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    if (submit(sizeof(request.hdr), NULL, 0, sizeof(response)) != 0) return -1;
    for (unsigned index = 0; index < VIRTIO_GPU_MAX_SCANOUTS; index++) {
        if (!response.display.pmodes[index].enabled) continue;
        display_width = response.display.pmodes[index].r.width;
        display_height = response.display.pmodes[index].r.height;
        return 0;
    }
    return -1;
}

/*
 * Ask the host what its 3D can do.
 *
 * The device says how many capsets it has in its configuration space and
 * describes them one at a time by index; the id is what a capset turns out to
 * be, not something to ask for. virgl2 is preferred wherever it appears
 * because it is what mesa asks for, and having only the original means a host
 * too old for anything current.
 *
 * The contents are not read here. They are a blob mesa parses and the kernel
 * only has to hand over intact, so this records which one to fetch and how big
 * it is; fetching waits until something asks.
 */
static void query_capsets(void) {
    uint32_t count = virtio_config_read32(&device, VIRTIO_GPU_CONFIG_NUM_CAPSETS);
    for (uint32_t index = 0; index < count; index++) {
        begin(VIRTIO_GPU_CMD_GET_CAPSET_INFO);
        request.capset_info.capset_index = index;
        if (submit(sizeof(request.capset_info), NULL, 0,
                   sizeof(response.capset_info)) != 0) continue;

        uint32_t id = response.capset_info.capset_id;
        if (id != VIRTIO_GPU_CAPSET_VIRGL && id != VIRTIO_GPU_CAPSET_VIRGL2) continue;
        /* One that will not fit in the reply buffer cannot be handed over, and
           a capset that arrives truncated is worse than one that never
           arrives: mesa would read capabilities out of uninitialised bytes. */
        if (response.capset_info.capset_max_size > MAX_CAPSET_BYTES) continue;
        if (capset_id == VIRTIO_GPU_CAPSET_VIRGL2 && id == VIRTIO_GPU_CAPSET_VIRGL)
            continue;

        capset_id = id;
        capset_version = response.capset_info.capset_max_version;
        capset_size = response.capset_info.capset_max_size;
    }
}

int virtgpu_virgl_available(void) { return virgl && capset_id != 0; }
uint32_t virtgpu_capset_id(void) { return capset_id; }
uint32_t virtgpu_capset_version(void) { return capset_version; }
uint32_t virtgpu_capset_size(void) { return capset_size; }

int virtgpu_get_capset(uint32_t id, uint32_t version, void *out, uint32_t bytes) {
    if (!virtgpu_virgl_available() || !out || !bytes) return -1;
    if (bytes > MAX_CAPSET_BYTES) return -1;

    begin(VIRTIO_GPU_CMD_GET_CAPSET);
    request.capset.capset_id = id;
    request.capset.capset_version = version;
    /* The device is told how much room the reply has, header included, and
       fills what fits. Asking for less than the whole capset is how mesa reads
       the prefix it understands of a newer one than it knows. */
    if (submit(sizeof(request.capset), NULL, 0,
               (uint32_t)sizeof(struct virtio_gpu_ctrl_hdr) + bytes) != 0) return -1;
    if (response.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET) return -1;
    memcpy(out, response.capset.capset_data, bytes);
    return 0;
}

int virtgpu_init(void) {
    /* Asking for a feature the device does not offer is not an error -- what
       is negotiated is the intersection -- so this is simply how the question
       gets asked. */
    uint64_t granted = 0;
    if (virtio_pci_attach(&device, VIRTIO_GPU_DEVICE_ID,
                          1ULL << VIRTIO_GPU_F_VIRGL, &granted) != 0) return -1;
    virgl = (granted & (1ULL << VIRTIO_GPU_F_VIRGL)) != 0;
    if (virtio_pci_setup_queue(&device, &control, VIRTIO_GPU_CONTROL_QUEUE) != 0) {
        virtio_pci_set_failed(&device);
        return -1;
    }
    virtio_pci_set_driver_ok(&device);

    if (query_display_info() != 0) {
        virtio_pci_set_failed(&device);
        return -1;
    }
    ready = 1;
    if (virgl) query_capsets();
    if (virtgpu_virgl_available())
        kprintf("TUNIX: virtio-gpu ready, virgl capset %u version %u, %u bytes\n",
                capset_id, capset_version, capset_size);
    else
        kprintf("TUNIX: virtio-gpu ready, 2D only\n");
    return 0;
}

int virtgpu_available(void) { return ready; }
uint32_t virtgpu_display_width(void) { return display_width; }
uint32_t virtgpu_display_height(void) { return display_height; }

uint32_t virtgpu_resource_create(uint32_t width, uint32_t height,
                                 const uint64_t *pages, uint64_t page_count) {
    if (!ready || !width || !height || !pages) return 0;
    if (!page_count || page_count > MAX_BACKING_PAGES) return 0;

    uint32_t resource = next_resource_id;
    begin(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    request.create.resource_id = resource;
    request.create.format = VIRTIO_GPU_FORMAT_B8G8R8X8;
    request.create.width = width;
    request.create.height = height;
    if (submit(sizeof(request.create), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
        return 0;

    for (uint64_t index = 0; index < page_count; index++) {
        backing[index].address = pages[index];
        backing[index].length = 4096;
        backing[index].padding = 0;
    }
    begin(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    request.attach.resource_id = resource;
    request.attach.nr_entries = (uint32_t)page_count;
    if (submit(sizeof(request.attach), backing,
               (uint32_t)(page_count * sizeof(backing[0])),
               sizeof(struct virtio_gpu_ctrl_hdr)) != 0) {
        virtgpu_resource_destroy(resource);
        return 0;
    }

    next_resource_id++;
    return resource;
}

void virtgpu_resource_destroy(uint32_t resource) {
    if (!ready || !resource) return;
    if (scanout_resource == resource) virtgpu_scanout_disable();
    begin(VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    request.unref.resource_id = resource;
    (void)submit(sizeof(request.unref), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    begin(VIRTIO_GPU_CMD_RESOURCE_UNREF);
    request.unref.resource_id = resource;
    (void)submit(sizeof(request.unref), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

static void set_rect(struct virtio_gpu_rect *r, uint32_t width, uint32_t height) {
    r->x = 0;
    r->y = 0;
    r->width = width;
    r->height = height;
}

int virtgpu_present(uint32_t resource, uint32_t width, uint32_t height) {
    if (!ready || !resource || !width || !height) return -1;

    if (scanout_resource != resource) {
        begin(VIRTIO_GPU_CMD_SET_SCANOUT);
        set_rect(&request.scanout.r, width, height);
        request.scanout.resource_id = resource;
        if (submit(sizeof(request.scanout), NULL, 0,
                   sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return -1;
        scanout_resource = resource;
    }

    begin(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    set_rect(&request.transfer.r, width, height);
    request.transfer.resource_id = resource;
    if (submit(sizeof(request.transfer), NULL, 0,
               sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return -1;

    begin(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    set_rect(&request.flush.r, width, height);
    request.flush.resource_id = resource;
    return submit(sizeof(request.flush), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

/* Created once, over the framebuffer the bootloader set up, and kept: the
   pages never move and the resource costs nothing while it is not scanned out. */
static uint32_t console_resource;

int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels,
                            uint32_t width, uint32_t height) {
    if (!ready || !stride_pixels || !width || !height) return -1;
    if (!console_resource) {
        uint64_t bytes = (uint64_t)stride_pixels * 4U * height;
        uint64_t page_count = (bytes + 4095U) / 4096U;
        uint64_t *pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
        if (!pages) return -1;
        for (uint64_t index = 0; index < page_count; index++)
            pages[index] = physical + index * 4096U;
        console_resource = virtgpu_resource_create(stride_pixels, height,
                                                   pages, page_count);
        kfree(pages);
        if (!console_resource) return -1;
    }
    return virtgpu_present(console_resource, width, height);
}

void virtgpu_scanout_disable(void) {
    if (!ready || !scanout_resource) return;
    begin(VIRTIO_GPU_CMD_SET_SCANOUT);
    set_rect(&request.scanout.r, display_width, display_height);
    request.scanout.resource_id = 0;
    (void)submit(sizeof(request.scanout), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    scanout_resource = 0;
}
