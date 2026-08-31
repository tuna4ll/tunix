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
#include "../../include/dma.h"
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

/* The 3D half. Every one of these is refused outright by a host that did not
   grant VIRGL, so nothing below is reachable without it. */
#define VIRTIO_GPU_CMD_CTX_CREATE 0x0200U
#define VIRTIO_GPU_CMD_CTX_DESTROY 0x0201U
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202U
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D 0x0204U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D 0x0205U
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206U
#define VIRTIO_GPU_CMD_SUBMIT_3D 0x0207U

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

/*
 * Command buffers are staged through a buffer of our own rather than handed to
 * the device where they lie.
 *
 * The device is given physical addresses, and the heap only promises virtually
 * contiguous memory -- a buffer that spans a page boundary can be anywhere in
 * physical memory on the other side of it. A static buffer is in the kernel
 * window, where contiguous means contiguous.
 *
 * A megabyte, because mesa batches texture uploads into the command stream and
 * a track loading in SuperTuxKart was measured at 266224 bytes. Keep this and
 * DRM_MAX_COMMAND_BYTES the same: the one is copied into the other.
 */
#define MAX_COMMAND_BYTES (1024U * 1024U)

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

struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
};

struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
};

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtgpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
};

struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;
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
    struct virtio_gpu_ctx_create ctx_create;
    struct virtio_gpu_ctx_resource ctx_resource;
    struct virtio_gpu_resource_create_3d create_3d;
    struct virtio_gpu_transfer_host_3d transfer_3d;
    struct virtio_gpu_cmd_submit submit_3d;
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
/*
 * The two buffers the device is pointed at, rather than two static arrays.
 *
 * Between them they are a megabyte and a half of the kernel image, reserved on
 * every machine including the ones with no virtio-gpu in them at all. They have
 * to be contiguous, which is the only reason they were static; dma_alloc()
 * makes that available at run time, so now they cost nothing until the device
 * turns out to be there.
 */
static struct virtio_gpu_mem_entry *backing;
static uint8_t *commands;

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
 * Tell the host which guest pages are a resource's storage.
 *
 * A resource is created empty: it exists on the host as a description with no
 * bytes behind it, and this is what says where the bytes are. The pages stay
 * the guest's -- the host reads them when a transfer says to, and nothing here
 * copies anything.
 *
 * `bytes` is the resource's real size, which is not the size of the pages
 * holding it: a 250x250 texture is 250000 bytes and lives in 62 pages of
 * 253952. Describing all of that as backing makes the host refuse every
 * transfer -- "IOV data size exceeds resource capacity" -- because it is being
 * handed more storage than the resource it belongs to can hold. So the last
 * page is described by the part of it that is actually the resource.
 */
static int attach_backing(uint32_t resource, const uint64_t *pages,
                          uint64_t page_count, uint64_t bytes) {
    if (!page_count || page_count > MAX_BACKING_PAGES) return -1;
    if (!bytes || bytes > page_count * 4096ULL) bytes = page_count * 4096ULL;
    uint64_t remaining = bytes;
    for (uint64_t index = 0; index < page_count; index++) {
        backing[index].address = pages[index];
        backing[index].length = remaining > 4096ULL ? 4096U : (uint32_t)remaining;
        backing[index].padding = 0;
        remaining -= backing[index].length;
    }
    begin(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    request.attach.resource_id = resource;
    request.attach.nr_entries = (uint32_t)page_count;
    return submit(sizeof(request.attach), backing,
                  (uint32_t)(page_count * sizeof(backing[0])),
                  sizeof(struct virtio_gpu_ctrl_hdr));
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

int virtgpu_pci_identity(struct virtgpu_pci_identity *out) {
    if (!ready || !out) return -1;
    out->bus = device.pci.bus;
    out->slot = device.pci.slot;
    out->function = device.pci.function;
    out->vendor = device.pci.vendor_id;
    out->device = device.pci.device_id;
    return 0;
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

/* --- 3D -------------------------------------------------------------------
 *
 * Everything below speaks for a context, which is one client's view of the
 * host's renderer: its own resources, its own GL state. mesa makes one per
 * screen and puts every command through it, so contexts live as long as the
 * process does and are not something to be economical with.
 *
 * Every command carries its context in the header, so there is no current
 * context to get wrong -- each one says whose it is.
 */

int virtgpu_context_create(uint32_t context, const char *name) {
    if (!virtgpu_virgl_available() || !context) return -1;

    begin(VIRTIO_GPU_CMD_CTX_CREATE);
    request.ctx_create.hdr.ctx_id = context;
    /* The name is for the host's own log when something goes wrong inside this
       context, and is the only thing that tells two of them apart there. */
    uint32_t length = 0;
    if (name) {
        while (name[length] &&
               length < (uint32_t)sizeof(request.ctx_create.debug_name) - 1U) {
            request.ctx_create.debug_name[length] = name[length];
            length++;
        }
    }
    request.ctx_create.nlen = length;
    return submit(sizeof(request.ctx_create), NULL, 0,
                  sizeof(struct virtio_gpu_ctrl_hdr));
}

void virtgpu_context_destroy(uint32_t context) {
    if (!virtgpu_virgl_available() || !context) return;
    begin(VIRTIO_GPU_CMD_CTX_DESTROY);
    request.hdr.ctx_id = context;
    (void)submit(sizeof(request.hdr), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

/*
 * A resource is created outside any context and then attached to the ones
 * allowed to name it. A command buffer that refers to a resource its context
 * was never given is how virglrenderer gets asked to touch something it should
 * not, and it refuses -- so this is a permission, not a formality.
 */
int virtgpu_context_attach(uint32_t context, uint32_t resource, int attach) {
    if (!virtgpu_virgl_available() || !context || !resource) return -1;
    begin(attach ? VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE
                 : VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);
    request.ctx_resource.hdr.ctx_id = context;
    request.ctx_resource.resource_id = resource;
    return submit(sizeof(request.ctx_resource), NULL, 0,
                  sizeof(struct virtio_gpu_ctrl_hdr));
}

/*
 * A 3D resource: a texture, a vertex buffer, a render target.
 *
 * Unlike the 2D kind it has no fixed shape -- the target says whether it is a
 * buffer or an image or an array of them, the bind flags say what it may be
 * used as, and the host allocates to suit. Those values are mesa's and are
 * passed through untouched; nothing here interprets a format.
 *
 * Backing is optional in a way it is not for 2D. A resource the guest never
 * reads or writes -- a depth buffer, a render target -- lives only on the host
 * and wants no guest pages behind it at all.
 */
uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec,
                                    const uint64_t *pages, uint64_t page_count,
                                    uint64_t bytes) {
    if (!virtgpu_virgl_available() || !spec) return 0;

    uint32_t resource = next_resource_id;
    begin(VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    request.create_3d.resource_id = resource;
    request.create_3d.target = spec->target;
    request.create_3d.format = spec->format;
    request.create_3d.bind = spec->bind;
    request.create_3d.width = spec->width;
    request.create_3d.height = spec->height;
    request.create_3d.depth = spec->depth;
    request.create_3d.array_size = spec->array_size;
    request.create_3d.last_level = spec->last_level;
    request.create_3d.nr_samples = spec->nr_samples;
    request.create_3d.flags = spec->flags;
    if (submit(sizeof(request.create_3d), NULL, 0,
               sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return 0;

    if (pages && page_count &&
        attach_backing(resource, pages, page_count, bytes) != 0) {
        virtgpu_resource_destroy(resource);
        return 0;
    }

    next_resource_id++;
    return resource;
}

/*
 * Move part of a resource between the guest pages and the host's copy.
 *
 * Both directions happen: a texture is uploaded, and a buffer the shader wrote
 * is read back. The box is in the resource's own units, which for a buffer
 * means x and w are bytes rather than pixels.
 */
int virtgpu_transfer_3d(uint32_t context, uint32_t resource,
                        const struct virtgpu_box *box, uint64_t offset,
                        uint32_t level, uint32_t stride, uint32_t layer_stride,
                        int to_host) {
    if (!virtgpu_virgl_available() || !resource || !box) return -1;

    begin(to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
                  : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    request.transfer_3d.hdr.ctx_id = context;
    request.transfer_3d.box = *box;
    request.transfer_3d.offset = offset;
    request.transfer_3d.resource_id = resource;
    request.transfer_3d.level = level;
    request.transfer_3d.stride = stride;
    request.transfer_3d.layer_stride = layer_stride;
    return submit(sizeof(request.transfer_3d), NULL, 0,
                  sizeof(struct virtio_gpu_ctrl_hdr));
}

/*
 * Hand the host a command buffer to execute. This is where the rendering
 * actually happens.
 *
 * The buffer is virglrenderer's own encoding of GL work, built by mesa; the
 * kernel does not read a word of it beyond checking that it will fit.
 * Submitting is synchronous because the queue is -- by the time the device
 * hands the descriptor back the host has done the work. That is what makes a
 * fence unnecessary here, and a frame slower than it has to be.
 */
int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes) {
    if (!virtgpu_virgl_available() || !context || !buffer) return -1;
    if (!bytes || bytes > MAX_COMMAND_BYTES) return -1;
    /* The encoding is a stream of 32-bit words. A length that is not a whole
       number of them would leave the host reading past the last one. */
    if (bytes % 4U) return -1;

    memcpy(commands, buffer, bytes);
    begin(VIRTIO_GPU_CMD_SUBMIT_3D);
    request.submit_3d.hdr.ctx_id = context;
    request.submit_3d.size = bytes;
    return submit(sizeof(request.submit_3d), commands, bytes,
                  sizeof(struct virtio_gpu_ctrl_hdr));
}

/*
 * What the device raises when it has put something on the used ring.
 *
 * It does not do the waiting -- submit() still watches the ring, because that
 * is where the answer is -- so all this does today is count. That is on
 * purpose: an interrupt arriving at all is the thing being proven here, and a
 * handler that also did the work would make a delivery failure look like a
 * hang rather than a number that stays at zero.
 *
 * It runs inside whatever the interrupted processor was doing, including a
 * submit() that is holding the kernel lock, so it must not touch anything
 * submit() is in the middle of.
 */
static uint64_t completions;

static void control_queue_interrupt(void *context) {
    (void)context;
    completions++;
}

uint64_t virtgpu_interrupt_count(void) { return completions; }

int virtgpu_init(void) {
    /* Asking for a feature the device does not offer is not an error -- what
       is negotiated is the intersection -- so this is simply how the question
       gets asked. */
    uint64_t granted = 0;
    if (virtio_pci_attach(&device, VIRTIO_GPU_DEVICE_ID,
                          1ULL << VIRTIO_GPU_F_VIRGL, &granted) != 0) return -1;

    /* After the device is known to be there, and before anything is submitted
       through them. */
    /* The physical addresses are asked for and then dropped: submit() derives
       them from the pointer, the same way it does for every other buffer it is
       handed, and one path for that is better than two. */
    uint64_t discarded = 0;
    backing = (struct virtio_gpu_mem_entry *)dma_alloc(
        sizeof(*backing) * MAX_BACKING_PAGES, 0, &discarded);
    commands = (uint8_t *)dma_alloc(MAX_COMMAND_BYTES, 0, &discarded);
    if (!backing || !commands) {
        if (backing) dma_free(backing, sizeof(*backing) * MAX_BACKING_PAGES);
        if (commands) dma_free(commands, MAX_COMMAND_BYTES);
        backing = NULL;
        commands = NULL;
        virtio_pci_set_failed(&device);
        return -1;
    }
    virgl = (granted & (1ULL << VIRTIO_GPU_F_VIRGL)) != 0;
    /* Before the queue, because a queue is pointed at the device's vector as it
       is set up and there is no second chance afterwards. A device that cannot
       give one is not a failure: this driver polled from the day it was written
       and still does. */
    virtio_pci_request_irq(&device, "virtio-gpu", control_queue_interrupt, NULL);
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
    if (device.vector)
        kprintf("TUNIX: virtio-gpu interrupts on vector %u\n", device.vector);
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

    /* A 2D resource is exactly its pixels, and the caller sized the pages to
       hold them. */
    if (attach_backing(resource, pages, page_count,
                       (uint64_t)width * 4ULL * height) != 0) {
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

int virtgpu_present(uint32_t resource, uint32_t width, uint32_t height,
                    int upload) {
    if (!ready || !resource || !width || !height) return -1;

    if (scanout_resource != resource) {
        begin(VIRTIO_GPU_CMD_SET_SCANOUT);
        set_rect(&request.scanout.r, width, height);
        request.scanout.resource_id = resource;
        if (submit(sizeof(request.scanout), NULL, 0,
                   sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return -1;
        scanout_resource = resource;
    }

    /*
     * Whether the guest pages are the picture, or stale.
     *
     * A resource the CPU drew into has to be sent to the host before it can be
     * shown. One the host itself rendered into is already right, and copying
     * the guest's pages over it would replace the frame with whatever those
     * pages last held -- which is nothing, so the screen would go black.
     */
    if (upload) {
        begin(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        set_rect(&request.transfer.r, width, height);
        request.transfer.resource_id = resource;
        if (submit(sizeof(request.transfer), NULL, 0,
                   sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return -1;
    }

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
    return virtgpu_present(console_resource, width, height, 1);
}

void virtgpu_scanout_disable(void) {
    if (!ready || !scanout_resource) return;
    begin(VIRTIO_GPU_CMD_SET_SCANOUT);
    set_rect(&request.scanout.r, display_width, display_height);
    request.scanout.resource_id = 0;
    (void)submit(sizeof(request.scanout), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    scanout_resource = 0;
}
