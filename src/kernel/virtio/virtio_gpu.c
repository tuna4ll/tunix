/*
 * virtio-gpu, 2D mode.
 *
 * Every command is a request buffer the device reads and a response buffer it
 * writes, submitted on the control queue and waited out. The one command that
 * carries a payload is RESOURCE_ATTACH_BACKING, whose list of guest pages goes
 * in a descriptor of its own.
 */
#include <stddef.h>
#include <stdint.h>

#include "../include/kstring.h"
#include "../include/virtgpu.h"
#include "../include/virtio.h"
#include "../include/vmm.h"

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

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101U

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
    struct virtio_gpu_ctrl_hdr hdr;
} request;
static struct virtio_gpu_resp_display_info response;
static struct virtio_gpu_mem_entry backing[MAX_BACKING_PAGES];

static struct virtio_device device;
static struct virtio_queue control;
static uint32_t next_resource_id = 1;
static uint32_t scanout_resource;
static uint32_t display_width;
static uint32_t display_height;
static int ready;

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
    if (response.hdr.type != VIRTIO_GPU_RESP_OK_NODATA &&
        response.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) return -1;
    return 0;
}

static void begin(uint32_t type) {
    memset(&request, 0, sizeof(request));
    request.hdr.type = type;
}

static int query_display_info(void) {
    begin(VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    if (submit(sizeof(request.hdr), NULL, 0, sizeof(response)) != 0) return -1;
    for (unsigned index = 0; index < VIRTIO_GPU_MAX_SCANOUTS; index++) {
        if (!response.pmodes[index].enabled) continue;
        display_width = response.pmodes[index].r.width;
        display_height = response.pmodes[index].r.height;
        return 0;
    }
    return -1;
}

int virtgpu_init(void) {
    if (virtio_pci_attach(&device, VIRTIO_GPU_DEVICE_ID, 0, NULL) != 0) return -1;
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
    kprintf("TUNIX: virtio-gpu ready, display %ux%u\n", display_width, display_height);
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

void virtgpu_scanout_disable(void) {
    if (!ready || !scanout_resource) return;
    begin(VIRTIO_GPU_CMD_SET_SCANOUT);
    set_rect(&request.scanout.r, display_width, display_height);
    request.scanout.resource_id = 0;
    (void)submit(sizeof(request.scanout), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    scanout_resource = 0;
}
