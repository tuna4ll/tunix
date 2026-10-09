#include <stddef.h>
#include <stdint.h>

#include <tunix/heap.h>
#include <tunix/cpu.h>
#include <tunix/dma.h>
#include <tunix/kstring.h>
#include <tunix/time.h>
#include <tunix/virtgpu.h>
#include <tunix/virtio.h>
#include <tunix/vmm.h>
#include <tunix/lock.h>
#include <tunix/percpu.h>
#include <tunix/process.h>
#include <tunix/workqueue.h>

static struct lock virtgpu_lock = LOCK_INITIALIZER("virtio-gpu", LOCK_RANK_DEVICE);

static void virtgpu_guard_release(int *unused) {
    (void)unused;
    lock_release(&virtgpu_lock);
}

#define VIRTGPU_LOCKED \
    __attribute__((cleanup(virtgpu_guard_release))) int virtgpu_guard = \
        (lock_acquire(&virtgpu_lock), 0)

extern void kprintf(const char *fmt, ...);

#define VIRTIO_GPU_DEVICE_ID         0x1050U
#define VIRTIO_GPU_CONTROL_QUEUE     0U
#define VIRTIO_GPU_CURSOR_QUEUE      1U
#define VIRTIO_GPU_CMD_UPDATE_CURSOR 0x0300U
#define VIRTIO_GPU_CMD_MOVE_CURSOR   0x0301U
#define CURSOR_SLOTS                 16U
#define CURSOR_SLOT_BYTES            64U

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO        0x0100U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      0x0101U
#define VIRTIO_GPU_CMD_RESOURCE_UNREF          0x0102U
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103U
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105U
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106U
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107U
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108U
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109U

#define VIRTIO_GPU_CMD_CTX_CREATE            0x0200U
#define VIRTIO_GPU_CMD_CTX_DESTROY           0x0201U
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE   0x0202U
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE   0x0203U
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D    0x0204U
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D   0x0205U
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206U
#define VIRTIO_GPU_CMD_SUBMIT_3D             0x0207U

#define VIRTIO_GPU_RESP_OK_NODATA       0x1100U
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO 0x1101U
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO  0x1102U
#define VIRTIO_GPU_RESP_OK_CAPSET       0x1103U

#define VIRTIO_GPU_RESP_ERR_BASE 0x1200U

#define VIRTIO_GPU_F_VIRGL 0U

#define VIRTIO_GPU_CAPSET_VIRGL  1U
#define VIRTIO_GPU_CAPSET_VIRGL2 2U

#define VIRTIO_GPU_CONFIG_NUM_CAPSETS 12U

#define MAX_CAPSET_BYTES 4096U

#define MAX_COMMAND_BYTES (1024U * 1024U)

#define VIRTIO_GPU_FORMAT_B8G8R8X8 2U

#define VIRTIO_GPU_MAX_SCANOUTS 16U

#define BACKING_ENTRIES 4096U

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

struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8_t capset_data[MAX_CAPSET_BYTES];
};

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

static union {
    struct virtio_gpu_resp_display_info display;
    struct virtio_gpu_resp_capset_info capset_info;
    struct virtio_gpu_resp_capset capset;
    struct virtio_gpu_ctrl_hdr hdr;
} response;

static struct virtio_gpu_mem_entry *backing;
static uint8_t *commands;

static struct virtio_device device;
static struct virtio_queue control;
static struct virtio_queue cursorq;
static int cursor_ready;
static uint8_t *cursor_arena;
static uint64_t cursor_arena_physical;
static uint64_t cursor_sequence[CURSOR_SLOTS];

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout_id;
    int32_t x;
    int32_t y;
    uint32_t pos_padding;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
};
static uint32_t next_resource_id = 1;
static uint32_t scanout_resource;
static uint32_t display_width;
static uint32_t display_height;
static int ready;

static int virgl;
static uint32_t capset_id;
static uint32_t capset_version;
static uint32_t capset_size;

#define ASYNC_SLOTS          32U
#define ASYNC_REQUEST_BYTES  128U
#define ASYNC_PAYLOAD_BYTES  (32U * 1024U)
#define ASYNC_RESPONSE_BYTES 64U
#define ASYNC_SLOT_BYTES     (ASYNC_REQUEST_BYTES + ASYNC_PAYLOAD_BYTES + ASYNC_RESPONSE_BYTES)

struct async_slot {
    uint8_t *base;
    uint64_t physical;

    uint64_t sequence;
};

static struct async_slot async_slots[ASYNC_SLOTS];
static uint8_t *async_arena;
static uint64_t async_arena_physical;
static unsigned async_errors_reported;

static int async_ready(void) { return async_arena != NULL; }

static struct async_slot *async_take_slot(void) {
    virtio_queue_reclaim(&control);
    for (unsigned index = 0; index < ASYNC_SLOTS; index++) {
        struct async_slot *slot = &async_slots[index];
        if (slot->sequence > control.completed) continue;
        if (slot->sequence) {
            const struct virtio_gpu_ctrl_hdr *answer =
                (const struct virtio_gpu_ctrl_hdr *)(slot->base + ASYNC_REQUEST_BYTES +
                                                     ASYNC_PAYLOAD_BYTES);
            if (answer->type >= VIRTIO_GPU_RESP_ERR_BASE && async_errors_reported < 4U) {
                async_errors_reported++;
                kprintf("virtio-gpu: the host refused a posted command (%x)\n",
                        (unsigned)answer->type);
            }
        }
        return slot;
    }
    return NULL;
}

static int submit_async(uint32_t request_bytes, const void *payload, uint32_t payload_bytes) {
    if (!async_ready() || request_bytes > ASYNC_REQUEST_BYTES) return -1;
    if (payload_bytes > ASYNC_PAYLOAD_BYTES) return -1;

    struct async_slot *slot = async_take_slot();
    if (!slot) return -1;

    memcpy(slot->base, &request, request_bytes);
    if (payload && payload_bytes) memcpy(slot->base + ASYNC_REQUEST_BYTES, payload, payload_bytes);
    memset(slot->base + ASYNC_REQUEST_BYTES + ASYNC_PAYLOAD_BYTES, 0, ASYNC_RESPONSE_BYTES);

    struct virtio_buffer buffers[3];
    unsigned count = 0;
    buffers[count].physical = slot->physical;
    buffers[count].length = request_bytes;
    count++;
    if (payload && payload_bytes) {
        buffers[count].physical = slot->physical + ASYNC_REQUEST_BYTES;
        buffers[count].length = payload_bytes;
        count++;
    }
    unsigned write_from = count;
    buffers[count].physical = slot->physical + ASYNC_REQUEST_BYTES + ASYNC_PAYLOAD_BYTES;
    buffers[count].length = ASYNC_RESPONSE_BYTES;
    count++;

    if (virtio_queue_post(&control, buffers, count, write_from) != 0) return -1;
    slot->sequence = control.posted;
    return 0;
}

#define COMPLETION_POLL_NS    2000000ULL
#define COMPLETION_TIMEOUT_NS 2000000000ULL

static const char completion_channel;

static int may_sleep_here(void) { return process_may_sleep(); }

static uint64_t completed_seen;

static void reclaim_control(void) {
    virtio_queue_reclaim(&control);
    __atomic_store_n(&completed_seen, control.completed, __ATOMIC_RELEASE);
}

static int completed_through(uint64_t target) {
    VIRTGPU_LOCKED;
    reclaim_control();
    return control.completed >= target;
}

static int wait_completed(uint64_t target) {
    uint64_t deadline = time_uptime_ns() + COMPLETION_TIMEOUT_NS;
    while (!completed_through(target)) {
        if (time_uptime_ns() > deadline) return -1;
        if (!may_sleep_here()) {
            cpu_relax();
            continue;
        }
        process_prepare_wait(&completion_channel, time_uptime_ns() + COMPLETION_POLL_NS);
        if (completed_through(target)) {
            process_finish_wait();
            break;
        }
        process_wait();
        process_finish_wait();
    }
    return 0;
}

static void wait_for_room(unsigned needed) {
    if (!may_sleep_here()) return;
    uint64_t target = 0;
    {
        VIRTGPU_LOCKED;
        if (!ready || !async_ready()) return;
        virtio_queue_reclaim(&control);
        unsigned busy = 0;
        uint64_t sequences[ASYNC_SLOTS];
        for (unsigned index = 0; index < ASYNC_SLOTS; index++)
            if (async_slots[index].sequence > control.completed)
                sequences[busy++] = async_slots[index].sequence;
        if (ASYNC_SLOTS - busy >= needed) return;
        unsigned release = needed - (ASYNC_SLOTS - busy);
        for (unsigned round = 0; round < release; round++) {
            unsigned lowest = round;
            for (unsigned index = round + 1; index < busy; index++)
                if (sequences[index] < sequences[lowest]) lowest = index;
            uint64_t swap = sequences[round];
            sequences[round] = sequences[lowest];
            sequences[lowest] = swap;
        }
        target = sequences[release - 1];
    }
    (void)wait_completed(target);
}

static void begin(uint32_t type);
static void resource_destroy_now(uint32_t resource);
static void set_rect(struct virtio_gpu_rect *r, uint32_t width, uint32_t height);
static int submit(uint32_t request_bytes, const void *payload, uint32_t payload_bytes,
                  uint32_t response_bytes);

static int cursor_post(uint32_t type, uint32_t resource, int32_t x, int32_t y, uint32_t hot_x,
                       uint32_t hot_y) {
    if (!cursor_ready) return -1;
    virtio_queue_reclaim(&cursorq);
    for (unsigned index = 0; index < CURSOR_SLOTS; index++) {
        if (cursor_sequence[index] > cursorq.completed) continue;
        struct virtio_gpu_update_cursor *command =
            (struct virtio_gpu_update_cursor *)(cursor_arena + (uint64_t)index * CURSOR_SLOT_BYTES);
        memset(command, 0, sizeof(*command));
        command->hdr.type = type;
        command->x = x;
        command->y = y;
        command->resource_id = resource;
        command->hot_x = hot_x;
        command->hot_y = hot_y;
        struct virtio_buffer buffer;
        buffer.physical = cursor_arena_physical + (uint64_t)index * CURSOR_SLOT_BYTES;
        buffer.length = sizeof(*command);
        if (virtio_queue_post(&cursorq, &buffer, 1, 1) != 0) return -1;
        cursor_sequence[index] = cursorq.posted;
        return 0;
    }
    return -1;
}

int virtgpu_cursor_available(void) {
    VIRTGPU_LOCKED;
    return ready && cursor_ready;
}

int virtgpu_cursor_set(uint32_t resource, uint32_t stride_pixels, int upload, int32_t x, int32_t y,
                       uint32_t hot_x, uint32_t hot_y) {
    uint64_t target = 0;
    if (resource && upload) {
        wait_for_room(1);
        VIRTGPU_LOCKED;
        if (!ready) return -1;
        begin(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        set_rect(&request.transfer.r, stride_pixels < 64U ? stride_pixels : 64U, 64U);
        request.transfer.resource_id = resource;
        if (submit_async(sizeof(request.transfer), NULL, 0) != 0 &&
            submit(sizeof(request.transfer), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
            return -1;
        target = control.posted;
    }
    if (target) (void)wait_completed(target);
    VIRTGPU_LOCKED;
    return cursor_post(VIRTIO_GPU_CMD_UPDATE_CURSOR, resource, x, y, hot_x, hot_y);
}

int virtgpu_cursor_move(int32_t x, int32_t y) {
    VIRTGPU_LOCKED;
    return cursor_post(VIRTIO_GPU_CMD_MOVE_CURSOR, 0, x, y, 0, 0);
}

int virtgpu_flush_pending(void) {
    uint64_t target;
    {
        VIRTGPU_LOCKED;
        if (!ready) return 0;
        target = control.posted;
    }
    return wait_completed(target);
}

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

static int attach_backing(uint32_t resource, const uint64_t *pages, uint64_t page_count,
                          uint64_t bytes) {
    if (!page_count || page_count > 0xFFFFFFFFULL) return -1;
    if (!bytes || bytes > page_count * 4096ULL) bytes = page_count * 4096ULL;
    uint64_t entries = 1;
    for (uint64_t index = 1; index < page_count; index++)
        if (pages[index] != pages[index - 1] + 4096ULL) entries++;
    struct virtio_gpu_mem_entry *table = backing;
    uint64_t discarded = 0;
    if (entries > BACKING_ENTRIES) {
        table = (struct virtio_gpu_mem_entry *)dma_alloc(entries * sizeof(*table), 0, &discarded);
        if (!table) return -1;
    }
    uint64_t remaining = bytes;
    uint64_t used = 0;
    for (uint64_t index = 0; index < page_count && remaining; index++) {
        uint32_t length = remaining > 4096ULL ? 4096U : (uint32_t)remaining;
        if (used && table[used - 1].address + table[used - 1].length == pages[index] &&
            table[used - 1].length <= 0xFFFFFFFFU - length) {
            table[used - 1].length += length;
        } else {
            table[used].address = pages[index];
            table[used].length = length;
            table[used].padding = 0;
            used++;
        }
        remaining -= length;
    }
    begin(VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    request.attach.resource_id = resource;
    request.attach.nr_entries = (uint32_t)used;
    int status;
    if (table == backing &&
        submit_async(sizeof(request.attach), table, (uint32_t)(used * sizeof(table[0]))) == 0)
        status = 0;
    else
        status = submit(sizeof(request.attach), table, (uint32_t)(used * sizeof(table[0])),
                        sizeof(struct virtio_gpu_ctrl_hdr));
    if (table != backing) dma_free(table, entries * sizeof(*table));
    return status;
}

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

static void query_capsets(void) {
    uint32_t count = virtio_config_read32(&device, VIRTIO_GPU_CONFIG_NUM_CAPSETS);
    for (uint32_t index = 0; index < count; index++) {
        begin(VIRTIO_GPU_CMD_GET_CAPSET_INFO);
        request.capset_info.capset_index = index;
        if (submit(sizeof(request.capset_info), NULL, 0, sizeof(response.capset_info)) != 0)
            continue;

        uint32_t id = response.capset_info.capset_id;
        if (id != VIRTIO_GPU_CAPSET_VIRGL && id != VIRTIO_GPU_CAPSET_VIRGL2) continue;

        if (response.capset_info.capset_max_size > MAX_CAPSET_BYTES) continue;
        if (capset_id == VIRTIO_GPU_CAPSET_VIRGL2 && id == VIRTIO_GPU_CAPSET_VIRGL) continue;

        capset_id = id;
        capset_version = response.capset_info.capset_max_version;
        capset_size = response.capset_info.capset_max_size;
    }
}

int virtgpu_pci_identity(struct virtgpu_pci_identity *out) {
    VIRTGPU_LOCKED;
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
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !out || !bytes) return -1;
    if (bytes > MAX_CAPSET_BYTES) return -1;

    begin(VIRTIO_GPU_CMD_GET_CAPSET);
    request.capset.capset_id = id;
    request.capset.capset_version = version;

    if (submit(sizeof(request.capset), NULL, 0,
               (uint32_t)sizeof(struct virtio_gpu_ctrl_hdr) + bytes) != 0)
        return -1;
    if (response.hdr.type != VIRTIO_GPU_RESP_OK_CAPSET) return -1;
    memcpy(out, response.capset.capset_data, bytes);
    return 0;
}

int virtgpu_context_create(uint32_t context, const char *name) {
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !context) return -1;

    begin(VIRTIO_GPU_CMD_CTX_CREATE);
    request.ctx_create.hdr.ctx_id = context;

    uint32_t length = 0;
    if (name) {
        while (name[length] && length < (uint32_t)sizeof(request.ctx_create.debug_name) - 1U) {
            request.ctx_create.debug_name[length] = name[length];
            length++;
        }
    }
    request.ctx_create.nlen = length;
    return submit(sizeof(request.ctx_create), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

void virtgpu_context_destroy(uint32_t context) {
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !context) return;
    begin(VIRTIO_GPU_CMD_CTX_DESTROY);
    request.hdr.ctx_id = context;
    (void)submit(sizeof(request.hdr), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

int virtgpu_context_attach(uint32_t context, uint32_t resource, int attach) {
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !context || !resource) return -1;
    begin(attach ? VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE : VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE);
    request.ctx_resource.hdr.ctx_id = context;
    request.ctx_resource.resource_id = resource;
    if (submit_async(sizeof(request.ctx_resource), NULL, 0) == 0) return 0;
    return submit(sizeof(request.ctx_resource), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

uint32_t virtgpu_resource_create_3d(const struct virtgpu_resource_3d *spec, const uint64_t *pages,
                                    uint64_t page_count, uint64_t bytes) {
    VIRTGPU_LOCKED;
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
    if (submit_async(sizeof(request.create_3d), NULL, 0) != 0 &&
        submit(sizeof(request.create_3d), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
        return 0;

    if (pages && page_count && attach_backing(resource, pages, page_count, bytes) != 0) {
        resource_destroy_now(resource);
        return 0;
    }

    next_resource_id++;
    return resource;
}

int virtgpu_transfer_3d(uint32_t context, uint32_t resource, const struct virtgpu_box *box,
                        uint64_t offset, uint32_t level, uint32_t stride, uint32_t layer_stride,
                        int to_host) {
    wait_for_room(1);
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !resource || !box) return -1;

    begin(to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    request.transfer_3d.hdr.ctx_id = context;
    request.transfer_3d.box = *box;
    request.transfer_3d.offset = offset;
    request.transfer_3d.resource_id = resource;
    request.transfer_3d.level = level;
    request.transfer_3d.stride = stride;
    request.transfer_3d.layer_stride = layer_stride;

    if (to_host && submit_async(sizeof(request.transfer_3d), NULL, 0) == 0) return 0;
    return submit(sizeof(request.transfer_3d), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

int virtgpu_submit_3d(uint32_t context, const void *buffer, uint32_t bytes) {
    wait_for_room(1);
    VIRTGPU_LOCKED;
    if (!virtgpu_virgl_available() || !context || !buffer) return -1;
    if (!bytes || bytes > MAX_COMMAND_BYTES) return -1;

    if (bytes % 4U) return -1;

    memcpy(commands, buffer, bytes);
    begin(VIRTIO_GPU_CMD_SUBMIT_3D);
    request.submit_3d.hdr.ctx_id = context;
    request.submit_3d.size = bytes;
    if (submit_async(sizeof(request.submit_3d), commands, bytes) == 0) return 0;
    return submit(sizeof(request.submit_3d), commands, bytes, sizeof(struct virtio_gpu_ctrl_hdr));
}

static uint64_t completions;

static void completion_reap(void *unused) {
    (void)unused;
    uint64_t before = __atomic_load_n(&completed_seen, __ATOMIC_ACQUIRE);
    {
        VIRTGPU_LOCKED;
        if (!ready) return;
        reclaim_control();
    }
    if (__atomic_load_n(&completed_seen, __ATOMIC_ACQUIRE) == before) return;
    (void)process_wake_io();
    process_io_recheck();
}

static struct work completion_work = WORK_INITIALIZER(completion_reap, NULL);

static void control_queue_interrupt(void *context) {
    (void)context;
    __atomic_add_fetch(&completions, 1, __ATOMIC_RELAXED);
    (void)process_wake_all(&completion_channel);
    work_queue(&completion_work);
}

uint64_t virtgpu_interrupt_count(void) { return completions; }

int virtgpu_init(void) {
    VIRTGPU_LOCKED;
    uint64_t granted = 0;
    if (virtio_pci_attach(&device, VIRTIO_GPU_DEVICE_ID, 1ULL << VIRTIO_GPU_F_VIRGL, &granted) != 0)
        return -1;

    uint64_t discarded = 0;
    backing =
        (struct virtio_gpu_mem_entry *)dma_alloc(sizeof(*backing) * BACKING_ENTRIES, 0, &discarded);
    commands = (uint8_t *)dma_alloc(MAX_COMMAND_BYTES, 0, &discarded);
    async_arena =
        (uint8_t *)dma_alloc((uint64_t)ASYNC_SLOTS * ASYNC_SLOT_BYTES, 0, &async_arena_physical);
    if (async_arena)
        for (unsigned index = 0; index < ASYNC_SLOTS; index++) {
            async_slots[index].base = async_arena + (uint64_t)index * ASYNC_SLOT_BYTES;
            async_slots[index].physical = async_arena_physical + (uint64_t)index * ASYNC_SLOT_BYTES;
            async_slots[index].sequence = 0;
        }
    if (!backing || !commands) {
        if (backing) dma_free(backing, sizeof(*backing) * BACKING_ENTRIES);
        if (commands) dma_free(commands, MAX_COMMAND_BYTES);
        backing = NULL;
        commands = NULL;
        virtio_pci_set_failed(&device);
        return -1;
    }
    virgl = (granted & (1ULL << VIRTIO_GPU_F_VIRGL)) != 0;

    virtio_pci_request_irq(&device, "virtio-gpu", control_queue_interrupt, NULL);
    if (virtio_pci_setup_queue(&device, &control, VIRTIO_GPU_CONTROL_QUEUE) != 0) {
        virtio_pci_set_failed(&device);
        return -1;
    }
    cursor_arena =
        (uint8_t *)dma_alloc((uint64_t)CURSOR_SLOTS * CURSOR_SLOT_BYTES, 0, &cursor_arena_physical);
    if (cursor_arena && virtio_pci_setup_queue(&device, &cursorq, VIRTIO_GPU_CURSOR_QUEUE) == 0)
        cursor_ready = 1;
    virtio_pci_set_driver_ok(&device);

    if (query_display_info() != 0) {
        virtio_pci_set_failed(&device);
        return -1;
    }
    ready = 1;
    if (virgl) query_capsets();
    if (virtgpu_virgl_available())
        kprintf("TUNIX: virtio-gpu ready, virgl capset %u version %u, %u bytes\n", capset_id,
                capset_version, capset_size);
    else kprintf("TUNIX: virtio-gpu ready, 2D only\n");
    if (device.vector) kprintf("TUNIX: virtio-gpu interrupts on vector %u\n", device.vector);
    return 0;
}

int virtgpu_available(void) { return ready; }
uint32_t virtgpu_display_width(void) { return display_width; }
uint32_t virtgpu_display_height(void) { return display_height; }

uint32_t virtgpu_resource_create(uint32_t width, uint32_t height, const uint64_t *pages,
                                 uint64_t page_count) {
    VIRTGPU_LOCKED;
    if (!ready || !width || !height || !pages) return 0;
    if (!page_count) return 0;

    uint32_t resource = next_resource_id;
    begin(VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    request.create.resource_id = resource;
    request.create.format = VIRTIO_GPU_FORMAT_B8G8R8X8;
    request.create.width = width;
    request.create.height = height;
    if (submit(sizeof(request.create), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0) return 0;

    if (attach_backing(resource, pages, page_count, (uint64_t)width * 4ULL * height) != 0) {
        resource_destroy_now(resource);
        return 0;
    }

    next_resource_id++;
    return resource;
}

static void resource_destroy_now(uint32_t resource) {
    VIRTGPU_LOCKED;
    if (!ready || !resource) return;
    if (scanout_resource == resource) virtgpu_scanout_disable();
    begin(VIRTIO_GPU_CMD_RESOURCE_UNREF);
    request.unref.resource_id = resource;
    (void)submit(sizeof(request.unref), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

uint64_t virtgpu_resource_release(uint32_t resource) {
    wait_for_room(1);
    VIRTGPU_LOCKED;
    if (!ready || !resource) return 0;
    if (scanout_resource == resource) virtgpu_scanout_disable();
    begin(VIRTIO_GPU_CMD_RESOURCE_UNREF);
    request.unref.resource_id = resource;
    if (submit_async(sizeof(request.unref), NULL, 0) == 0) return control.posted;
    (void)submit(sizeof(request.unref), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    return 0;
}

int virtgpu_sequence_done(uint64_t sequence) {
    if (!sequence) return 1;
    return completed_through(sequence);
}

uint64_t virtgpu_completed(void) { return __atomic_load_n(&completed_seen, __ATOMIC_ACQUIRE); }

uint64_t virtgpu_flip_fence(void) {
    VIRTGPU_LOCKED;
    if (!ready || !device.vector || !control.interrupt_driven) return 0;
    return control.posted;
}

uint64_t virtgpu_posted(void) {
    VIRTGPU_LOCKED;
    return ready ? control.posted : 0;
}

int virtgpu_wait_sequence(uint64_t sequence) {
    if (!sequence) return 0;
    return wait_completed(sequence);
}

static void set_rect(struct virtio_gpu_rect *r, uint32_t width, uint32_t height) {
    r->x = 0;
    r->y = 0;
    r->width = width;
    r->height = height;
}

int virtgpu_present(uint32_t resource, uint32_t stride_pixels, uint32_t width, uint32_t height,
                    const struct virtgpu_rect *damage, int upload) {
    wait_for_room(3);
    VIRTGPU_LOCKED;
    if (!ready || !resource || !width || !height) return -1;
    struct virtio_gpu_rect dirty;
    set_rect(&dirty, width, height);
    if (damage && damage->width && damage->height && damage->x < width && damage->y < height) {
        dirty.x = damage->x;
        dirty.y = damage->y;
        dirty.width = damage->width < width - damage->x ? damage->width : width - damage->x;
        dirty.height = damage->height < height - damage->y ? damage->height : height - damage->y;
    }

    if (scanout_resource != resource) {
        begin(VIRTIO_GPU_CMD_SET_SCANOUT);
        set_rect(&request.scanout.r, width, height);
        request.scanout.resource_id = resource;
        if (submit_async(sizeof(request.scanout), NULL, 0) != 0 &&
            submit(sizeof(request.scanout), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
            return -1;
        scanout_resource = resource;
    }

    if (upload) {
        begin(VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
        request.transfer.r = dirty;
        request.transfer.offset = ((uint64_t)dirty.y * stride_pixels + dirty.x) * 4U;
        request.transfer.resource_id = resource;
        if (submit_async(sizeof(request.transfer), NULL, 0) != 0 &&
            submit(sizeof(request.transfer), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
            return -1;
    }

    begin(VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    request.flush.r = dirty;
    request.flush.resource_id = resource;
    if (submit_async(sizeof(request.flush), NULL, 0) == 0) return 0;
    return submit(sizeof(request.flush), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
}

static uint32_t console_resource;

int virtgpu_console_present(uint64_t physical, uint32_t stride_pixels, uint32_t width,
                            uint32_t height) {
    VIRTGPU_LOCKED;
    if (!ready || !stride_pixels || !width || !height) return -1;
    if (!console_resource) {
        uint64_t bytes = (uint64_t)stride_pixels * 4U * height;
        uint64_t page_count = (bytes + 4095U) / 4096U;
        uint64_t *pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
        if (!pages) return -1;
        for (uint64_t index = 0; index < page_count; index++)
            pages[index] = physical + index * 4096U;
        console_resource = virtgpu_resource_create(stride_pixels, height, pages, page_count);
        kfree(pages);
        if (!console_resource) return -1;
    }
    return virtgpu_present(console_resource, stride_pixels, width, height, NULL, 1);
}

void virtgpu_scanout_disable(void) {
    VIRTGPU_LOCKED;
    if (!ready || !scanout_resource) return;
    begin(VIRTIO_GPU_CMD_SET_SCANOUT);
    set_rect(&request.scanout.r, display_width, display_height);
    request.scanout.resource_id = 0;
    (void)submit(sizeof(request.scanout), NULL, 0, sizeof(struct virtio_gpu_ctrl_hdr));
    scanout_resource = 0;
}
