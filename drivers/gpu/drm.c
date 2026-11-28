#include <stddef.h>
#include <stdint.h>
#include <tunix/cpu.h>
#include <tunix/cred.h>
#include <tunix/percpu.h>
#include <tunix/drm.h>
#include <tunix/display.h>
#include <tunix/file.h>
#include <tunix/framebuffer.h>
#include <tunix/heap.h>
#include <tunix/boot.h>
#include <tunix/time.h>
#include <tunix/timer.h>
#include <tunix/kstring.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/usercopy.h>
#include <tunix/vt.h>
#include <tunix/virtgpu.h>
#include <tunix/vmm.h>
#include <tunix/lock.h>
#include <tunix/mutex.h>
#include <tunix/smp.h>
#include <tunix/kprintf.h>
#include <tunix/errno.h>

static struct mutex drm_lock = MUTEX_INITIALIZER("drm", LOCK_RANK_CHAR);

static void drm_guard_release(int *unused) {
    (void)unused;
    mutex_unlock(&drm_lock);
}

#define DRM_LOCKED \
    __attribute__((cleanup(drm_guard_release))) int drm_guard = (mutex_lock(&drm_lock), 0)

#define DRM_CLOEXEC 02000000

#define DRM_IOCTL_TYPE      'd'
#define IOCTL_TYPE(request) (((request) >> 8) & 0xFFU)
#define IOCTL_NR(request)   ((request) & 0xFFU)

#define DRM_NR_VERSION                0x00
#define DRM_NR_GET_UNIQUE             0x01
#define DRM_NR_GET_MAGIC              0x02
#define DRM_NR_GEM_CLOSE              0x09
#define DRM_NR_GET_CAP                0x0c
#define DRM_NR_SET_CLIENT_CAP         0x0d
#define DRM_NR_SET_VERSION            0x07
#define DRM_NR_AUTH_MAGIC             0x11
#define DRM_NR_SET_MASTER             0x1e
#define DRM_NR_DROP_MASTER            0x1f
#define DRM_NR_MODE_GETRESOURCES      0xa0
#define DRM_NR_MODE_GETCRTC           0xa1
#define DRM_NR_MODE_SETCRTC           0xa2
#define DRM_NR_MODE_GETENCODER        0xa6
#define DRM_NR_MODE_GETCONNECTOR      0xa7
#define DRM_NR_MODE_GETFB             0xad
#define DRM_NR_MODE_ADDFB             0xae
#define DRM_NR_MODE_RMFB              0xaf
#define DRM_NR_MODE_CLOSEFB           0xd0
#define DRM_NR_MODE_PAGE_FLIP         0xb0
#define DRM_NR_MODE_CURSOR            0xa3
#define DRM_NR_MODE_GETGAMMA          0xa4
#define DRM_NR_MODE_SETGAMMA          0xa5
#define DRM_NR_MODE_CURSOR2           0xbb
#define DRM_NR_MODE_ATOMIC            0xbc
#define DRM_NR_MODE_CREATEPROPBLOB    0xbd
#define DRM_NR_MODE_DESTROYPROPBLOB   0xbe
#define DRM_NR_MODE_DIRTYFB           0xb1
#define DRM_NR_MODE_CREATE_DUMB       0xb2
#define DRM_NR_MODE_MAP_DUMB          0xb3
#define DRM_NR_MODE_DESTROY_DUMB      0xb4
#define DRM_NR_MODE_ADDFB2            0xb8
#define DRM_NR_MODE_GETPROPERTY       0xaa
#define DRM_NR_MODE_SETPROPERTY       0xab
#define DRM_NR_MODE_GETPLANERESOURCES 0xb5
#define DRM_NR_MODE_GETPLANE          0xb6
#define DRM_NR_MODE_SETPLANE          0xb7
#define DRM_NR_MODE_OBJ_GETPROPERTIES 0xb9
#define DRM_NR_MODE_OBJ_SETPROPERTY   0xba
#define DRM_NR_PRIME_HANDLE_TO_FD     0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE     0x2e
#define DRM_NR_MODE_CREATE_LEASE      0xc6
#define DRM_NR_MODE_LIST_LESSEES      0xc7
#define DRM_NR_MODE_GET_LEASE         0xc8
#define DRM_NR_MODE_REVOKE_LEASE      0xc9

#define DRM_COMMAND_BASE                    0x40
#define DRM_NR_VIRTGPU_MAP                  (DRM_COMMAND_BASE + 0x01)
#define DRM_NR_VIRTGPU_EXECBUFFER           (DRM_COMMAND_BASE + 0x02)
#define DRM_NR_VIRTGPU_GETPARAM             (DRM_COMMAND_BASE + 0x03)
#define DRM_NR_VIRTGPU_RESOURCE_CREATE      (DRM_COMMAND_BASE + 0x04)
#define DRM_NR_VIRTGPU_RESOURCE_INFO        (DRM_COMMAND_BASE + 0x05)
#define DRM_NR_VIRTGPU_TRANSFER_FROM_HOST   (DRM_COMMAND_BASE + 0x06)
#define DRM_NR_VIRTGPU_TRANSFER_TO_HOST     (DRM_COMMAND_BASE + 0x07)
#define DRM_NR_VIRTGPU_WAIT                 (DRM_COMMAND_BASE + 0x08)
#define DRM_NR_VIRTGPU_GET_CAPS             (DRM_COMMAND_BASE + 0x09)
#define DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB (DRM_COMMAND_BASE + 0x0a)
#define DRM_NR_VIRTGPU_CONTEXT_INIT         (DRM_COMMAND_BASE + 0x0b)

#define VIRTGPU_PARAM_3D_FEATURES          1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX     2
#define VIRTGPU_PARAM_RESOURCE_BLOB        3
#define VIRTGPU_PARAM_HOST_VISIBLE         4
#define VIRTGPU_PARAM_CROSS_DEVICE         5
#define VIRTGPU_PARAM_CONTEXT_INIT         6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS 7

#define DRM_MAX_COMMAND_BYTES (1024U * 1024U)

#define VIRTGPU_EXECBUF_FENCE_FD_IN  0x01U
#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02U
#define VIRTGPU_EXECBUF_RING_IDX     0x04U

#define VIRTGPU_BLOB_MEM_GUEST       1U
#define VIRTGPU_BLOB_MEM_HOST3D      2U
#define VIRTGPU_BLOB_MEM_HOST3D_GUEST 3U
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 1U

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID       1U
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS       2U
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 3U
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME      4U

#define VIRTIO_GPU_MAP_CACHE_MASK     0x0fU
#define VIRTIO_GPU_MAP_CACHE_UNCACHED 2U
#define VIRTIO_GPU_MAP_CACHE_WC       3U

struct drm_virtgpu_resource_create_blob {
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t bo_handle;
    uint32_t res_handle;
    uint64_t size;
    uint32_t pad;
    uint32_t cmd_size;
    uint64_t cmd;
    uint64_t blob_id;
};

struct drm_virtgpu_context_set_param {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_context_init {
    uint32_t num_params;
    uint32_t pad;
    uint64_t ctx_set_params;
};

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

struct drm_virtgpu_getparam {
    uint64_t param;
    uint64_t value;
};

struct drm_virtgpu_execbuffer {
    uint32_t flags;
    uint32_t size;
    uint64_t command;
    uint64_t bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx;
    uint32_t syncobj_stride;
    uint32_t num_in_syncobjs;
    uint32_t num_out_syncobjs;
    uint64_t in_syncobjs;
    uint64_t out_syncobjs;
};

struct drm_virtgpu_resource_create {
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
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t stride;
};

struct drm_virtgpu_resource_info {
    uint32_t bo_handle;
    uint32_t res_handle;
    uint32_t size;
    uint32_t blob_mem;
};

struct drm_virtgpu_3d_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
};

struct drm_virtgpu_3d_transfer {
    uint32_t bo_handle;
    struct drm_virtgpu_3d_box box;
    uint32_t level;
    uint32_t offset;
    uint32_t stride;
    uint32_t layer_stride;
};

struct drm_virtgpu_3d_wait {
    uint32_t handle;
    uint32_t flags;
};

struct drm_virtgpu_get_caps {
    uint32_t cap_set_id;
    uint32_t cap_set_ver;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
};

typedef char
    drm_virtgpu_execbuffer_size_check[(sizeof(struct drm_virtgpu_execbuffer) == 64) ? 1 : -1];
typedef char
    drm_virtgpu_resource_create_size_check[(sizeof(struct drm_virtgpu_resource_create) == 56) ? 1
                                                                                              : -1];
typedef char
    drm_virtgpu_transfer_size_check[(sizeof(struct drm_virtgpu_3d_transfer) == 44) ? 1 : -1];
typedef char drm_virtgpu_get_caps_size_check[(sizeof(struct drm_virtgpu_get_caps) == 24) ? 1 : -1];

#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_PRIME                0x5
#define DRM_PRIME_CAP_IMPORT         0x1
#define DRM_PRIME_CAP_EXPORT         0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12

#define DRM_CRTC_ID      1
#define DRM_CONNECTOR_ID 2
#define DRM_ENCODER_ID   3
#define DRM_PLANE_ID     4
#define DRM_CURSOR_ID    5

#define DRM_PROP_TYPE_ID               10
#define DRM_PROP_CRTC_ACTIVE           11
#define DRM_PROP_CRTC_MODE_ID          12
#define DRM_PROP_PLANE_CRTC_ID         13
#define DRM_PROP_PLANE_FB_ID           14
#define DRM_PROP_CONNECTOR_CRTC_ID     15
#define DRM_PROP_PLANE_SRC_X           16
#define DRM_PROP_PLANE_SRC_Y           17
#define DRM_PROP_PLANE_SRC_W           18
#define DRM_PROP_PLANE_SRC_H           19
#define DRM_PROP_PLANE_CRTC_X          20
#define DRM_PROP_PLANE_CRTC_Y          21
#define DRM_PROP_PLANE_CRTC_W          22
#define DRM_PROP_PLANE_CRTC_H          23
#define DRM_PROP_PLANE_FB_DAMAGE_CLIPS 24
#define DRM_PROP_CONNECTOR_DPMS        25
#define DRM_PROP_PLANE_FIRST_RECT      DRM_PROP_PLANE_SRC_X
#define DRM_PROP_PLANE_LAST_RECT       DRM_PROP_PLANE_CRTC_H

#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER   0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee

#define DRM_MODE_PROP_IMMUTABLE    (1 << 2)
#define DRM_MODE_PROP_RANGE        (1 << 1)
#define DRM_MODE_PROP_BLOB         (1 << 4)
#define DRM_MODE_PROP_ENUM         (1 << 3)
#define DRM_MODE_PROP_OBJECT       (1 << 6)
#define DRM_MODE_PROP_SIGNED_RANGE (2 << 6)
#define DRM_MODE_PROP_ATOMIC       0x80000000U
#define DRM_MODE_OBJECT_ANY        0
#define DRM_MODE_OBJECT_FB         0xfbfbfbfbULL

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR  2

#define DRM_PROP_NAME_LEN 32

#define DRM_FORMAT_XRGB8888 0x34325258
#define DRM_FORMAT_ARGB8888 0x34325241

#define DRM_MODE_CONNECTED         1
#define DRM_MODE_SUBPIXEL_UNKNOWN  1
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_ENCODER_VIRTUAL   5
#define DRM_MODE_TYPE_PREFERRED    (1 << 3)
#define DRM_MODE_FLAG_PHSYNC 0x1U
#define DRM_MODE_FLAG_NHSYNC 0x2U
#define DRM_MODE_FLAG_PVSYNC 0x4U
#define DRM_MODE_FLAG_NVSYNC 0x8U
#define DRM_MODE_TYPE_DRIVER       (1 << 6)

#define DRM_DISPLAY_MODE_LEN 32

struct drm_version {
    int32_t version_major;
    int32_t version_minor;
    int32_t version_patchlevel;
    uint32_t __pad;
    uint64_t name_len;
    uint64_t name;
    uint64_t date_len;
    uint64_t date;
    uint64_t desc_len;
    uint64_t desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_property_enum {
    uint64_t value;
    char name[DRM_PROP_NAME_LEN];
};

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
};

struct drm_mode_create_blob {
    uint64_t data;
    uint32_t length;
    uint32_t blob_id;
};
struct drm_mode_destroy_blob {
    uint32_t blob_id;
};

typedef char drm_modeinfo_size_check[(sizeof(struct drm_mode_modeinfo) == 68) ? 1 : -1];
typedef char drm_create_dumb_size_check[(sizeof(struct drm_mode_create_dumb) == 32) ? 1 : -1];

#define DRM_MAX_DIMENSION 16384U

struct drm_dumb_buffer {
    uint32_t handle;
    const struct file *owner;
    uint8_t shared;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
    uint64_t page_count;
    uint64_t *pages;
    uint32_t virtio_resource;
    uint8_t rendered;
    uint8_t owner_closed;
    uint32_t refs;
    uint64_t busy_until;
    // A blob the host allocated: no guest pages, mapped (if at all) from the device's
    // host-visible region.
    uint8_t host_blob;
    uint32_t blob_mem;
    uint64_t host_physical;
    uint32_t map_info;
    // A render driver's buffer in device memory, mapped write-combined.
    uint8_t device_memory;
};

struct drm_import {
    const struct file *file;
    uint32_t handle;
};

struct drm_retired {
    uint64_t *pages;
    uint64_t page_count;
    uint64_t sequence;
};

struct drm_framebuffer {
    uint32_t id;
    const struct file *owner;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_MAX_EVENTS          64
#define DRM_VBLANK_NS           16666667ULL
#define DRM_EVENT_QUEUES        32

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

struct drm_event_queue {
    const struct file *owner;
    struct drm_event_vblank events[DRM_MAX_EVENTS];
    uint64_t fences[DRM_MAX_EVENTS];
    uint64_t due_ns[DRM_MAX_EVENTS];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

static struct drm_event_queue event_queues[DRM_EVENT_QUEUES];
static uint32_t flip_sequence;

static const char drm_display_owner;
static uint32_t open_count;

#define DROPPED_MASTERS 16
static const struct file *master_file;
static const struct file *dropped_masters[DROPPED_MASTERS];

static struct drm_dumb_buffer **buffers;
static const struct drm_render_driver *render_driver;
static uint32_t buffer_capacity;
static struct drm_framebuffer *framebuffers;
static int framebuffer_capacity;
#define DRM_MAX_DAMAGE_RECTS 16
#define DRM_MAX_BLOB_BYTES   65536
struct drm_property_blob {
    uint32_t id;
    const struct file *owner;
    uint32_t length;
    uint8_t *data;
};
static struct drm_property_blob *blobs;
static unsigned blob_capacity;
static uint32_t next_blob_id = 1;
static uint32_t next_handle = 1;
struct drm_render_context {
    uint64_t pid;
    uint32_t context;
};
static struct drm_render_context *render_contexts;
static struct drm_import *imports;
static int import_capacity;
static struct drm_retired *retired;
static int retired_capacity;
static int retired_count;
static uint64_t untracked_busy_until;
static int context_capacity;

static void *grow_table(void *table, size_t element, size_t *capacity) {
    size_t grown_capacity = *capacity ? *capacity * 2 : 16;
    uint8_t *grown = kmalloc(grown_capacity * element);
    if (!grown) return NULL;
    memset(grown, 0, grown_capacity * element);
    if (*capacity) memcpy(grown, table, *capacity * element);
    kfree(table);
    *capacity = grown_capacity;
    return grown;
}

static int grow_int_table(void **table, size_t element, int *capacity) {
    size_t value = (size_t)*capacity;
    void *grown = grow_table(*table, element, &value);
    if (!grown) return -1;
    *table = grown;
    *capacity = (int)value;
    return 0;
}
static uint32_t next_render_context = 1;
static void render_contexts_release(void);
static uint32_t render_context(void);
static uint32_t client_context(const struct file *client);
static uint32_t next_fb_id = 1;
static uint32_t active_fb_id;
static int drm_ready;

#define DRM_MAP_OFFSET_BASE 0x100000000ULL

void drm_init(void) {
    next_handle = 1;
    next_fb_id = 1;
    next_blob_id = 1;
    active_fb_id = 0;
    memset(event_queues, 0, sizeof(event_queues));
    flip_sequence = 0;
    open_count = 0;
    next_render_context = 1;
    drm_ready = framebuffer_available();
}

int drm_available(void) { return drm_ready; }

static struct drm_dumb_buffer *buffer_slot(uint32_t *handle_out) {
    for (uint32_t step = 0; step < buffer_capacity; step++) {
        uint32_t index = (next_handle - 1U + step) % buffer_capacity;
        if (!buffers[index] || !buffers[index]->handle) {
            if (!buffers[index]) {
                buffers[index] = kmalloc(sizeof(struct drm_dumb_buffer));
                if (!buffers[index]) return NULL;
                memset(buffers[index], 0, sizeof(struct drm_dumb_buffer));
            }
            *handle_out = index + 1U;
            next_handle = index + 2U > buffer_capacity ? 1U : index + 2U;
            return buffers[index];
        }
    }
    size_t capacity = buffer_capacity;
    struct drm_dumb_buffer **grown = grow_table(buffers, sizeof(*buffers), &capacity);
    if (!grown) return NULL;
    buffers = grown;
    uint32_t first = buffer_capacity;
    buffer_capacity = (uint32_t)capacity;
    next_handle = first + 1U;
    return buffer_slot(handle_out);
}

static struct drm_dumb_buffer *buffer_find(uint32_t handle) {
    if (!handle || handle > buffer_capacity || !buffers[handle - 1U]) return NULL;
    struct drm_dumb_buffer *buffer = buffers[handle - 1U];
    return buffer->handle == handle ? buffer : NULL;
}

static struct drm_framebuffer *framebuffer_find(uint32_t id) {
    if (!id) return NULL;
    for (int index = 0; index < framebuffer_capacity; index++) {
        if (framebuffers[index].id == id) return &framebuffers[index];
    }
    return NULL;
}

static struct drm_property_blob *blob_find(uint32_t id) {
    if (!id) return NULL;
    for (unsigned index = 0; index < blob_capacity; index++)
        if (blobs[index].id == id) return &blobs[index];
    return NULL;
}

static struct drm_import *import_find(const struct file *client, uint32_t handle) {
    for (int index = 0; index < import_capacity; index++)
        if (imports[index].file == client && imports[index].handle == handle)
            return &imports[index];
    return NULL;
}

static int owns_handle(const struct drm_dumb_buffer *buffer, const struct file *client) {
    return buffer->owner == client && !buffer->owner_closed;
}

static struct drm_dumb_buffer *buffer_of(const struct file *client, uint32_t handle) {
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (!buffer) return NULL;
    if (owns_handle(buffer, client) || import_find(client, handle)) return buffer;
    return (buffer->shared && buffer->owner != client) ? buffer : NULL;
}

static struct drm_framebuffer *framebuffer_of(const struct file *client, uint32_t id) {
    struct drm_framebuffer *fb = framebuffer_find(id);
    return (fb && fb->owner == client) ? fb : NULL;
}

static void blob_release(struct drm_property_blob *blob) {
    kfree(blob->data);
    memset(blob, 0, sizeof(*blob));
}

static struct drm_property_blob *blob_of(const struct file *client, uint32_t id) {
    struct drm_property_blob *blob = blob_find(id);
    return (blob && blob->owner == client) ? blob : NULL;
}

static int64_t ioctl_create_blob(const struct file *client, uint64_t user_argument) {
    struct drm_mode_create_blob request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.length > DRM_MAX_BLOB_BYTES || (request.length && !request.data)) return -EINVAL;
    unsigned free_blob = 0;
    while (free_blob < blob_capacity && blobs[free_blob].id) free_blob++;
    if (free_blob == blob_capacity) {
        size_t capacity = blob_capacity;
        struct drm_property_blob *grown = grow_table(blobs, sizeof(*blobs), &capacity);
        if (!grown) return -ENOMEM;
        blobs = grown;
        blob_capacity = (unsigned)capacity;
    }
    for (unsigned index = free_blob; index < blob_capacity; index++) {
        if (blobs[index].id) continue;
        uint8_t *data = NULL;
        if (request.length) {
            data = (uint8_t *)kmalloc(request.length);
            if (!data) return -ENOMEM;
        }
        if (request.length && copy_from_user(data, request.data, request.length) != 0) {
            kfree(data);
            return -EFAULT;
        }
        blobs[index].data = data;
        blobs[index].id = next_blob_id++;
        if (!blobs[index].id) blobs[index].id = next_blob_id++;
        blobs[index].owner = client;
        blobs[index].length = request.length;
        request.blob_id = blobs[index].id;
        return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
    }
    return -ENOMEM;
}

static int64_t ioctl_destroy_blob(const struct file *client, uint64_t user_argument) {
    struct drm_mode_destroy_blob request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_property_blob *blob = blob_of(client, request.blob_id);
    if (!blob) return -ENOENT;
    blob_release(blob);
    return 0;
}

static int64_t ioctl_getfb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_cmd request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_framebuffer *fb = framebuffer_of(client, request.fb_id);
    if (!fb) return -ENOENT;
    request.width = fb->width;
    request.height = fb->height;
    request.pitch = fb->pitch;
    request.bpp = 32;
    request.depth = 24;
    request.handle = fb->handle;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

#define DRM_MODE_CURSOR_BO   0x01U
#define DRM_MODE_CURSOR_MOVE 0x02U
#define DRM_CURSOR_SIZE      64U

static int32_t cursor_x;
static int32_t cursor_y;
static uint32_t cursor_fb;

// The cursor, a 64x64 ARGB8888 image over the screen: virtio-gpu's, or the
// display driver's when it has one.
static const struct display_flipper *cursor_flipper(void) {
    const struct display_flipper *flipper = display_flipper();
    return flipper && flipper->cursor_set && flipper->cursor_move ? flipper : NULL;
}

static int cursor_available(void) { return virtgpu_cursor_available() || cursor_flipper(); }

// Shows BUFFER (NULL hides the cursor) with its top left at X, Y.
static int cursor_show(struct drm_dumb_buffer *buffer, int32_t x, int32_t y, uint32_t hot_x,
                       uint32_t hot_y) {
    cursor_x = x;
    cursor_y = y;
    if (buffer && (buffer->pitch / 4U < DRM_CURSOR_SIZE || buffer->height < DRM_CURSOR_SIZE))
        return -EINVAL;
    if (virtgpu_cursor_available()) {
        if (!buffer) return virtgpu_cursor_set(0, 0, 0, x, y, 0, 0) == 0 ? 0 : -EIO;
        if (!buffer->virtio_resource) {
            buffer->virtio_resource = virtgpu_resource_create(buffer->pitch / 4U, buffer->height,
                                                              buffer->pages, buffer->page_count);
            if (!buffer->virtio_resource) return -ENOMEM;
        }
        return virtgpu_cursor_set(buffer->virtio_resource, buffer->pitch / 4U, !buffer->rendered,
                                  x, y, hot_x, hot_y) == 0
            ? 0
            : -EIO;
    }
    const struct display_flipper *flipper = cursor_flipper();
    if (!flipper) return -ENXIO;
    if (!buffer) return flipper->cursor_set(NULL) == 0 ? 0 : -EIO;
    if (!buffer->pages) return -EINVAL;
    static uint32_t image[DRM_CURSOR_SIZE * DRM_CURSOR_SIZE];
    for (uint32_t row = 0; row < DRM_CURSOR_SIZE; row++) {
        uint64_t offset = (uint64_t)row * buffer->pitch;
        for (uint32_t done = 0; done < DRM_CURSOR_SIZE * 4U;) {
            uint64_t page = (offset + done) / 4096ULL, within = (offset + done) % 4096ULL;
            if (page >= buffer->page_count) return -EINVAL;
            uint32_t chunk = (uint32_t)(4096ULL - within);
            if (chunk > DRM_CURSOR_SIZE * 4U - done) chunk = DRM_CURSOR_SIZE * 4U - done;
            memcpy((uint8_t *)&image[row * DRM_CURSOR_SIZE] + done,
                   (uint8_t *)vmm_phys_to_virt(buffer->pages[page]) + within, chunk);
            done += chunk;
        }
    }
    if (flipper->cursor_set(image) != 0) return -EIO;
    return flipper->cursor_move(x, y) == 0 ? 0 : -EIO;
}

static int cursor_move_to(int32_t x, int32_t y) {
    cursor_x = x;
    cursor_y = y;
    if (virtgpu_cursor_available()) return virtgpu_cursor_move(x, y) == 0 ? 0 : -EIO;
    const struct display_flipper *flipper = cursor_flipper();
    return flipper && flipper->cursor_move(x, y) == 0 ? 0 : -EIO;
}

static int64_t ioctl_cursor(const struct file *client, uint64_t user_argument, int cursor2) {
    struct {
        struct drm_mode_cursor base;
        int32_t hot_x;
        int32_t hot_y;
    } request;
    memset(&request, 0, sizeof(request));
    if (copy_from_user(&request, user_argument, cursor2 ? sizeof(request) : sizeof(request.base)) !=
        0)
        return -EFAULT;
    if (request.base.crtc_id != DRM_CRTC_ID) return -ENOENT;
    if (!cursor_available()) return -ENXIO;
    int32_t x = request.base.flags & DRM_MODE_CURSOR_MOVE ? request.base.x : cursor_x;
    int32_t y = request.base.flags & DRM_MODE_CURSOR_MOVE ? request.base.y : cursor_y;
    if (request.base.flags & DRM_MODE_CURSOR_BO) {
        cursor_fb = 0;
        if (!request.base.handle) return cursor_show(NULL, x, y, 0, 0);
        struct drm_dumb_buffer *buffer = buffer_of(client, request.base.handle);
        if (!buffer) return -ENOENT;
        if (request.base.width != DRM_CURSOR_SIZE || request.base.height != DRM_CURSOR_SIZE)
            return -EINVAL;
        return cursor_show(buffer, x, y, request.hot_x > 0 ? (uint32_t)request.hot_x : 0,
                           request.hot_y > 0 ? (uint32_t)request.hot_y : 0);
    }
    if (request.base.flags & DRM_MODE_CURSOR_MOVE) return cursor_move_to(x, y);
    return 0;
}

// The gamma ramp, 256 entries a channel, when the display driver has a LUT.
#define DRM_GAMMA_SIZE 256U

struct drm_mode_crtc_lut {
    uint32_t crtc_id;
    uint32_t gamma_size;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
};

static uint16_t gamma_ramp[3][DRM_GAMMA_SIZE];
static int gamma_loaded;

static const struct display_flipper *gamma_flipper(void) {
    const struct display_flipper *flipper = display_flipper();
    return flipper && flipper->gamma_set ? flipper : NULL;
}

static void gamma_identity(void) {
    for (unsigned channel = 0; channel < 3U; channel++)
        for (unsigned index = 0; index < DRM_GAMMA_SIZE; index++)
            gamma_ramp[channel][index] = (uint16_t)(index * 0x101U);
}

static int64_t ioctl_gamma(uint64_t user_argument, int set) {
    struct drm_mode_crtc_lut request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.crtc_id != DRM_CRTC_ID) return -ENOENT;
    const struct display_flipper *flipper = gamma_flipper();
    if (!flipper) return -ENOSYS;
    if (request.gamma_size != DRM_GAMMA_SIZE) return -EINVAL;
    if (!gamma_loaded) gamma_identity();
    uint64_t pointers[3] = {request.red, request.green, request.blue};
    for (unsigned channel = 0; channel < 3U; channel++) {
        int status = set ? copy_from_user(gamma_ramp[channel], pointers[channel],
                                          sizeof(gamma_ramp[channel]))
                         : copy_to_user(pointers[channel], gamma_ramp[channel],
                                        sizeof(gamma_ramp[channel]));
        if (status != 0) return -EFAULT;
    }
    if (!set) return 0;
    gamma_loaded = 1;
    return flipper->gamma_set(gamma_ramp[0], gamma_ramp[1], gamma_ramp[2]) == 0 ? 0 : -EIO;
}

#define DRM_MODE_PAGE_FLIP_EVENT      0x01U
#define DRM_MODE_ATOMIC_TEST_ONLY     0x100U
#define DRM_MODE_ATOMIC_NONBLOCK      0x200U
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x400U
#define DRM_MODE_PAGE_FLIP_ASYNC      0x02U
#define DRM_ATOMIC_MAX_OBJECTS        16U
#define DRM_ATOMIC_MAX_PROPS          64U

struct drm_damage {
    uint32_t count;
    struct drm_mode_rect {
        int32_t x1, y1, x2, y2;
    } rects[DRM_MAX_DAMAGE_RECTS];
};

static int scanout_current;

static int present_framebuffer(const struct file *client, uint32_t fb_id,
                               const struct drm_damage *damage);
static void queue_flip_event(const struct file *client, uint64_t user_data);

static int plane_rectangle_ok(uint32_t property, uint64_t value) {
    switch (property) {
    case DRM_PROP_PLANE_SRC_X:
    case DRM_PROP_PLANE_SRC_Y:
    case DRM_PROP_PLANE_CRTC_X:
    case DRM_PROP_PLANE_CRTC_Y: return value == 0;
    case DRM_PROP_PLANE_SRC_W:  return value == ((uint64_t)framebuffer_width() << 16);
    case DRM_PROP_PLANE_SRC_H:  return value == ((uint64_t)framebuffer_height() << 16);
    case DRM_PROP_PLANE_CRTC_W: return value == framebuffer_width();
    case DRM_PROP_PLANE_CRTC_H: return value == framebuffer_height();
    default:                    return 0;
    }
}

static void damage_collect(struct drm_damage *damage, const uint8_t *data, uint32_t count) {
    damage->count = 0;
    struct drm_mode_rect bounds = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (uint32_t index = 0; index < count; index++) {
        struct drm_mode_rect rect;
        memcpy(&rect, data + (uint64_t)index * sizeof(rect), sizeof(rect));
        if (rect.x1 >= rect.x2 || rect.y1 >= rect.y2) continue;
        if (rect.x1 < bounds.x1) bounds.x1 = rect.x1;
        if (rect.y1 < bounds.y1) bounds.y1 = rect.y1;
        if (rect.x2 > bounds.x2) bounds.x2 = rect.x2;
        if (rect.y2 > bounds.y2) bounds.y2 = rect.y2;
        if (damage->count < DRM_MAX_DAMAGE_RECTS) damage->rects[damage->count] = rect;
        damage->count++;
    }
    if (damage->count > DRM_MAX_DAMAGE_RECTS) {
        damage->rects[0] = bounds;
        damage->count = 1;
    }
}

static int64_t ioctl_atomic(const struct file *client, uint64_t user_argument) {
    struct drm_mode_atomic request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.flags &
        ~(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET |
          DRM_MODE_PAGE_FLIP_EVENT))
        return -EINVAL;
    if (!request.count_objs || request.count_objs > DRM_ATOMIC_MAX_OBJECTS) return -EINVAL;

    uint32_t objects[DRM_ATOMIC_MAX_OBJECTS];
    uint32_t counts[DRM_ATOMIC_MAX_OBJECTS];
    uint64_t bytes = (uint64_t)request.count_objs * sizeof(objects[0]);
    if (copy_from_user(objects, request.objs_ptr, bytes) != 0 ||
        copy_from_user(counts, request.count_props_ptr, bytes) != 0)
        return -EFAULT;

    uint32_t total = 0;
    for (uint32_t object = 0; object < request.count_objs; object++) {
        if (counts[object] > DRM_ATOMIC_MAX_PROPS || total + counts[object] > DRM_ATOMIC_MAX_PROPS)
            return -EINVAL;
        total += counts[object];
    }
    if (!total) return -EINVAL;

    uint32_t props[DRM_ATOMIC_MAX_PROPS];
    uint64_t values[DRM_ATOMIC_MAX_PROPS];
    if (copy_from_user(props, request.props_ptr, (uint64_t)total * sizeof(props[0])) != 0 ||
        copy_from_user(values, request.prop_values_ptr, (uint64_t)total * sizeof(values[0])) != 0)
        return -EFAULT;

    uint32_t new_fb = active_fb_id;
    int primary_touched = 0;
    struct {
        int touched;
        uint32_t fb, crtc;
        int32_t x, y;
        int size_ok; // its source and CRTC sizes those of the cursor image
    } cursor = {0, cursor_fb, cursor_fb ? DRM_CRTC_ID : 0, cursor_x, cursor_y, 1};
    const struct drm_property_blob *damage_blob = NULL;
    int new_active = active_fb_id != 0;
    int mode_cleared = 0;
    uint32_t taken = 0;
    for (uint32_t object = 0; object < request.count_objs; object++) {
        for (uint32_t index = 0; index < counts[object]; index++, taken++) {
            uint32_t id = props[taken];
            uint64_t value = values[taken];
            if (objects[object] == DRM_PLANE_ID) primary_touched = 1;
            if (objects[object] == DRM_CURSOR_ID) {
                if (!cursor_available()) return -EINVAL;
                cursor.touched = 1;
                if (id == DRM_PROP_PLANE_FB_ID) {
                    const struct drm_framebuffer *fb =
                        value > UINT32_MAX ? NULL : framebuffer_of(client, (uint32_t)value);
                    if (value > UINT32_MAX ||
                        (value && (!fb || fb->width != DRM_CURSOR_SIZE ||
                                   fb->height != DRM_CURSOR_SIZE)))
                        return -EINVAL;
                    cursor.fb = (uint32_t)value;
                } else if (id == DRM_PROP_PLANE_CRTC_ID) {
                    if (value != 0 && value != DRM_CRTC_ID) return -EINVAL;
                    cursor.crtc = (uint32_t)value;
                } else if (id == DRM_PROP_PLANE_CRTC_X || id == DRM_PROP_PLANE_CRTC_Y) {
                    int64_t at = (int64_t)value;
                    if (at < INT16_MIN || at > INT16_MAX) return -EINVAL;
                    if (id == DRM_PROP_PLANE_CRTC_X) cursor.x = (int32_t)at;
                    else cursor.y = (int32_t)at;
                } else if (id == DRM_PROP_PLANE_CRTC_W || id == DRM_PROP_PLANE_CRTC_H) {
                    // Checked below, once it is known whether the plane is on:
                    // a plane being turned off has them all zero.
                    if (value != DRM_CURSOR_SIZE) cursor.size_ok = 0;
                } else if (id == DRM_PROP_PLANE_SRC_X || id == DRM_PROP_PLANE_SRC_Y) {
                    if (value != 0) cursor.size_ok = 0;
                } else if (id == DRM_PROP_PLANE_SRC_W || id == DRM_PROP_PLANE_SRC_H) {
                    if (value != (uint64_t)DRM_CURSOR_SIZE << 16) cursor.size_ok = 0;
                } else if (id != DRM_PROP_PLANE_FB_DAMAGE_CLIPS) {
                    return -EINVAL;
                }
                continue;
            }
            if (objects[object] == DRM_CRTC_ID && id == DRM_PROP_CRTC_ACTIVE) {
                if (value > 1) return -EINVAL;
                new_active = (int)value;
            } else if (objects[object] == DRM_CRTC_ID && id == DRM_PROP_CRTC_MODE_ID) {
                if (!value) {
                    mode_cleared = 1;
                    continue;
                }
                struct drm_property_blob *blob = blob_of(client, (uint32_t)value);
                if (!blob || blob->length < sizeof(struct drm_mode_modeinfo)) return -EINVAL;
                struct drm_mode_modeinfo mode;
                memcpy(&mode, blob->data, sizeof(mode));
                if (mode.hdisplay != framebuffer_width() || mode.vdisplay != framebuffer_height())
                    return -EINVAL;
            } else if (objects[object] == DRM_PLANE_ID && id == DRM_PROP_PLANE_FB_ID) {
                if (value > UINT32_MAX || (value && !framebuffer_of(client, (uint32_t)value)))
                    return -EINVAL;
                new_fb = (uint32_t)value;
            } else if (objects[object] == DRM_PLANE_ID && id == DRM_PROP_PLANE_FB_DAMAGE_CLIPS) {
                if (!value) {
                    damage_blob = NULL;
                    continue;
                }
                struct drm_property_blob *blob =
                    value > UINT32_MAX ? NULL : blob_of(client, (uint32_t)value);
                if (!blob || blob->length % sizeof(struct drm_mode_rect)) return -EINVAL;
                damage_blob = blob;
            } else if (objects[object] == DRM_PLANE_ID && id == DRM_PROP_PLANE_CRTC_ID) {
                if (value != 0 && value != DRM_CRTC_ID) return -EINVAL;
            } else if (objects[object] == DRM_PLANE_ID && id >= DRM_PROP_PLANE_FIRST_RECT &&
                       id <= DRM_PROP_PLANE_LAST_RECT) {
                if (!plane_rectangle_ok(id, value)) return -EINVAL;
            } else if (objects[object] == DRM_CONNECTOR_ID && id == DRM_PROP_CONNECTOR_CRTC_ID) {
                if (value != 0 && value != DRM_CRTC_ID) return -EINVAL;
            } else {
                return -EINVAL;
            }
        }
    }
    if (cursor.touched && cursor.fb && cursor.crtc && !cursor.size_ok) return -EINVAL;
    if (mode_cleared) new_active = 0;
    if (request.flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;

    if (cursor.touched) {
        const struct drm_framebuffer *fb = cursor.fb ? framebuffer_of(client, cursor.fb) : NULL;
        struct drm_dumb_buffer *buffer = fb ? buffer_of(client, fb->handle) : NULL;
        int status;
        if (!buffer || !cursor.crtc) status = cursor_show(NULL, cursor.x, cursor.y, 0, 0);
        else if (cursor.fb == cursor_fb) status = cursor_move_to(cursor.x, cursor.y);
        else status = cursor_show(buffer, cursor.x, cursor.y, 0, 0);
        if (status != 0) return status;
        cursor_fb = buffer && cursor.crtc ? cursor.fb : 0;
    }
    // A commit of the cursor alone leaves the screen as it is.
    if (!primary_touched && new_active == (active_fb_id != 0) && new_fb == active_fb_id) {
        if (request.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(client, request.user_data);
        return 0;
    }

    if (!new_active || !new_fb) {
        active_fb_id = 0;
        virtgpu_scanout_disable();
        scanout_current = 0;
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
    } else {
        struct drm_damage damage;
        const struct drm_damage *clips = NULL;
        if (damage_blob) {
            damage_collect(&damage, damage_blob->data,
                           damage_blob->length / sizeof(struct drm_mode_rect));
            clips = &damage;
        }
        int status = present_framebuffer(client, new_fb, clips);
        if (status != 0) return status;
        active_fb_id = new_fb;
    }
    if (request.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(client, request.user_data);
    return 0;
}

static void pages_free(uint64_t *pages, uint64_t page_count) {
    for (uint64_t index = 0; index < page_count; index++)
        if (pages[index]) pmm_free_page((void *)pages[index]);
    kfree(pages);
}

static void retired_reap(void) {
    if (!retired_count) return;
    for (int index = 0; index < retired_capacity; index++) {
        if (!retired[index].pages || !virtgpu_sequence_done(retired[index].sequence)) continue;
        pages_free(retired[index].pages, retired[index].page_count);
        memset(&retired[index], 0, sizeof(retired[index]));
        retired_count--;
    }
}

static void pages_retire(uint64_t *pages, uint64_t page_count, uint64_t sequence) {
    if (!pages) return;
    if (virtgpu_sequence_done(sequence)) {
        pages_free(pages, page_count);
        return;
    }
    for (;;) {
        for (int index = 0; index < retired_capacity; index++) {
            if (retired[index].pages) continue;
            retired[index].pages = pages;
            retired[index].page_count = page_count;
            retired[index].sequence = sequence;
            retired_count++;
            return;
        }
        if (grow_int_table((void **)&retired, sizeof(*retired), &retired_capacity) != 0) break;
    }
    (void)virtgpu_wait_sequence(sequence);
    pages_free(pages, page_count);
}

static void buffer_release(struct drm_dumb_buffer *buffer) {
    if (!buffer || !buffer->handle) return;
    if (buffer->refs > 1) {
        buffer->refs--;
        return;
    }
    if (render_driver && render_driver->buffer_gone) render_driver->buffer_gone(buffer->handle);
    const struct display_flipper *flipper = display_flipper();
    if (flipper && flipper->forget_pages && buffer->pages) flipper->forget_pages(buffer->pages);
    if (buffer->host_blob && buffer->host_physical) virtgpu_unmap_blob(buffer->virtio_resource);
    uint64_t sequence =
        buffer->virtio_resource ? virtgpu_resource_release(buffer->virtio_resource) : 0;
    pages_retire(buffer->pages, buffer->page_count, sequence);
    memset(buffer, 0, sizeof(*buffer));
}

// The monitor's preferred timing (its EDID's first detailed descriptor), when
// it is the mode on screen.
static const uint8_t *edid_timing(void) {
    const struct display_flipper *flipper = display_flipper();
    const uint8_t *edid = flipper && flipper->edid ? flipper->edid() : NULL;
    if (!edid) return NULL;
    const uint8_t *d = edid + 54;
    uint32_t width = d[2] | ((d[4] & 0xF0U) << 4), height = d[5] | ((d[7] & 0xF0U) << 4);
    if (!(d[0] | d[1]) || width != framebuffer_width() || height != framebuffer_height())
        return NULL;
    return d;
}

static int fill_edid_mode(struct drm_mode_modeinfo *mode) {
    const uint8_t *d = edid_timing();
    if (!d) return 0;
    uint32_t hblank = d[3] | ((d[4] & 0x0FU) << 8), vblank = d[6] | ((d[7] & 0x0FU) << 8);
    uint32_t hfront = d[8] | ((d[11] & 0xC0U) << 2), hsync = d[9] | ((d[11] & 0x30U) << 4);
    uint32_t vfront = (d[10] >> 4) | ((d[11] & 0x0CU) << 2);
    uint32_t vsync = (d[10] & 0x0FU) | ((d[11] & 0x03U) << 4);
    mode->clock = (uint32_t)(d[0] | (d[1] << 8)) * 10U;
    mode->hsync_start = (uint16_t)(mode->hdisplay + hfront);
    mode->hsync_end = (uint16_t)(mode->hsync_start + hsync);
    mode->htotal = (uint16_t)(mode->hdisplay + hblank);
    mode->vsync_start = (uint16_t)(mode->vdisplay + vfront);
    mode->vsync_end = (uint16_t)(mode->vsync_start + vsync);
    mode->vtotal = (uint16_t)(mode->vdisplay + vblank);
    uint64_t pixels = (uint64_t)mode->htotal * mode->vtotal;
    mode->vrefresh = (uint32_t)(((uint64_t)mode->clock * 1000ULL + pixels / 2U) / pixels);
    // Digital separate sync gives each polarity a bit; anything else is negative.
    int separate = (d[17] & 0x18U) == 0x18U;
    mode->flags = (separate && (d[17] & 2U) ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
        (separate && (d[17] & 4U) ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
    return 1;
}

static void fill_mode(struct drm_mode_modeinfo *mode) {
    uint32_t width = framebuffer_width();
    uint32_t height = framebuffer_height();

    memset(mode, 0, sizeof(*mode));
    mode->hdisplay = (uint16_t)width;
    mode->hsync_start = (uint16_t)width;
    mode->hsync_end = (uint16_t)width;
    mode->htotal = (uint16_t)width;
    mode->vdisplay = (uint16_t)height;
    mode->vsync_start = (uint16_t)height;
    mode->vsync_end = (uint16_t)height;
    mode->vtotal = (uint16_t)height;
    mode->vrefresh = 60;
    mode->clock = (uint32_t)(((uint64_t)width * height * 60ULL) / 1000ULL);
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    (void)fill_edid_mode(mode);

    char *out = mode->name;
    size_t limit = sizeof(mode->name) - 1;
    size_t used = 0;
    uint32_t parts[2] = {width, height};
    for (int part = 0; part < 2; part++) {
        char digits[12];
        int count = 0;
        uint32_t value = parts[part];
        if (!value) digits[count++] = '0';
        while (value && count < (int)sizeof(digits)) {
            digits[count++] = (char)('0' + value % 10U);
            value /= 10U;
        }
        while (count-- > 0 && used < limit) out[used++] = digits[count];
        if (part == 0 && used < limit) out[used++] = 'x';
    }
    out[used] = '\0';
}

static int copy_array_out(uint64_t user_pointer, uint32_t user_count, const void *source,
                          size_t item_size, uint32_t count) {
    if (!user_pointer || user_count == 0) return 0;
    if (user_count < count) count = user_count;
    if (!count) return 0;
    return copy_to_user(user_pointer, source, item_size * count) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_version(uint64_t user_argument) {
    struct drm_version version;
    if (copy_from_user(&version, user_argument, sizeof(version)) != 0) return -EFAULT;

    int rendering = virtgpu_virgl_available();
    static const char virtio_name[] = "virtio_gpu";
    static const char plain_name[] = "tunixdrm";
    const char *name = rendering ? virtio_name : plain_name;
    size_t name_size = (rendering ? sizeof(virtio_name) : sizeof(plain_name)) - 1;
    static const char date[] = "20260721";
    const char *desc = "Tunix framebuffer KMS";

    version.version_major = rendering ? 0 : 1;
    version.version_minor = 0;
    version.version_patchlevel = 0;
    if (render_driver) {
        name = render_driver->name;
        name_size = strlen(name);
        desc = render_driver->desc;
        version.version_major = render_driver->major;
        version.version_minor = render_driver->minor;
        version.version_patchlevel = render_driver->patchlevel;
    }

    struct {
        uint64_t pointer;
        uint64_t *length;
        const char *text;
        size_t size;
    } fields[] = {
        {version.name, &version.name_len, name, name_size},
        {version.date, &version.date_len, date, sizeof(date) - 1},
        {version.desc, &version.desc_len, desc, strlen(desc)},
    };
    for (int index = 0; index < 3; index++) {
        uint64_t room = *fields[index].length;
        if (fields[index].pointer && room) {
            size_t amount = fields[index].size < room ? fields[index].size : room;
            if (copy_to_user(fields[index].pointer, fields[index].text, amount) != 0)
                return -EFAULT;
        }
        *fields[index].length = fields[index].size;
    }
    return copy_to_user(user_argument, &version, sizeof(version)) == 0 ? 0 : -EFAULT;
}

#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6

static int64_t ioctl_set_client_cap(uint64_t user_argument) {
    struct drm_set_client_cap {
        uint64_t capability;
        uint64_t value;
    } request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.capability != DRM_CLIENT_CAP_UNIVERSAL_PLANES &&
        request.capability != DRM_CLIENT_CAP_ATOMIC &&
        request.capability != DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT)
        return -EOPNOTSUPP;
    return 0;
}

static uint32_t object_type_of(uint32_t type, uint32_t id) {
    if (type != DRM_MODE_OBJECT_ANY) return type;
    switch (id) {
    case DRM_CRTC_ID:      return DRM_MODE_OBJECT_CRTC;
    case DRM_CONNECTOR_ID: return DRM_MODE_OBJECT_CONNECTOR;
    case DRM_ENCODER_ID:   return DRM_MODE_OBJECT_ENCODER;
    case DRM_PLANE_ID:
    case DRM_CURSOR_ID:    return DRM_MODE_OBJECT_PLANE;
    default:               return DRM_MODE_OBJECT_ANY;
    }
}

static uint32_t connector_dpms;

static int64_t set_connector_property(uint32_t connector_id, uint32_t prop_id, uint64_t value) {
    if (connector_id != DRM_CONNECTOR_ID) return -ENOENT;
    if (prop_id != DRM_PROP_CONNECTOR_DPMS || value > 3U) return -EINVAL;
    connector_dpms = (uint32_t)value;
    return 0;
}

static int64_t ioctl_connector_set_property(uint64_t user_argument) {
    struct {
        uint64_t value;
        uint32_t prop_id;
        uint32_t connector_id;
    } request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    return set_connector_property(request.connector_id, request.prop_id, request.value);
}

static int64_t ioctl_obj_set_property(uint64_t user_argument) {
    struct {
        uint64_t value;
        uint32_t prop_id;
        uint32_t obj_id;
        uint32_t obj_type;
    } request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (object_type_of(request.obj_type, request.obj_id) != DRM_MODE_OBJECT_CONNECTOR)
        return -EINVAL;
    return set_connector_property(request.obj_id, request.prop_id, request.value);
}

static int64_t ioctl_get_cap(uint64_t user_argument) {
    struct drm_get_cap cap;
    if (copy_from_user(&cap, user_argument, sizeof(cap)) != 0) return -EFAULT;
    switch (cap.capability) {
    case DRM_CAP_DUMB_BUFFER:         cap.value = 1; break;
    case DRM_CAP_TIMESTAMP_MONOTONIC: cap.value = 1; break;
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT:       cap.value = 64; break;
    case DRM_CAP_PRIME:            cap.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    case DRM_CAP_ADDFB2_MODIFIERS: cap.value = 1; break;
    case DRM_CAP_CRTC_IN_VBLANK_EVENT: cap.value = 1; break;
    default:                           cap.value = 0; break;
    }
    return copy_to_user(user_argument, &cap, sizeof(cap)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_resources(uint64_t user_argument) {
    struct drm_mode_card_res res;
    if (copy_from_user(&res, user_argument, sizeof(res)) != 0) return -EFAULT;

    uint32_t crtc = DRM_CRTC_ID;
    uint32_t connector = DRM_CONNECTOR_ID;
    uint32_t encoder = DRM_ENCODER_ID;

    if (copy_array_out(res.crtc_id_ptr, res.count_crtcs, &crtc, sizeof(crtc), 1) != 0 ||
        copy_array_out(res.connector_id_ptr, res.count_connectors, &connector, sizeof(connector),
                       1) != 0 ||
        copy_array_out(res.encoder_id_ptr, res.count_encoders, &encoder, sizeof(encoder), 1) != 0)
        return -EFAULT;

    res.count_fbs = 0;
    res.count_crtcs = 1;
    res.count_connectors = 1;
    res.count_encoders = 1;
    res.min_width = framebuffer_width();
    res.max_width = framebuffer_width();
    res.min_height = framebuffer_height();
    res.max_height = framebuffer_height();
    return copy_to_user(user_argument, &res, sizeof(res)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_connector(uint64_t user_argument) {
    struct drm_mode_get_connector connector;
    if (copy_from_user(&connector, user_argument, sizeof(connector)) != 0) return -EFAULT;
    if (connector.connector_id != DRM_CONNECTOR_ID) return -ENOENT;

    struct drm_mode_modeinfo mode;
    fill_mode(&mode);
    uint32_t encoder = DRM_ENCODER_ID;
    uint32_t prop[2] = {DRM_PROP_CONNECTOR_CRTC_ID, DRM_PROP_CONNECTOR_DPMS};
    uint64_t prop_value[2] = {active_fb_id ? DRM_CRTC_ID : 0, connector_dpms};

    if (copy_array_out(connector.modes_ptr, connector.count_modes, &mode, sizeof(mode), 1) != 0 ||
        copy_array_out(connector.encoders_ptr, connector.count_encoders, &encoder, sizeof(encoder),
                       1) != 0 ||
        copy_array_out(connector.props_ptr, connector.count_props, prop, sizeof(prop[0]), 2) != 0 ||
        copy_array_out(connector.prop_values_ptr, connector.count_props, prop_value,
                       sizeof(prop_value[0]), 2) != 0)
        return -EFAULT;

    connector.count_modes = 1;
    connector.count_encoders = 1;
    connector.count_props = 2;
    connector.encoder_id = DRM_ENCODER_ID;
    connector.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    connector.connector_type_id = 1;
    connector.connection = DRM_MODE_CONNECTED;
    const uint8_t *timing = edid_timing();
    connector.mm_width = timing ? timing[12] | ((timing[14] & 0xF0U) << 4) : 0;
    connector.mm_height = timing ? timing[13] | ((timing[14] & 0x0FU) << 8) : 0;
    connector.subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
    return copy_to_user(user_argument, &connector, sizeof(connector)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_encoder(uint64_t user_argument) {
    struct drm_mode_get_encoder encoder;
    if (copy_from_user(&encoder, user_argument, sizeof(encoder)) != 0) return -EFAULT;
    if (encoder.encoder_id != DRM_ENCODER_ID) return -ENOENT;
    encoder.encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    encoder.crtc_id = DRM_CRTC_ID;
    encoder.possible_crtcs = 1;
    encoder.possible_clones = 0;
    return copy_to_user(user_argument, &encoder, sizeof(encoder)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_crtc(uint64_t user_argument) {
    struct drm_mode_crtc crtc;
    if (copy_from_user(&crtc, user_argument, sizeof(crtc)) != 0) return -EFAULT;
    if (crtc.crtc_id != DRM_CRTC_ID) return -ENOENT;
    crtc.fb_id = active_fb_id;
    crtc.x = 0;
    crtc.y = 0;
    crtc.gamma_size = gamma_flipper() ? DRM_GAMMA_SIZE : 0;
    crtc.mode_valid = active_fb_id ? 1 : 0;
    fill_mode(&crtc.mode);
    crtc.count_connectors = 0;
    return copy_to_user(user_argument, &crtc, sizeof(crtc)) == 0 ? 0 : -EFAULT;
}

void drm_buffer_put(uint32_t handle) {
    DRM_LOCKED;
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (buffer) buffer_release(buffer);
}

static int64_t ioctl_prime_handle_to_fd(const struct file *client, uint64_t user_argument) {
    struct drm_prime_handle request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, request.handle);
    if (!buffer) return -ENOENT;
    if (buffer->refs == 0xFFFFFFFFU) return -EMFILE;

    buffer->refs++;
    buffer->shared = 1;
    struct file *file = file_create_dmabuf(request.handle, 0);
    if (!file) {
        buffer_release(buffer);
        return -ENOMEM;
    }
    int fd = process_install_file_flags(process_current(), file, 0,
                                        (request.flags & DRM_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (fd < 0) {
        file_unref(file);
        return -EMFILE;
    }
    request.fd = fd;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int import_remember(const struct file *client, uint32_t handle) {
    for (;;) {
        for (int index = 0; index < import_capacity; index++) {
            if (imports[index].file) continue;
            imports[index].file = client;
            imports[index].handle = handle;
            return 0;
        }
        if (grow_int_table((void **)&imports, sizeof(*imports), &import_capacity) != 0) return -1;
    }
}

static int64_t ioctl_prime_fd_to_handle(const struct file *client, uint64_t user_argument) {
    struct drm_prime_handle request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct file *file = process_file_get(process_current(), request.fd);
    if (!file) return -EBADF;
    int kind = file->kind;
    uint32_t handle = file->dmabuf_handle;
    file_unref(file);
    if (kind != FILE_KIND_DMABUF) return -EINVAL;
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (!buffer) return -ENOENT;
    if (!owns_handle(buffer, client) && !import_find(client, handle)) {
        if (buffer->refs == 0xFFFFFFFFU) return -EMFILE;
        if (import_remember(client, handle) != 0) return -ENOMEM;
        buffer->refs++;
    }

    if (buffer->rendered && buffer->virtio_resource) {
        uint32_t context = client_context(client);
        if (context) (void)virtgpu_context_attach(context, buffer->virtio_resource, 1);
    }

    request.handle = file->dmabuf_handle;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

// Maps LENGTH bytes of BUFFER from OFFSET at VIRTUAL_ADDRESS.
static int64_t map_buffer(struct drm_dumb_buffer *buffer, uint64_t cr3, uint64_t virtual_address,
                          uint64_t length, uint64_t offset, uint64_t page_flags) {
    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX;
    uint64_t mapped = 0;
    if (buffer->device_memory) {
        flags |= PAGE_DEVICE |
            (vmm_write_combining_available() ? PAGE_WRITE_COMBINING : PAGE_UNCACHED);
        for (; mapped < length; mapped += 4096ULL) {
            if (vmm_map_page_in(cr3, virtual_address + mapped,
                                buffer->host_physical + offset + mapped, flags) != 0) {
                while (mapped) {
                    mapped -= 4096ULL;
                    (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                }
                return -EINVAL;
            }
        }
        return 0;
    }
    if (buffer->host_blob) {
        if (!buffer->host_physical) return -EINVAL;
        uint32_t cache = buffer->map_info & VIRTIO_GPU_MAP_CACHE_MASK;
        flags |= PAGE_DEVICE;
        if (cache == VIRTIO_GPU_MAP_CACHE_UNCACHED) flags |= PAGE_UNCACHED;
        if (cache == VIRTIO_GPU_MAP_CACHE_WC)
            flags |= vmm_write_combining_available() ? PAGE_WRITE_COMBINING : PAGE_UNCACHED;
        for (; mapped < length; mapped += 4096ULL) {
            if (vmm_map_page_in(cr3, virtual_address + mapped,
                                buffer->host_physical + offset + mapped, flags) != 0) {
                while (mapped) {
                    mapped -= 4096ULL;
                    (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                }
                return -EINVAL;
            }
        }
        return 0;
    }
    for (; mapped < length; mapped += 4096ULL) {
        uint64_t physical = buffer->pages[(offset + mapped) / 4096ULL];
        if (pmm_page_ref(physical) != 0 ||
            vmm_map_page_in(cr3, virtual_address + mapped, physical, flags) != 0) {
            while (mapped) {
                mapped -= 4096ULL;
                (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                pmm_free_page((void *)buffer->pages[(offset + mapped) / 4096ULL]);
            }
            return -EINVAL;
        }
    }
    return 0;
}

int64_t drm_dmabuf_size(const struct file *file) {
    DRM_LOCKED;
    struct drm_dumb_buffer *buffer = file ? buffer_find(file->dmabuf_handle) : NULL;
    return buffer ? (int64_t)buffer->size : -ENOENT;
}

int64_t drm_dmabuf_mmap(struct file *file, uint64_t cr3, uint64_t virtual_address, uint64_t length,
                        uint64_t offset, uint64_t page_flags) {
    DRM_LOCKED;
    if (!file || !length || (offset & 0xFFFULL)) return -EINVAL;
    struct drm_dumb_buffer *buffer = buffer_find(file->dmabuf_handle);
    if (!buffer) return -ENOENT;
    if (offset >= buffer->size || length > buffer->size - offset) return -EINVAL;
    return map_buffer(buffer, cr3, virtual_address, length, offset, page_flags);
}

static int64_t ioctl_get_plane_resources(uint64_t user_argument) {
    struct drm_mode_get_plane_res res;
    if (copy_from_user(&res, user_argument, sizeof(res)) != 0) return -EFAULT;

    uint32_t planes[2] = {DRM_PLANE_ID, DRM_CURSOR_ID};
    uint32_t count = cursor_available() ? 2U : 1U;
    if (copy_array_out(res.plane_id_ptr, res.count_planes, planes, sizeof(planes[0]), count) != 0)
        return -EFAULT;
    res.count_planes = count;
    return copy_to_user(user_argument, &res, sizeof(res)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_plane(uint64_t user_argument) {
    struct drm_mode_get_plane plane;
    if (copy_from_user(&plane, user_argument, sizeof(plane)) != 0) return -EFAULT;
    int is_cursor = plane.plane_id == DRM_CURSOR_ID && cursor_available();
    if (plane.plane_id != DRM_PLANE_ID && !is_cursor) return -ENOENT;

    uint32_t format = is_cursor ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    if (copy_array_out(plane.format_type_ptr, plane.count_format_types, &format, sizeof(format),
                       1) != 0)
        return -EFAULT;

    uint32_t fb_id = is_cursor ? cursor_fb : active_fb_id;
    plane.crtc_id = fb_id ? DRM_CRTC_ID : 0;
    plane.fb_id = fb_id;
    plane.possible_crtcs = 1U;
    plane.gamma_size = 0;
    plane.count_format_types = 1;
    return copy_to_user(user_argument, &plane, sizeof(plane)) == 0 ? 0 : -EFAULT;
}

static const char *const plane_type_names[] = {"Overlay", "Primary", "Cursor"};

static int64_t ioctl_get_property(uint64_t user_argument) {
    struct drm_mode_get_property property;
    if (copy_from_user(&property, user_argument, sizeof(property)) != 0) return -EFAULT;
    if (property.prop_id == DRM_PROP_CRTC_ACTIVE) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, "ACTIVE", sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = {0, 1};
        if (copy_array_out(property.values_ptr, property.count_values, range, sizeof(range[0]),
                           2) != 0)
            return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_CRTC_MODE_ID ||
        property.prop_id == DRM_PROP_PLANE_FB_DAMAGE_CLIPS) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name,
                property.prop_id == DRM_PROP_CRTC_MODE_ID ? "MODE_ID" : "FB_DAMAGE_CLIPS",
                sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_BLOB;
        property.count_values = 0;
        property.count_enum_blobs = 0;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_PLANE_CRTC_ID || property.prop_id == DRM_PROP_PLANE_FB_ID ||
        property.prop_id == DRM_PROP_CONNECTOR_CRTC_ID) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, property.prop_id == DRM_PROP_PLANE_FB_ID ? "FB_ID" : "CRTC_ID",
                sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC;
        property.count_values = 1;
        property.count_enum_blobs = 0;
        uint64_t object_type[1] = {property.prop_id == DRM_PROP_PLANE_FB_ID ? DRM_MODE_OBJECT_FB
                                                                            : DRM_MODE_OBJECT_CRTC};
        if (copy_array_out(property.values_ptr, property.count_values, object_type,
                           sizeof(object_type[0]), 1) != 0)
            return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id >= DRM_PROP_PLANE_FIRST_RECT &&
        property.prop_id <= DRM_PROP_PLANE_LAST_RECT) {
        static const char *const rectangle_names[] = {
            "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
        };
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, rectangle_names[property.prop_id - DRM_PROP_PLANE_FIRST_RECT],
                sizeof(property.name) - 1);
        int signed_position =
            property.prop_id == DRM_PROP_PLANE_CRTC_X || property.prop_id == DRM_PROP_PLANE_CRTC_Y;
        int crtc_size =
            property.prop_id == DRM_PROP_PLANE_CRTC_W || property.prop_id == DRM_PROP_PLANE_CRTC_H;
        property.flags = signed_position ? DRM_MODE_PROP_SIGNED_RANGE : DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = {signed_position ? (uint64_t)(int64_t)INT32_MIN : 0,
                             signed_position || crtc_size ? (uint64_t)INT32_MAX : 0xFFFFFFFFULL};
        if (copy_array_out(property.values_ptr, property.count_values, range, sizeof(range[0]),
                           2) != 0)
            return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_CONNECTOR_DPMS) {
        static const char *const dpms_names[] = {"On", "Standby", "Suspend", "Off"};
        struct drm_mode_property_enum levels[4];
        memset(levels, 0, sizeof(levels));
        for (unsigned index = 0; index < 4U; index++) {
            levels[index].value = index;
            strncpy(levels[index].name, dpms_names[index], DRM_PROP_NAME_LEN - 1);
        }
        if (copy_array_out(property.enum_blob_ptr, property.count_enum_blobs, levels,
                           sizeof(levels[0]), 4) != 0)
            return -EFAULT;
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, "DPMS", sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_ENUM;
        property.count_values = 0;
        property.count_enum_blobs = 4;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id != DRM_PROP_TYPE_ID) return -ENOENT;

    struct drm_mode_property_enum choices[3];
    memset(choices, 0, sizeof(choices));
    for (unsigned index = 0; index < 3U; index++) {
        choices[index].value = index;
        strncpy(choices[index].name, plane_type_names[index], DRM_PROP_NAME_LEN - 1);
    }
    if (copy_array_out(property.enum_blob_ptr, property.count_enum_blobs, choices,
                       sizeof(choices[0]), 3) != 0)
        return -EFAULT;

    memset(property.name, 0, sizeof(property.name));
    strncpy(property.name, "type", sizeof(property.name) - 1);
    property.flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
    property.count_values = 0;
    property.count_enum_blobs = 3;
    return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_obj_get_properties(uint64_t user_argument) {
    struct drm_mode_obj_get_properties request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    uint32_t type = object_type_of(request.obj_type, request.obj_id);
    if (type == DRM_MODE_OBJECT_ANY) return -ENOENT;

    uint32_t count = 0;
    uint32_t ids[16];
    uint64_t values[16];
    if (type == DRM_MODE_OBJECT_PLANE && request.obj_id == DRM_CURSOR_ID && cursor_available()) {
        static const uint32_t cursor_ids[] = {
            DRM_PROP_TYPE_ID,     DRM_PROP_PLANE_CRTC_ID, DRM_PROP_PLANE_FB_ID,
            DRM_PROP_PLANE_SRC_X, DRM_PROP_PLANE_SRC_Y,   DRM_PROP_PLANE_SRC_W,
            DRM_PROP_PLANE_SRC_H, DRM_PROP_PLANE_CRTC_X,  DRM_PROP_PLANE_CRTC_Y,
            DRM_PROP_PLANE_CRTC_W, DRM_PROP_PLANE_CRTC_H,
        };
        uint64_t cursor_values[] = {
            DRM_PLANE_TYPE_CURSOR, cursor_fb ? DRM_CRTC_ID : 0, cursor_fb, 0, 0,
            (uint64_t)DRM_CURSOR_SIZE << 16, (uint64_t)DRM_CURSOR_SIZE << 16,
            (uint64_t)(int64_t)cursor_x, (uint64_t)(int64_t)cursor_y, DRM_CURSOR_SIZE,
            DRM_CURSOR_SIZE,
        };
        count = sizeof(cursor_ids) / sizeof(cursor_ids[0]);
        memcpy(ids, cursor_ids, sizeof(cursor_ids));
        memcpy(values, cursor_values, sizeof(cursor_values));
    } else if (type == DRM_MODE_OBJECT_PLANE) {
        if (request.obj_id != DRM_PLANE_ID) return -ENOENT;
        ids[0] = DRM_PROP_TYPE_ID;
        values[0] = DRM_PLANE_TYPE_PRIMARY;
        ids[1] = DRM_PROP_PLANE_CRTC_ID;
        values[1] = DRM_CRTC_ID;
        ids[2] = DRM_PROP_PLANE_FB_ID;
        values[2] = active_fb_id;
        ids[3] = DRM_PROP_PLANE_SRC_X;
        values[3] = 0;
        ids[4] = DRM_PROP_PLANE_SRC_Y;
        values[4] = 0;
        ids[5] = DRM_PROP_PLANE_SRC_W;
        values[5] = (uint64_t)framebuffer_width() << 16;
        ids[6] = DRM_PROP_PLANE_SRC_H;
        values[6] = (uint64_t)framebuffer_height() << 16;
        ids[7] = DRM_PROP_PLANE_CRTC_X;
        values[7] = 0;
        ids[8] = DRM_PROP_PLANE_CRTC_Y;
        values[8] = 0;
        ids[9] = DRM_PROP_PLANE_CRTC_W;
        values[9] = framebuffer_width();
        ids[10] = DRM_PROP_PLANE_CRTC_H;
        values[10] = framebuffer_height();
        ids[11] = DRM_PROP_PLANE_FB_DAMAGE_CLIPS;
        values[11] = 0;
        count = 12;
    } else if (type == DRM_MODE_OBJECT_CRTC) {
        if (request.obj_id != DRM_CRTC_ID) return -ENOENT;
        ids[0] = DRM_PROP_CRTC_ACTIVE;
        values[0] = active_fb_id != 0;
        ids[1] = DRM_PROP_CRTC_MODE_ID;
        values[1] = 0;
        count = 2;
    } else if (type == DRM_MODE_OBJECT_CONNECTOR) {
        if (request.obj_id != DRM_CONNECTOR_ID) return -ENOENT;
        ids[0] = DRM_PROP_CONNECTOR_CRTC_ID;
        values[0] = active_fb_id ? DRM_CRTC_ID : 0;
        ids[1] = DRM_PROP_CONNECTOR_DPMS;
        values[1] = connector_dpms;
        count = 2;
    } else if (type == DRM_MODE_OBJECT_ENCODER) {
        if (request.obj_id != DRM_ENCODER_ID) return -ENOENT;
    } else {
        return -EINVAL;
    }

    if (copy_array_out(request.props_ptr, request.count_props, ids, sizeof(ids[0]), count) != 0 ||
        copy_array_out(request.prop_values_ptr, request.count_props, values, sizeof(values[0]),
                       count) != 0)
        return -EFAULT;
    request.count_props = count;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int present_via_virtgpu(const struct drm_framebuffer *fb, struct drm_dumb_buffer *buffer,
                               const struct drm_damage *damage) {
    uint32_t stride_pixels = buffer->pitch / 4U;
    if (!stride_pixels || !buffer->height) return -1;
    if (!buffer->virtio_resource) {
        buffer->virtio_resource = virtgpu_resource_create(stride_pixels, buffer->height,
                                                          buffer->pages, buffer->page_count);
        if (!buffer->virtio_resource) return -1;
    }
    uint32_t width = fb->width < stride_pixels ? fb->width : stride_pixels;
    uint32_t height = fb->height < buffer->height ? fb->height : buffer->height;
    struct virtgpu_rect bounds = {0, 0, 0, 0};
    if (damage && damage->count) {
        int32_t left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
        for (uint32_t index = 0; index < damage->count; index++) {
            const struct drm_mode_rect *rect = &damage->rects[index];
            if (rect->x1 < left) left = rect->x1;
            if (rect->y1 < top) top = rect->y1;
            if (rect->x2 > right) right = rect->x2;
            if (rect->y2 > bottom) bottom = rect->y2;
        }
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > left && bottom > top) {
            bounds.x = (uint32_t)left;
            bounds.y = (uint32_t)top;
            bounds.width = (uint32_t)(right - left);
            bounds.height = (uint32_t)(bottom - top);
        }
    }
    return virtgpu_present(buffer->virtio_resource, stride_pixels, width, height,
                           bounds.width ? &bounds : NULL, !buffer->rendered);
}

static int64_t drm_dispatch_ioctl(struct file *file, unsigned long request, uint64_t user_argument);

static void drm_enter(void) { mutex_lock(&drm_lock); }

static void drm_leave(void) { mutex_unlock(&drm_lock); }

static void copy_row_span(const struct drm_framebuffer *fb, const struct drm_dumb_buffer *buffer,
                          uint8_t *destination, uint32_t row, uint32_t first, uint32_t end) {
    uint64_t source_offset = (uint64_t)row * fb->pitch + first;
    uint64_t page = source_offset / 4096ULL;
    uint64_t within = source_offset % 4096ULL;
    uint32_t copied = first;
    while (copied < end && page < buffer->page_count) {
        uint64_t chunk = 4096ULL - within;
        if (chunk > end - copied) chunk = end - copied;
        memcpy(destination + copied, (uint8_t *)vmm_phys_to_virt(buffer->pages[page]) + within,
               (size_t)chunk);
        copied += (uint32_t)chunk;
        page++;
        within = 0;
    }
}

static int32_t clamp_edge(int32_t value, uint32_t limit) {
    if (value < 0) return 0;
    return (uint32_t)value > limit ? (int32_t)limit : value;
}

#define FLIP_WAIT_NS 50000000ULL

static int present_via_flipper(const struct display_flipper *flipper,
                               const struct drm_framebuffer *fb,
                               const struct drm_dumb_buffer *buffer) {
    uint64_t deadline = time_uptime_ns() + FLIP_WAIT_NS;
    while (!flipper->idle(1)) {
        if (time_uptime_ns() > deadline) return -EBUSY;
        cpu_relax();
    }
    uint32_t screen_height = framebuffer_height();
    uint32_t screen_pitch = framebuffer_pitch();
    uint32_t rows = fb->height < screen_height ? fb->height : screen_height;
    uint32_t row_bytes = fb->pitch < screen_pitch ? fb->pitch : screen_pitch;
    if (flipper->present_pages && buffer->pages &&
        flipper->present_pages(buffer->pages, buffer->page_count, fb->pitch, row_bytes, rows) == 0) {
        scanout_current = 1;
        return 0;
    }
    if (buffer->device_memory) {
        // Device memory has no pages for the CPU's copy below.
        if (!flipper->present_device ||
            flipper->present_device(buffer->host_physical, fb->pitch,
                                    fb->width < framebuffer_width() ? fb->width : framebuffer_width(),
                                    row_bytes, rows) != 0)
            return -EINVAL;
        scanout_current = 1;
        return 0;
    }
    uint8_t *back = flipper->back_buffer();
    if (!back) return -EPERM;
    for (uint32_t row = 0; row < rows; row++) {
        copy_row_span(fb, buffer, back + (uint64_t)row * screen_pitch, row, 0, row_bytes);
        smp_service_flush();
    }
    framebuffer_present();
    if (flipper->flip_to_back() != 0) return -EIO;
    scanout_current = 1;
    return 0;
}

static int present_framebuffer(const struct file *client, uint32_t fb_id,
                               const struct drm_damage *damage) {
    struct drm_framebuffer *fb = client ? framebuffer_of(client, fb_id) : framebuffer_find(fb_id);
    if (!fb) return -ENOENT;
    struct drm_dumb_buffer *buffer = buffer_find(fb->handle);
    if (!buffer) return -ENOENT;

    int status = framebuffer_claim_graphics(&drm_display_owner);
    if (status != 0) return status;

    if (!framebuffer_graphics_foreground(&drm_display_owner)) {
        scanout_current = 0;
        return 0;
    }

    if (virtgpu_available()) {
        static uint32_t last_virtgpu_fb;
        const struct drm_damage *partial =
            scanout_current && last_virtgpu_fb == fb_id ? damage : NULL;
        if (present_via_virtgpu(fb, buffer, partial) == 0) {
            last_virtgpu_fb = fb_id;
            scanout_current = 1;
            return 0;
        }
    }

    const struct display_flipper *flipper = display_flipper();
    if (flipper) return present_via_flipper(flipper, fb, buffer);

    uint8_t *scanout = framebuffer_scanout();
    if (!scanout) return -EPERM;

    uint32_t screen_height = framebuffer_height();
    uint32_t screen_pitch = framebuffer_pitch();
    uint32_t rows = fb->height < screen_height ? fb->height : screen_height;
    uint32_t row_bytes = fb->pitch < screen_pitch ? fb->pitch : screen_pitch;
    uint32_t columns = row_bytes / 4U;
    struct drm_damage whole = {1, {{0, 0, (int32_t)columns, (int32_t)rows}}};
    if (!damage || !scanout_current) damage = &whole;

    for (uint32_t index = 0; index < damage->count; index++) {
        const struct drm_mode_rect *rect = &damage->rects[index];
        uint32_t top = (uint32_t)clamp_edge(rect->y1, rows);
        uint32_t bottom = (uint32_t)clamp_edge(rect->y2, rows);
        uint32_t first = (uint32_t)clamp_edge(rect->x1, columns) * 4U;
        uint32_t end = (uint32_t)clamp_edge(rect->x2, columns) * 4U;
        if (rect == &whole.rects[0]) end = row_bytes;
        if (first >= end) continue;
        for (uint32_t row = top; row < bottom; row++) {
            copy_row_span(fb, buffer, scanout + (uint64_t)row * screen_pitch, row, first, end);
            smp_service_flush();
        }
    }
    framebuffer_present();
    scanout_current = 1;
    return 0;
}

static int64_t ioctl_set_crtc(const struct file *client, uint64_t user_argument) {
    struct drm_mode_crtc crtc;
    if (copy_from_user(&crtc, user_argument, sizeof(crtc)) != 0) return -EFAULT;
    if (crtc.crtc_id != DRM_CRTC_ID) return -ENOENT;

    if (!crtc.fb_id) {
        active_fb_id = 0;
        virtgpu_scanout_disable();
        scanout_current = 0;
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    }
    int status = present_framebuffer(client, crtc.fb_id, NULL);
    if (status != 0) return status;
    active_fb_id = crtc.fb_id;
    return 0;
}

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

static int64_t ioctl_dirty_fb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_dirty_cmd cmd;
    if (copy_from_user(&cmd, user_argument, sizeof(cmd)) != 0) return -EFAULT;
    return present_framebuffer(client, cmd.fb_id, NULL);
}

static struct drm_event_queue *event_queue_of(const struct file *client, int create) {
    struct drm_event_queue *spare = NULL;
    for (unsigned index = 0; index < DRM_EVENT_QUEUES; index++) {
        if (event_queues[index].owner == client) return &event_queues[index];
        if (!spare && !event_queues[index].owner) spare = &event_queues[index];
    }
    if (!create || !spare) return NULL;
    spare->owner = client;
    spare->head = spare->tail = spare->count = 0;
    return spare;
}

#define FLIP_FENCE_PATIENCE_NS 100000000ULL

static int hardware_vblank(uint64_t *sequence, uint64_t *time_ns) {
    const struct display_flipper *flipper = display_flipper();
    uint64_t count = 0, at = 0;
    if (!flipper || !flipper->vblank || flipper->vblank(&count, &at) != 0) return 0;
    if (sequence) *sequence = count;
    if (time_ns) *time_ns = at;
    return 1;
}

static void queue_flip_event(const struct file *client, uint64_t user_data) {
    struct drm_event_queue *queue = event_queue_of(client, 1);
    if (!queue) return;
    if (queue->count == DRM_MAX_EVENTS) {
        queue->head = (queue->head + 1U) % DRM_MAX_EVENTS;
        queue->count--;
    }
    struct drm_event_vblank *event = &queue->events[queue->tail];
    memset(event, 0, sizeof(*event));
    event->base.type = DRM_EVENT_FLIP_COMPLETE;
    event->base.length = sizeof(*event);
    event->user_data = user_data;
    ++flip_sequence;
    event->crtc_id = DRM_CRTC_ID;
    uint64_t now = time_uptime_ns();
    uint64_t due = (now / DRM_VBLANK_NS + 1U) * DRM_VBLANK_NS;
    if (hardware_vblank(NULL, NULL)) due = now;
    queue->fences[queue->tail] = virtgpu_flip_fence();
    queue->due_ns[queue->tail] = due;
    queue->tail = (queue->tail + 1U) % DRM_MAX_EVENTS;
    timer_note_deadline(due == now ? now + FLIP_FENCE_PATIENCE_NS : due);
    __atomic_add_fetch(&queue->count, 1, __ATOMIC_RELEASE);
    (void)process_wake_io();
}

static int flip_head_ready(const struct drm_event_queue *queue, int reclaim) {
    if (!__atomic_load_n(&queue->count, __ATOMIC_ACQUIRE)) return 0;
    uint64_t now = time_uptime_ns();
    uint64_t due = queue->due_ns[queue->head];
    if (now < due) return 0;
    const struct display_flipper *flipper = display_flipper();
    if (flipper && !flipper->idle(reclaim)) {
        if (now - due > FLIP_FENCE_PATIENCE_NS) return 1;
        // A vblank interrupt may be late or missing, so the waiter looks
        // again a millisecond later rather than trusting it to wake it.
        timer_note_deadline(now + 1000000ULL);
        return 0;
    }
    uint64_t fence = queue->fences[queue->head];
    if (fence <= virtgpu_completed()) return 1;
    if (reclaim && virtgpu_sequence_done(fence)) return 1;
    return now - due > FLIP_FENCE_PATIENCE_NS;
}

int64_t drm_file_read(struct file *file, size_t size, void *buffer) {
    DRM_LOCKED;
    if (!buffer) return -EINVAL;
    struct drm_event_queue *queue = event_queue_of(file, 0);
    size_t produced = 0;
    uint8_t *out = (uint8_t *)buffer;
    while (queue && flip_head_ready(queue, 1) &&
           size - produced >= sizeof(struct drm_event_vblank)) {
        struct drm_event_vblank *event = &queue->events[queue->head];
        uint64_t vblank = time_uptime_ns() / DRM_VBLANK_NS;
        uint64_t at = vblank * DRM_VBLANK_NS;
        (void)hardware_vblank(&vblank, &at);
        event->tv_sec = (uint32_t)(at / 1000000000ULL);
        event->tv_usec = (uint32_t)((at % 1000000000ULL) / 1000ULL);
        event->sequence = (uint32_t)vblank;
        memcpy(out + produced, event, sizeof(struct drm_event_vblank));
        queue->head = (queue->head + 1U) % DRM_MAX_EVENTS;
        queue->count--;
        produced += sizeof(struct drm_event_vblank);
    }
    if (!produced) return -EAGAIN;
    return (int64_t)produced;
}

int drm_file_read_ready(struct file *file) {
    for (unsigned index = 0; index < DRM_EVENT_QUEUES; index++)
        if (event_queues[index].owner == file) return flip_head_ready(&event_queues[index], 0);
    return 0;
}

int64_t drm_device_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    return -EAGAIN;
}

int drm_device_read_ready(struct vfs_node *node) {
    (void)node;
    return 0;
}

static int64_t ioctl_page_flip(const struct file *client, uint64_t user_argument) {
    struct drm_mode_crtc_page_flip flip;
    if (copy_from_user(&flip, user_argument, sizeof(flip)) != 0) return -EFAULT;
    if (flip.crtc_id != DRM_CRTC_ID) return -ENOENT;
    int status = present_framebuffer(client, flip.fb_id, NULL);
    if (status != 0) return status;
    active_fb_id = flip.fb_id;
    if (flip.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(client, flip.user_data);
    return 0;
}

static int64_t ioctl_create_dumb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_create_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (!request.width || !request.height || request.bpp != 32) return -EINVAL;
    if (request.width > DRM_MAX_DIMENSION || request.height > DRM_MAX_DIMENSION) return -EINVAL;

    uint64_t pitch = (uint64_t)request.width * 4ULL;
    uint64_t size = pitch * request.height;
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (1024ULL * 1024ULL * 1024ULL) / 4096ULL) return -EINVAL;

    uint32_t handle = 0;
    struct drm_dumb_buffer *slot = buffer_slot(&handle);
    if (!slot) return -ENOMEM;

    slot->pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!slot->pages) return -ENOMEM;
    memset(slot->pages, 0, page_count * sizeof(uint64_t));

    for (uint64_t index = 0; index < page_count; index++) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) {
            slot->handle = 1;
            slot->page_count = index;
            buffer_release(slot);
            return -ENOMEM;
        }
        memset(vmm_phys_to_virt(physical), 0, 4096);
        slot->pages[index] = physical;
    }

    slot->handle = handle;
    slot->owner = client;
    slot->refs = 1;
    slot->width = request.width;
    slot->height = request.height;
    slot->pitch = (uint32_t)pitch;
    slot->size = size;
    slot->page_count = page_count;

    request.handle = slot->handle;
    request.pitch = (uint32_t)pitch;
    request.size = size;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_map_dumb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_map_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, request.handle);
    if (!buffer) return -ENOENT;
    request.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t handle_close(const struct file *client, uint32_t handle) {
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (!buffer) return -ENOENT;
    struct drm_import *import = import_find(client, handle);
    if (import) {
        memset(import, 0, sizeof(*import));
    } else if (owns_handle(buffer, client)) {
        buffer->owner_closed = 1;
    } else {
        return -ENOENT;
    }
    buffer_release(buffer);
    return 0;
}

static int64_t ioctl_destroy_dumb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_destroy_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    return handle_close(client, request.handle);
}

static int64_t ioctl_add_framebuffer(const struct file *client, uint32_t handle, uint32_t width,
                                     uint32_t height, uint32_t pitch, uint32_t *fb_id_out) {
    struct drm_dumb_buffer *buffer = buffer_of(client, handle);
    if (!buffer) return -ENOENT;
    struct drm_framebuffer *slot = NULL;
    for (int index = 0; index < framebuffer_capacity; index++) {
        if (!framebuffers[index].id) {
            slot = &framebuffers[index];
            break;
        }
    }
    if (!slot) {
        int first = framebuffer_capacity;
        if (grow_int_table((void **)&framebuffers, sizeof(*framebuffers), &framebuffer_capacity) !=
            0)
            return -ENOMEM;
        slot = &framebuffers[first];
    }
    if (buffer->refs == 0xFFFFFFFFU) return -EMFILE;
    buffer->refs++;
    slot->id = next_fb_id++;
    slot->owner = client;
    slot->handle = handle;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch ? pitch : buffer->pitch;
    *fb_id_out = slot->id;
    return 0;
}

static int64_t ioctl_addfb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_cmd request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    int64_t status = ioctl_add_framebuffer(client, request.handle, request.width, request.height,
                                           request.pitch, &request.fb_id);
    if (status != 0) return status;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_addfb2(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_cmd2 request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.handles[1] || request.handles[2] || request.handles[3]) return -EINVAL;
    int64_t status = ioctl_add_framebuffer(client, request.handles[0], request.width,
                                           request.height, request.pitches[0], &request.fb_id);
    if (status != 0) return status;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_rmfb(const struct file *client, uint64_t user_argument) {
    uint32_t fb_id;
    if (copy_from_user(&fb_id, user_argument, sizeof(fb_id)) != 0) return -EFAULT;
    struct drm_framebuffer *fb = framebuffer_of(client, fb_id);
    if (!fb) return -ENOENT;
    if (active_fb_id == fb_id) active_fb_id = 0;
    struct drm_dumb_buffer *buffer = buffer_find(fb->handle);
    memset(fb, 0, sizeof(*fb));
    if (buffer) buffer_release(buffer);
    return 0;
}

static int64_t ioctl_gem_close(const struct file *client, uint64_t user_argument) {
    struct drm_gem_close request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    return handle_close(client, request.handle);
}

// A context a client made for itself with CONTEXT_INIT -- Venus's, for Vulkan --
// rather than the per-process virgl one render_context() gives the others.
#define FILE_CONTEXTS 64U

static struct {
    const struct file *file;
    uint32_t context;
    uint32_t rings;
} file_contexts[FILE_CONTEXTS];

static int file_context_index(const struct file *client) {
    for (unsigned index = 0; index < FILE_CONTEXTS; index++)
        if (file_contexts[index].context && file_contexts[index].file == client) return (int)index;
    return -1;
}

static uint32_t render_context(void);

static uint32_t client_context(const struct file *client) {
    int index = file_context_index(client);
    return index >= 0 ? file_contexts[index].context : render_context();
}

static void file_context_release(const struct file *client) {
    int index = file_context_index(client);
    if (index < 0) return;
    virtgpu_context_destroy(file_contexts[index].context);
    memset(&file_contexts[index], 0, sizeof(file_contexts[index]));
}

static uint32_t render_context(void) {
    if (!virtgpu_virgl_available()) return 0;
    struct process *process = process_current();
    if (!process) return 0;

    for (int index = 0; index < context_capacity; index++) {
        if (render_contexts[index].context && render_contexts[index].pid == process->pid)
            return render_contexts[index].context;
    }
    for (int index = 0; index < context_capacity; index++) {
        if (render_contexts[index].context) continue;
        uint32_t context = next_render_context;
        if (virtgpu_context_create(context, "tunix") != 0) return 0;
        next_render_context++;
        render_contexts[index].pid = process->pid;
        render_contexts[index].context = context;
        return context;
    }
    int first = context_capacity;
    if (grow_int_table((void **)&render_contexts, sizeof(*render_contexts), &context_capacity) != 0)
        return 0;
    uint32_t context = next_render_context;
    if (virtgpu_context_create(context, "tunix") != 0) return 0;
    next_render_context++;
    render_contexts[first].pid = process->pid;
    render_contexts[first].context = context;
    return context;
}

static void render_contexts_release(void) {
    for (int index = 0; index < context_capacity; index++) {
        if (!render_contexts[index].context) continue;
        virtgpu_context_destroy(render_contexts[index].context);
        render_contexts[index].context = 0;
        render_contexts[index].pid = 0;
    }
}

static struct drm_dumb_buffer *buffer_new(const struct file *client, uint64_t size) {
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (1024ULL * 1024ULL * 1024ULL) / 4096ULL) return NULL;

    uint32_t handle = 0;
    struct drm_dumb_buffer *slot = buffer_slot(&handle);
    if (!slot) return NULL;

    slot->pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!slot->pages) return NULL;
    memset(slot->pages, 0, page_count * sizeof(uint64_t));

    for (uint64_t index = 0; index < page_count; index++) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) {
            slot->handle = 1;
            slot->page_count = index;
            buffer_release(slot);
            return NULL;
        }
        memset(vmm_phys_to_virt(physical), 0, 4096);
        slot->pages[index] = physical;
    }

    slot->handle = handle;
    slot->owner = client;
    slot->refs = 1;
    slot->size = size;
    slot->page_count = page_count;
    return slot;
}

static int64_t ioctl_virtgpu_getparam(uint64_t user_argument) {
    struct drm_virtgpu_getparam query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;

    if (!query.value) return -EINVAL;

    uint32_t answer;
    switch (query.param) {
    case VIRTGPU_PARAM_3D_FEATURES:
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX: answer = 1; break;
    case VIRTGPU_PARAM_RESOURCE_BLOB:
        answer = (virtgpu_features() & VIRTGPU_FEATURE_BLOB) != 0;
        break;
    case VIRTGPU_PARAM_HOST_VISIBLE:
        answer = (virtgpu_features() & VIRTGPU_FEATURE_HOST_VISIBLE) != 0;
        break;
    case VIRTGPU_PARAM_CONTEXT_INIT:
        answer = (virtgpu_features() & VIRTGPU_FEATURE_CONTEXT_INIT) != 0;
        break;
    case VIRTGPU_PARAM_CROSS_DEVICE: answer = 0; break;
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS: answer = virtgpu_capset_mask(); break;
    default: return -EINVAL;
    }
    return copy_to_user(query.value, &answer, sizeof(answer)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_get_caps(uint64_t user_argument) {
    struct drm_virtgpu_get_caps query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.addr || !query.size) return -EINVAL;
    uint32_t size = 0;
    if (virtgpu_capset_info(query.cap_set_id, NULL, &size) != 0) return -EINVAL;
    if (query.size < size) size = query.size;
    if (!size) return -ENODEV;

    void *capset = kmalloc(size);
    if (!capset) return -ENOMEM;
    if (virtgpu_get_capset(query.cap_set_id, query.cap_set_ver, capset, size) != 0) {
        kfree(capset);
        return -EIO;
    }
    int copied = copy_to_user(query.addr, capset, size) == 0;
    kfree(capset);
    return copied ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_resource_create(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_resource_create query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.size) return -EINVAL;

    uint32_t context = render_context();
    if (!context) return -ENODEV;

    struct drm_dumb_buffer *buffer = buffer_new(client, query.size);
    if (!buffer) return -ENOMEM;

    struct virtgpu_resource_3d spec;
    memset(&spec, 0, sizeof(spec));
    spec.target = query.target;
    spec.format = query.format;
    spec.bind = query.bind;
    spec.width = query.width;
    spec.height = query.height;
    spec.depth = query.depth;
    spec.array_size = query.array_size;
    spec.last_level = query.last_level;
    spec.nr_samples = query.nr_samples;
    spec.flags = query.flags;

    uint32_t resource =
        virtgpu_resource_create_3d(&spec, buffer->pages, buffer->page_count, query.size);
    if (!resource) {
        buffer_release(buffer);
        return -ENOMEM;
    }
    if (virtgpu_context_attach(context, resource, 1) != 0) {
        buffer->virtio_resource = resource;
        buffer_release(buffer);
        return -EIO;
    }

    buffer->virtio_resource = resource;
    buffer->rendered = 1;
    buffer->width = query.width;
    buffer->height = query.height;
    buffer->pitch = query.stride;

    query.bo_handle = buffer->handle;
    query.res_handle = resource;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_context_init(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_context_init query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!(virtgpu_features() & VIRTGPU_FEATURE_CONTEXT_INIT)) return -EINVAL;
    if (!query.num_params || query.num_params > 8) return -EINVAL;
    if (file_context_index(client) >= 0) return -EEXIST;

    uint32_t capset = 0, rings = 0;
    char name[64] = "tunix";
    for (uint32_t index = 0; index < query.num_params; index++) {
        struct drm_virtgpu_context_set_param param;
        if (copy_from_user(&param, query.ctx_set_params + index * sizeof(param), sizeof(param)) != 0)
            return -EFAULT;
        switch (param.param) {
        case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
            if (param.value >= 32 || !(virtgpu_capset_mask() & (1U << param.value))) return -EINVAL;
            capset = (uint32_t)param.value;
            break;
        case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
            if (param.value > 64) return -EINVAL;
            rings = (uint32_t)param.value;
            break;
        case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK: break;
        case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
            if (copy_from_user(name, param.value, sizeof(name) - 1) != 0) return -EFAULT;
            name[sizeof(name) - 1] = '\0';
            break;
        default: return -EINVAL;
        }
    }
    if (!capset) return -EINVAL;

    for (unsigned index = 0; index < FILE_CONTEXTS; index++) {
        if (file_contexts[index].context) continue;
        uint32_t context = next_render_context;
        if (virtgpu_context_create_init(context, name, capset) != 0) return -EIO;
        next_render_context++;
        file_contexts[index].file = client;
        file_contexts[index].context = context;
        file_contexts[index].rings = rings;
        return 0;
    }
    return -ENOMEM;
}

static int64_t ioctl_virtgpu_resource_create_blob(const struct file *client,
                                                  uint64_t user_argument) {
    struct drm_virtgpu_resource_create_blob query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!(virtgpu_features() & VIRTGPU_FEATURE_BLOB) || !query.size) return -EINVAL;
    if (query.blob_mem < VIRTGPU_BLOB_MEM_GUEST || query.blob_mem > VIRTGPU_BLOB_MEM_HOST3D_GUEST)
        return -EINVAL;
    uint64_t size = (query.size + 4095ULL) & ~4095ULL;
    uint32_t context = client_context(client);
    if (query.blob_mem != VIRTGPU_BLOB_MEM_GUEST && !context) return -EINVAL;

    // Commands that make the host object the blob names, ahead of it.
    if (query.cmd_size) {
        if (query.cmd_size > DRM_MAX_COMMAND_BYTES) return -EINVAL;
        void *staging = kmalloc(query.cmd_size);
        if (!staging) return -ENOMEM;
        if (copy_from_user(staging, query.cmd, query.cmd_size) != 0) {
            kfree(staging);
            return -EFAULT;
        }
        int submitted = virtgpu_submit_3d(context, staging, query.cmd_size);
        kfree(staging);
        if (submitted != 0) return -EIO;
    }

    struct drm_dumb_buffer *buffer = NULL;
    if (query.blob_mem == VIRTGPU_BLOB_MEM_HOST3D) {
        uint32_t handle = 0;
        buffer = buffer_slot(&handle);
        if (!buffer) return -ENOMEM;
        buffer->handle = handle;
        buffer->owner = client;
        buffer->refs = 1;
        buffer->size = size;
        buffer->host_blob = 1;
    } else {
        buffer = buffer_new(client, size);
        if (!buffer) return -ENOMEM;
    }
    buffer->blob_mem = query.blob_mem;

    uint32_t resource = virtgpu_resource_create_blob(context, query.blob_mem, query.blob_flags,
                                                     query.blob_id, size, buffer->pages,
                                                     buffer->page_count);
    if (!resource) {
        buffer_release(buffer);
        return -EIO;
    }
    buffer->virtio_resource = resource;
    buffer->rendered = 1;
    if (context) (void)virtgpu_context_attach(context, resource, 1);

    if (buffer->host_blob && (query.blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) &&
        virtgpu_map_blob(resource, size, &buffer->host_physical, &buffer->map_info) != 0) {
        buffer_release(buffer);
        return -ENOMEM;
    }

    query.bo_handle = buffer->handle;
    query.res_handle = resource;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_resource_info(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_resource_info query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.bo_handle);
    if (!buffer) return -ENOENT;

    query.res_handle = buffer->virtio_resource;
    query.size = (uint32_t)buffer->size;
    query.blob_mem = buffer->blob_mem;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_map(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_map query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.handle);
    if (!buffer) return -ENOENT;
    query.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_transfer(const struct file *client, uint64_t user_argument,
                                      int to_host) {
    struct drm_virtgpu_3d_transfer query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.bo_handle);
    if (!buffer || !buffer->virtio_resource) return -ENOENT;

    struct virtgpu_box box;
    box.x = query.box.x;
    box.y = query.box.y;
    box.z = query.box.z;
    box.w = query.box.w;
    box.h = query.box.h;
    box.d = query.box.d;
    if (virtgpu_transfer_3d(render_context(), buffer->virtio_resource, &box, query.offset,
                            query.level, query.stride, query.layer_stride, to_host) != 0)
        return -EIO;
    buffer->busy_until = virtgpu_posted();
    return 0;
}

#define VIRTGPU_WAIT_NOWAIT   0x1U

static void mark_busy(const struct file *client, uint64_t user_handles, uint32_t count,
                      uint64_t sequence) {
    if (!count || !user_handles) {
        untracked_busy_until = sequence;
        return;
    }
    uint32_t handles[64];
    for (uint32_t done = 0; done < count;) {
        uint32_t chunk = count - done < 64U ? count - done : 64U;
        if (copy_from_user(handles, user_handles + (uint64_t)done * sizeof(handles[0]),
                           chunk * sizeof(handles[0])) != 0) {
            untracked_busy_until = sequence;
            return;
        }
        for (uint32_t index = 0; index < chunk; index++) {
            struct drm_dumb_buffer *buffer = buffer_of(client, handles[index]);
            if (buffer) buffer->busy_until = sequence;
        }
        done += chunk;
    }
}

static int64_t ioctl_virtgpu_execbuffer(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_execbuffer query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.command || !query.size) return -EINVAL;
    if (query.size > DRM_MAX_COMMAND_BYTES) return -EINVAL;

    if (query.flags & ~(VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT |
                        VIRTGPU_EXECBUF_RING_IDX))
        return -EINVAL;
    if (query.num_in_syncobjs || query.num_out_syncobjs) return -EINVAL;

    uint32_t context = client_context(client);
    if (!context) return -ENODEV;
    int slot = file_context_index(client);
    if ((query.flags & VIRTGPU_EXECBUF_RING_IDX) &&
        (slot < 0 || query.ring_idx >= file_contexts[slot].rings))
        return -EINVAL;

    // A fence to wait for first: one of ours, from an earlier submission.
    if (query.flags & VIRTGPU_EXECBUF_FENCE_FD_IN) {
        struct file *fence = process_file_get(process_current(), query.fence_fd);
        if (!fence) return -EBADF;
        uint64_t sequence = fence->kind == FILE_KIND_SYNC_FILE ? fence->fence_sequence : 0;
        int kind = fence->kind;
        file_unref(fence);
        if (kind != FILE_KIND_SYNC_FILE) return -EINVAL;
        if (!virtgpu_fence_signaled(sequence)) {
            drm_leave();
            int waited = virtgpu_wait_sequence(sequence);
            drm_enter();
            if (waited != 0) return -EBUSY;
        }
    }

    void *staging = kmalloc(query.size);
    if (!staging) return -ENOMEM;
    if (copy_from_user(staging, query.command, query.size) != 0) {
        kfree(staging);
        return -EFAULT;
    }
    uint64_t sequence = 0;
    int fenced = (query.flags & (VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT)) != 0;
    if (fenced) {
        sequence = virtgpu_submit_fenced(context, (query.flags & VIRTGPU_EXECBUF_RING_IDX)
                                                      ? query.ring_idx : 0,
                                         staging, query.size);
        kfree(staging);
        if (!sequence) return -EIO;
    } else {
        int submitted = virtgpu_submit_3d(context, staging, query.size);
        kfree(staging);
        if (submitted != 0) return -EIO;
        sequence = virtgpu_posted();
    }
    mark_busy(client, query.bo_handles, query.num_bo_handles, sequence);

    query.fence_fd = -1;
    if (query.flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
        struct file *fence = file_create_sync_file(sequence, 0);
        if (!fence) return -ENOMEM;
        int fd = process_install_file_flags(process_current(), fence, 0, PROCESS_FD_CLOEXEC);
        if (fd < 0) {
            file_unref(fence);
            return fd;
        }
        query.fence_fd = fd;
    }
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_wait(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_3d_wait query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.handle);
    if (!buffer) return -ENOENT;
    uint64_t target =
        buffer->busy_until > untracked_busy_until ? buffer->busy_until : untracked_busy_until;
    if (query.flags & VIRTGPU_WAIT_NOWAIT) return virtgpu_sequence_done(target) ? 0 : -EBUSY;
    if (virtgpu_sequence_done(target)) return 0;
    drm_leave();
    int status = virtgpu_wait_sequence(target);
    drm_enter();
    return status == 0 ? 0 : -EBUSY;
}

int64_t drm_file_ioctl(struct file *file, unsigned long request, uint64_t user_argument) {
    if (!drm_ready) return -ENOTTY;
    if (IOCTL_TYPE(request) != (unsigned)DRM_IOCTL_TYPE) return -ENOTTY;
    // drmtrace on the command line: every failing or slow ioctl, and the
    // first few hundred of each process, with who asked and how long it took.
    static int trace = -1;
    static unsigned traced;
    if (trace < 0) trace = boot_command_line_flag("drmtrace");
    uint64_t start = trace ? time_uptime_ns() : 0;
    drm_enter();
    int64_t answer = drm_dispatch_ioctl(file, request, user_argument);
    drm_leave();
    if (trace) {
        uint64_t took = (time_uptime_ns() - start) / 1000U;
        // The budget starts again for each new process, so a later one is seen too.
        static uint64_t traced_pid;
        if (process_current_pid() != traced_pid) {
            traced_pid = process_current_pid();
            traced = 0;
        }
        if (traced < 400U || answer < 0 || took > 5000U) {
            traced++;
            kprintf("drm: pid %u ioctl %x size %u -> %d in %u us\n",
                    (unsigned)process_current_pid(), (unsigned)IOCTL_NR(request),
                    (unsigned)((request >> 16) & 0x3FFFU), (int)answer, (unsigned)took);
        }
    }
    return answer;
}

static int dropped_master(const struct file *file) {
    for (unsigned index = 0; index < DROPPED_MASTERS; index++)
        if (dropped_masters[index] == file) return 1;
    return 0;
}

static void note_dropped_master(const struct file *file) {
    if (dropped_master(file)) return;
    for (unsigned index = 0; index < DROPPED_MASTERS; index++) {
        if (dropped_masters[index]) continue;
        dropped_masters[index] = file;
        return;
    }
}

static void forget_master(const struct file *file) {
    for (unsigned index = 0; index < DROPPED_MASTERS; index++)
        if (dropped_masters[index] == file) dropped_masters[index] = NULL;
    if (master_file == file) master_file = NULL;
}

static int64_t require_master(const struct file *file) {
    if (master_file == file) return 0;
    if (master_file || dropped_master(file)) return -EACCES;
    master_file = file;
    return 0;
}

static int64_t ioctl_set_master(const struct file *file) {
    if (!cred_is_root()) return -EPERM;
    if (master_file && master_file != file) return -EBUSY;
    for (unsigned index = 0; index < DROPPED_MASTERS; index++)
        if (dropped_masters[index] == file) dropped_masters[index] = NULL;
    master_file = file;
    if (framebuffer_claim_graphics(&drm_display_owner) == 0) {
        framebuffer_resume_graphics();
        vt_display_claimed();
    }
    return 0;
}

static int64_t ioctl_drop_master(const struct file *file) {
    if (!cred_is_root()) return -EPERM;
    if (master_file != file) return -EINVAL;
    master_file = NULL;
    note_dropped_master(file);
    if (cursor_available()) (void)cursor_show(NULL, 0, 0, 0, 0);
    cursor_fb = 0;
    if (gamma_loaded && gamma_flipper()) (void)gamma_flipper()->gamma_set(NULL, NULL, NULL);
    gamma_loaded = 0;
    active_fb_id = 0;
    virtgpu_scanout_disable();
    scanout_current = 0;
    (void)framebuffer_release_graphics(&drm_display_owner, 0);
    return 0;
}

static int64_t ioctl_get_magic(uint64_t user_argument) {
    uint32_t magic = 1;
    return copy_to_user(user_argument, &magic, sizeof(magic)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_auth_magic(const struct file *file, uint64_t user_argument) {
    uint32_t magic;
    if (copy_from_user(&magic, user_argument, sizeof(magic)) != 0) return -EFAULT;
    if (require_master(file) != 0) return -EACCES;
    return magic ? 0 : -EINVAL;
}

static int modesetting_request(unsigned nr) {
    switch (nr) {
    case DRM_NR_MODE_SETCRTC:
    case DRM_NR_MODE_CURSOR:
    case DRM_NR_MODE_CURSOR2:
    case DRM_NR_MODE_SETGAMMA:
    case DRM_NR_MODE_ATOMIC:
    case DRM_NR_MODE_PAGE_FLIP:
    case DRM_NR_MODE_DIRTYFB:
    case DRM_NR_MODE_SETPROPERTY:
    case DRM_NR_MODE_OBJ_SETPROPERTY: return 1;
    default:                          return 0;
    }
}

static int64_t drm_dispatch_ioctl(struct file *file, unsigned long request,
                                  uint64_t user_argument) {
    retired_reap();
    // Driver ioctls are 0x40..0x9F; the mode-setting ones start at 0xA0.
    if (render_driver && IOCTL_NR(request) >= DRM_COMMAND_BASE && IOCTL_NR(request) < 0xA0U)
        return render_driver->ioctl(file, IOCTL_NR(request) - DRM_COMMAND_BASE,
                                    (unsigned)((request >> 16) & 0x3FFFU), user_argument);
    if (modesetting_request(IOCTL_NR(request))) {
        int64_t status = require_master(file);
        if (status != 0) return status;
    }
    switch (IOCTL_NR(request)) {
    case DRM_NR_VERSION:                return ioctl_version(user_argument);
    case DRM_NR_GET_CAP:                return ioctl_get_cap(user_argument);
    case DRM_NR_SET_CLIENT_CAP:         return ioctl_set_client_cap(user_argument);
    case DRM_NR_SET_VERSION:            return 0;
    case DRM_NR_DROP_MASTER:            return ioctl_drop_master(file);
    case DRM_NR_SET_MASTER:             return ioctl_set_master(file);
    case DRM_NR_GET_MAGIC:              return ioctl_get_magic(user_argument);
    case DRM_NR_AUTH_MAGIC:             return ioctl_auth_magic(file, user_argument);
    case DRM_NR_PRIME_HANDLE_TO_FD:     return ioctl_prime_handle_to_fd(file, user_argument);
    case DRM_NR_PRIME_FD_TO_HANDLE:     return ioctl_prime_fd_to_handle(file, user_argument);
    case DRM_NR_MODE_GETPLANERESOURCES: return ioctl_get_plane_resources(user_argument);
    case DRM_NR_MODE_GETPLANE:          return ioctl_get_plane(user_argument);
    case DRM_NR_MODE_GETPROPERTY:       return ioctl_get_property(user_argument);
    case DRM_NR_MODE_OBJ_GETPROPERTIES: return ioctl_obj_get_properties(user_argument);
    case DRM_NR_MODE_SETPROPERTY:       return ioctl_connector_set_property(user_argument);
    case DRM_NR_MODE_OBJ_SETPROPERTY:   return ioctl_obj_set_property(user_argument);
    case DRM_NR_MODE_SETPLANE:          return -EINVAL;
    case DRM_NR_MODE_GETRESOURCES:      return ioctl_get_resources(user_argument);
    case DRM_NR_MODE_GETCONNECTOR:      return ioctl_get_connector(user_argument);
    case DRM_NR_MODE_GETENCODER:        return ioctl_get_encoder(user_argument);
    case DRM_NR_MODE_GETCRTC:           return ioctl_get_crtc(user_argument);
    case DRM_NR_MODE_GETFB:             return ioctl_getfb(file, user_argument);
    case DRM_NR_MODE_CURSOR:            return ioctl_cursor(file, user_argument, 0);
    case DRM_NR_MODE_CURSOR2:           return ioctl_cursor(file, user_argument, 1);
    case DRM_NR_MODE_GETGAMMA:          return ioctl_gamma(user_argument, 0);
    case DRM_NR_MODE_SETGAMMA:          return ioctl_gamma(user_argument, 1);
    case DRM_NR_MODE_ATOMIC:            return ioctl_atomic(file, user_argument);
    case DRM_NR_MODE_CREATEPROPBLOB:    return ioctl_create_blob(file, user_argument);
    case DRM_NR_MODE_DESTROYPROPBLOB:   return ioctl_destroy_blob(file, user_argument);
    case DRM_NR_MODE_SETCRTC:           return ioctl_set_crtc(file, user_argument);
    case DRM_NR_MODE_PAGE_FLIP:         return ioctl_page_flip(file, user_argument);
    case DRM_NR_MODE_DIRTYFB:           return ioctl_dirty_fb(file, user_argument);
    case DRM_NR_MODE_CREATE_DUMB:       return ioctl_create_dumb(file, user_argument);
    case DRM_NR_MODE_MAP_DUMB:          return ioctl_map_dumb(file, user_argument);
    case DRM_NR_MODE_DESTROY_DUMB:      return ioctl_destroy_dumb(file, user_argument);
    case DRM_NR_MODE_ADDFB:             return ioctl_addfb(file, user_argument);
    case DRM_NR_MODE_ADDFB2:            return ioctl_addfb2(file, user_argument);
    case DRM_NR_MODE_RMFB:
    case DRM_NR_MODE_CLOSEFB:           return ioctl_rmfb(file, user_argument);
    case DRM_NR_GEM_CLOSE:              return ioctl_gem_close(file, user_argument);
    case DRM_NR_MODE_CREATE_LEASE:
    case DRM_NR_MODE_LIST_LESSEES:
    case DRM_NR_MODE_GET_LEASE:
    case DRM_NR_MODE_REVOKE_LEASE:      return -EOPNOTSUPP;

    case DRM_NR_VIRTGPU_GETPARAM:
    case DRM_NR_VIRTGPU_GET_CAPS:
    case DRM_NR_VIRTGPU_RESOURCE_CREATE:
    case DRM_NR_VIRTGPU_RESOURCE_INFO:
    case DRM_NR_VIRTGPU_MAP:
    case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
    case DRM_NR_VIRTGPU_TRANSFER_TO_HOST:
    case DRM_NR_VIRTGPU_EXECBUFFER:
    case DRM_NR_VIRTGPU_WAIT:
        if (!virtgpu_virgl_available()) return -ENOTTY;
        switch (IOCTL_NR(request)) {
        case DRM_NR_VIRTGPU_GETPARAM: return ioctl_virtgpu_getparam(user_argument);
        case DRM_NR_VIRTGPU_GET_CAPS: return ioctl_virtgpu_get_caps(user_argument);
        case DRM_NR_VIRTGPU_RESOURCE_CREATE:
            return ioctl_virtgpu_resource_create(file, user_argument);
        case DRM_NR_VIRTGPU_RESOURCE_INFO: return ioctl_virtgpu_resource_info(file, user_argument);
        case DRM_NR_VIRTGPU_MAP:           return ioctl_virtgpu_map(file, user_argument);
        case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
            return ioctl_virtgpu_transfer(file, user_argument, 0);
        case DRM_NR_VIRTGPU_TRANSFER_TO_HOST: return ioctl_virtgpu_transfer(file, user_argument, 1);
        case DRM_NR_VIRTGPU_EXECBUFFER:       return ioctl_virtgpu_execbuffer(file, user_argument);
        default:                              return ioctl_virtgpu_wait(file, user_argument);
        }
    case DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB:
        if (!virtgpu_virgl_available()) return -ENOTTY;
        return ioctl_virtgpu_resource_create_blob(file, user_argument);
    case DRM_NR_VIRTGPU_CONTEXT_INIT:
        if (!virtgpu_virgl_available()) return -ENOTTY;
        return ioctl_virtgpu_context_init(file, user_argument);
    default:                                  return -ENOTTY;
    }
}

int64_t drm_device_mmap(struct vfs_node *node, struct file *file, uint64_t cr3,
                        uint64_t virtual_address, uint64_t length, uint64_t offset,
                        uint64_t page_flags) {
    DRM_LOCKED;
    (void)node;
    if (!drm_ready || !length) return -EINVAL;
    if (offset < DRM_MAP_OFFSET_BASE) return -EINVAL;

    uint32_t handle = (uint32_t)((offset - DRM_MAP_OFFSET_BASE) / 4096ULL);
    struct drm_dumb_buffer *buffer = buffer_of(file, handle);
    if (!buffer) return -ENOENT;
    if (length > buffer->size) return -EINVAL;
    return map_buffer(buffer, cr3, virtual_address, length, 0, page_flags);
}

static int driver_held;

void drm_device_open(struct vfs_node *node) {
    DRM_LOCKED;
    (void)node;
    if (!open_count++) driver_held = virtgpu_hold() == 0;
}

int drm_is_open(void) {
    DRM_LOCKED;
    return open_count != 0;
}

void drm_device_close(struct vfs_node *node) {
    DRM_LOCKED;
    (void)node;
    drm_enter();
    if (open_count) open_count--;
    if (!open_count) {
        active_fb_id = 0;
        render_contexts_release();
        virtgpu_scanout_disable();
        scanout_current = 0;
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        if (driver_held) virtgpu_release();
        driver_held = 0;
    }
    drm_leave();
}

void drm_file_close(struct file *file) {
    DRM_LOCKED;
    if (!file) return;
    drm_enter();
    if (render_driver && render_driver->file_close) render_driver->file_close(file);
    forget_master(file);
    for (int index = 0; index < framebuffer_capacity; index++) {
        if (!framebuffers[index].id || framebuffers[index].owner != file) continue;
        if (active_fb_id == framebuffers[index].id) active_fb_id = 0;
        struct drm_dumb_buffer *buffer = buffer_find(framebuffers[index].handle);
        memset(&framebuffers[index], 0, sizeof(framebuffers[index]));
        if (buffer) buffer_release(buffer);
    }
    for (unsigned index = 0; index < blob_capacity; index++)
        if (blobs[index].id && blobs[index].owner == file) blob_release(&blobs[index]);
    struct drm_event_queue *queue = event_queue_of(file, 0);
    if (queue) memset(queue, 0, sizeof(*queue));
    file_context_release(file);
    retired_reap();
    for (int index = 0; index < import_capacity; index++) {
        if (imports[index].file != file) continue;
        struct drm_dumb_buffer *buffer = buffer_find(imports[index].handle);
        memset(&imports[index], 0, sizeof(imports[index]));
        if (buffer) buffer_release(buffer);
    }
    for (uint32_t index = 0; index < buffer_capacity; index++)
        if (buffers[index] && buffers[index]->handle && owns_handle(buffers[index], file)) {
            buffers[index]->owner_closed = 1;
            buffer_release(buffers[index]);
        }
    drm_leave();
}

void drm_display_suspend(void) {
    DRM_LOCKED;
    if (!drm_ready) return;
    drm_console_present();
}

void drm_console_present(void) {
    const struct display_flipper *flipper = display_flipper();
    if (flipper) {
        if (flipper->console_in_front() || !mutex_trylock(&drm_lock)) return;
        (void)flipper->flip_to_console();
        mutex_unlock(&drm_lock);
        return;
    }
    if (!virtgpu_available()) return;
    if (!mutex_trylock(&drm_lock)) return;
    uint32_t pitch = framebuffer_pitch();
    if (pitch)
        (void)virtgpu_console_present(framebuffer_physical_address() + framebuffer_memory_offset(),
                                      pitch / 4U, framebuffer_width(), framebuffer_height());
    mutex_unlock(&drm_lock);
}

void drm_display_resume(void) {
    if (!drm_ready || !active_fb_id) return;
    drm_enter();
    (void)present_framebuffer(NULL, active_fb_id, NULL);
    drm_leave();
}

// At boot, from the display driver's late setup, before any client opens DRM.
void drm_register_render_driver(const struct drm_render_driver *driver) {
    __atomic_store_n(&render_driver, driver, __ATOMIC_RELEASE);
}

const struct drm_render_driver *drm_render_driver(void) { return render_driver; }

int drm_buffer_new_pages(struct file *file, uint64_t size, uint32_t *handle) {
    struct drm_dumb_buffer *buffer = buffer_new(file, size);
    if (!buffer) return -ENOMEM;
    *handle = buffer->handle;
    return 0;
}

int drm_buffer_new_device(struct file *file, uint64_t physical, uint64_t size, uint32_t *handle) {
    if (!size || size > (1ULL << 32)) return -EINVAL;
    uint32_t slot_handle = 0;
    struct drm_dumb_buffer *slot = buffer_slot(&slot_handle);
    if (!slot) return -ENOMEM;
    slot->owner = file;
    slot->refs = 1;
    slot->size = (size + 4095ULL) & ~4095ULL;
    slot->device_memory = 1;
    slot->host_physical = physical;
    slot->handle = slot_handle;
    *handle = slot_handle;
    return 0;
}

int drm_buffer_lookup(struct file *file, uint32_t handle, struct drm_buffer_view *view) {
    struct drm_dumb_buffer *buffer = buffer_of(file, handle);
    if (!buffer) return -ENOENT;
    view->pages = buffer->pages;
    view->page_count = buffer->page_count;
    view->device_physical = buffer->device_memory ? buffer->host_physical : 0;
    view->size = buffer->size;
    return 0;
}

uint64_t drm_buffer_map_offset(uint32_t handle) {
    return DRM_MAP_OFFSET_BASE + (uint64_t)handle * 4096ULL;
}

void drm_unlock_for_wait(void) { drm_leave(); }

void drm_relock_after_wait(void) { drm_enter(); }
