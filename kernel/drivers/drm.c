#include <stddef.h>
#include <stdint.h>
#include "../include/klock.h"
#include "../include/percpu.h"
#include "../include/drm.h"
#include "../include/file.h"
#include "../include/framebuffer.h"
#include "../include/heap.h"
#include "../include/time.h"
#include "../include/kstring.h"
#include "../include/pmm.h"
#include "../include/process.h"
#include "../include/usercopy.h"
#include "../include/virtgpu.h"
#include "../include/vmm.h"

extern void kprintf(const char *fmt, ...);

#define EINVAL 22
#define ENOENT 2
#define ENOMEM 12
#define ENOTTY 25
#define EFAULT 14
#define EPERM 1
#define EAGAIN 11
#define EBADF 9
#define EMFILE 24
#define EIO 5
#define EBUSY 16
#define ENODEV 19
#define EOPNOTSUPP 95

/* DRM_CLOEXEC in <drm/drm.h> is O_CLOEXEC by another name. */
#define DRM_CLOEXEC 02000000

/* The ABI below is Linux's, from <drm/drm.h> and <drm/drm_mode.h>. */

/* Decoded by type and number, because the size field differs between
   libdrm versions. */
#define DRM_IOCTL_TYPE 'd'
#define IOCTL_TYPE(request) (((request) >> 8) & 0xFFU)
#define IOCTL_NR(request) ((request) & 0xFFU)

#define DRM_NR_VERSION 0x00
#define DRM_NR_GET_UNIQUE 0x01
#define DRM_NR_GET_MAGIC 0x02
#define DRM_NR_GEM_CLOSE 0x09
#define DRM_NR_GET_CAP 0x0c
#define DRM_NR_SET_CLIENT_CAP 0x0d
#define DRM_NR_SET_VERSION 0x07
#define DRM_NR_AUTH_MAGIC 0x11
#define DRM_NR_SET_MASTER 0x1e
#define DRM_NR_DROP_MASTER 0x1f
#define DRM_NR_MODE_GETRESOURCES 0xa0
#define DRM_NR_MODE_GETCRTC 0xa1
#define DRM_NR_MODE_SETCRTC 0xa2
#define DRM_NR_MODE_GETENCODER 0xa6
#define DRM_NR_MODE_GETCONNECTOR 0xa7
#define DRM_NR_MODE_GETFB 0xad
#define DRM_NR_MODE_ADDFB 0xae
#define DRM_NR_MODE_RMFB 0xaf
#define DRM_NR_MODE_PAGE_FLIP 0xb0
#define DRM_NR_MODE_CURSOR 0xa3
#define DRM_NR_MODE_CURSOR2 0xa4
#define DRM_NR_MODE_ATOMIC 0xbc
#define DRM_NR_MODE_CREATEPROPBLOB 0xbd
#define DRM_NR_MODE_DESTROYPROPBLOB 0xbe
#define DRM_NR_MODE_DIRTYFB 0xb1
#define DRM_NR_MODE_CREATE_DUMB 0xb2
#define DRM_NR_MODE_MAP_DUMB 0xb3
#define DRM_NR_MODE_DESTROY_DUMB 0xb4
#define DRM_NR_MODE_ADDFB2 0xb8
#define DRM_NR_MODE_GETPROPERTY 0xaa
#define DRM_NR_MODE_SETPROPERTY 0xab
#define DRM_NR_MODE_GETPLANERESOURCES 0xb5
#define DRM_NR_MODE_GETPLANE 0xb6
#define DRM_NR_MODE_SETPLANE 0xb7
#define DRM_NR_MODE_OBJ_GETPROPERTIES 0xb9
#define DRM_NR_MODE_OBJ_SETPROPERTY 0xba
#define DRM_NR_PRIME_HANDLE_TO_FD 0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE 0x2e

/* The driver-private range, which means virtio_gpu's calls because VERSION says so. */
#define DRM_COMMAND_BASE 0x40
#define DRM_NR_VIRTGPU_MAP (DRM_COMMAND_BASE + 0x01)
#define DRM_NR_VIRTGPU_EXECBUFFER (DRM_COMMAND_BASE + 0x02)
#define DRM_NR_VIRTGPU_GETPARAM (DRM_COMMAND_BASE + 0x03)
#define DRM_NR_VIRTGPU_RESOURCE_CREATE (DRM_COMMAND_BASE + 0x04)
#define DRM_NR_VIRTGPU_RESOURCE_INFO (DRM_COMMAND_BASE + 0x05)
#define DRM_NR_VIRTGPU_TRANSFER_FROM_HOST (DRM_COMMAND_BASE + 0x06)
#define DRM_NR_VIRTGPU_TRANSFER_TO_HOST (DRM_COMMAND_BASE + 0x07)
#define DRM_NR_VIRTGPU_WAIT (DRM_COMMAND_BASE + 0x08)
#define DRM_NR_VIRTGPU_GET_CAPS (DRM_COMMAND_BASE + 0x09)
#define DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB (DRM_COMMAND_BASE + 0x0a)
#define DRM_NR_VIRTGPU_CONTEXT_INIT (DRM_COMMAND_BASE + 0x0b)

/* What mesa asks before deciding how to talk to us; zero sends it down
   the older path. */
#define VIRTGPU_PARAM_3D_FEATURES 1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB 3
#define VIRTGPU_PARAM_HOST_VISIBLE 4
#define VIRTGPU_PARAM_CROSS_DEVICE 5
#define VIRTGPU_PARAM_CONTEXT_INIT 6
/* A bitmask of the capsets that exist, so mesa can ask once instead of
   probing each id in turn. */
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS 7

struct drm_virtgpu_map {
    uint64_t offset;
    uint32_t handle;
    uint32_t pad;
};

/* `value` is where to put the answer, and the answer is four bytes wide. */
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

/* Both directions have the same shape; which one it is comes from the call. */
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

/* These cross to userspace, and a field in the wrong place is a resource
   created with someone else's width. */
typedef char drm_virtgpu_execbuffer_size_check[
    (sizeof(struct drm_virtgpu_execbuffer) == 64) ? 1 : -1];
typedef char drm_virtgpu_resource_create_size_check[
    (sizeof(struct drm_virtgpu_resource_create) == 56) ? 1 : -1];
typedef char drm_virtgpu_transfer_size_check[
    (sizeof(struct drm_virtgpu_3d_transfer) == 44) ? 1 : -1];
typedef char drm_virtgpu_get_caps_size_check[
    (sizeof(struct drm_virtgpu_get_caps) == 24) ? 1 : -1];

#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_PRIME 0x5
#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CAP_CURSOR_WIDTH 0x8
#define DRM_CAP_CURSOR_HEIGHT 0x9
#define DRM_CAP_ADDFB2_MODIFIERS 0x10

/* The single set of object ids this device ever reports. */
#define DRM_CRTC_ID 1
#define DRM_CONNECTOR_ID 2
#define DRM_ENCODER_ID 3
#define DRM_PLANE_ID 4

/* The one property this device exposes, because weston needs a plane with a type. */
#define DRM_PROP_TYPE_ID 10
#define DRM_PROP_CRTC_ACTIVE 11
#define DRM_PROP_CRTC_MODE_ID 12
#define DRM_PROP_PLANE_CRTC_ID 13
#define DRM_PROP_PLANE_FB_ID 14
#define DRM_PROP_CONNECTOR_CRTC_ID 15
/* Where the plane reads from and where it lands, which an atomic commit must
   set: without them it has not said where the plane goes. */
#define DRM_PROP_PLANE_SRC_X 16
#define DRM_PROP_PLANE_SRC_Y 17
#define DRM_PROP_PLANE_SRC_W 18
#define DRM_PROP_PLANE_SRC_H 19
#define DRM_PROP_PLANE_CRTC_X 20
#define DRM_PROP_PLANE_CRTC_Y 21
#define DRM_PROP_PLANE_CRTC_W 22
#define DRM_PROP_PLANE_CRTC_H 23
#define DRM_PROP_PLANE_FIRST_RECT DRM_PROP_PLANE_SRC_X
#define DRM_PROP_PLANE_LAST_RECT DRM_PROP_PLANE_CRTC_H

#define DRM_MODE_OBJECT_CRTC 0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER 0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee

#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
#define DRM_MODE_PROP_RANGE (1 << 1)
#define DRM_MODE_PROP_BLOB (1 << 4)
#define DRM_MODE_PROP_ENUM (1 << 3)

#define DRM_PLANE_TYPE_OVERLAY 0
#define DRM_PLANE_TYPE_PRIMARY 1
#define DRM_PLANE_TYPE_CURSOR 2

#define DRM_PROP_NAME_LEN 32

/* The scanout is XRGB8888 and there is no format conversion anywhere here. */
#define DRM_FORMAT_XRGB8888 0x34325258

#define DRM_MODE_CONNECTED 1
#define DRM_MODE_SUBPIXEL_UNKNOWN 1
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_ENCODER_VIRTUAL 5
#define DRM_MODE_TYPE_PREFERRED (1 << 3)
#define DRM_MODE_TYPE_DRIVER (1 << 6)

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

/* Linux's layout: flat object ids, and per-object counts behind count_props_ptr. */
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

struct drm_mode_create_blob { uint64_t data; uint32_t length; uint32_t blob_id; };
struct drm_mode_destroy_blob { uint32_t blob_id; };

typedef char drm_modeinfo_size_check[
    (sizeof(struct drm_mode_modeinfo) == 68) ? 1 : -1];
typedef char drm_create_dumb_size_check[
    (sizeof(struct drm_mode_create_dumb) == 32) ? 1 : -1];

/* --- objects ------------------------------------------------------------ */

/* Wide enough for any display here, and narrow enough that width * 4 * height cannot overflow. */
#define DRM_MAX_DIMENSION 16384U
#define DRM_MAX_BUFFERS 4096
#define DRM_MAX_FRAMEBUFFERS 64

/* A run of ordinary pages userspace maps and draws into. */
struct drm_dumb_buffer {
    uint32_t handle;      /* 0 when the slot is free */
    /* The open file that made it; handles are private to one client. */
    const struct file *owner;
    /* Set by a PRIME export, after which the descriptor is the capability. */
    uint8_t shared;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;        /* page-aligned byte count */
    uint64_t page_count;
    uint64_t *pages;      /* physical addresses */
    /* The host resource these pages back, created on first present and 0 on a
       machine with no virtio-gpu. */
    uint32_t virtio_resource;
    /* Made by RESOURCE_CREATE, so the contents are the host's and the pages
       are staging. */
    uint8_t rendered;
    /* Holders beyond the handle: every PRIME descriptor and every framebuffer. */
    uint32_t refs;
};

struct drm_framebuffer {
    uint32_t id;          /* 0 when the slot is free */
    const struct file *owner;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

/* Completion events read back off the descriptor; presentation is synchronous. */
#define DRM_EVENT_FLIP_COMPLETE 0x02
#define DRM_MAX_EVENTS 16

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

static struct drm_event_vblank events[DRM_MAX_EVENTS];
static uint32_t event_head;
static uint32_t event_tail;
static uint32_t event_count;
static uint32_t flip_sequence;

/* The console and DRM share one scanout, so one of them has to stand down. */
static const char drm_display_owner;
static uint32_t open_count;

static struct drm_dumb_buffer buffers[DRM_MAX_BUFFERS];
static struct drm_framebuffer framebuffers[DRM_MAX_FRAMEBUFFERS];
#define DRM_MAX_BLOBS 32
#define DRM_MAX_BLOB_BYTES 256
struct drm_property_blob {
    uint32_t id; const struct file *owner;
    uint32_t length; uint8_t data[DRM_MAX_BLOB_BYTES];
};
static struct drm_property_blob blobs[DRM_MAX_BLOBS];
static uint32_t next_blob_id = 1;
/* Where to start looking for a free slot, so a table that is mostly full is
   not walked from the beginning every time. */
static uint32_t next_handle = 1;
/* One host rendering context per process; see render_context() below. */
#define DRM_MAX_CONTEXTS 8
static struct {
    uint64_t pid;
    uint32_t context;
} render_contexts[DRM_MAX_CONTEXTS];
static uint32_t next_render_context = 1;
static void render_contexts_release(void);
static uint32_t render_context(void);
static uint32_t next_fb_id = 1;
static uint32_t active_fb_id;
static int drm_ready;

#define DRM_MAP_OFFSET_BASE 0x100000000ULL

void drm_init(void) {
    memset(buffers, 0, sizeof(buffers));
    memset(framebuffers, 0, sizeof(framebuffers));
    memset(blobs, 0, sizeof(blobs));
    next_handle = 1;
    next_fb_id = 1;
    next_blob_id = 1;
    active_fb_id = 0;
    event_head = event_tail = event_count = 0;
    flip_sequence = 0;
    open_count = 0;
    memset(render_contexts, 0, sizeof(render_contexts));
    next_render_context = 1;
    drm_ready = framebuffer_available();
}

int drm_available(void) { return drm_ready; }

static struct drm_dumb_buffer *buffer_find(uint32_t handle) {
    if (!handle || handle > (uint32_t)DRM_MAX_BUFFERS) return NULL;
    struct drm_dumb_buffer *buffer = &buffers[handle - 1U];
    return buffer->handle == handle ? buffer : NULL;
}

static struct drm_framebuffer *framebuffer_find(uint32_t id) {
    if (!id) return NULL;
    for (int index = 0; index < DRM_MAX_FRAMEBUFFERS; index++) {
        if (framebuffers[index].id == id) return &framebuffers[index];
    }
    return NULL;
}

static struct drm_property_blob *blob_find(uint32_t id) {
    /* Zero is not an id, and without this it matches the first free slot. */
    if (!id) return NULL;
    for (unsigned index = 0; index < DRM_MAX_BLOBS; index++)
        if (blobs[index].id == id) return &blobs[index];
    return NULL;
}

/* Refusing an object this client was never given, because handles are guessable. */
static struct drm_dumb_buffer *buffer_of(const struct file *client, uint32_t handle) {
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (!buffer) return NULL;
    return (buffer->owner == client || buffer->shared) ? buffer : NULL;
}

static struct drm_framebuffer *framebuffer_of(const struct file *client, uint32_t id) {
    struct drm_framebuffer *fb = framebuffer_find(id);
    return (fb && fb->owner == client) ? fb : NULL;
}

static struct drm_property_blob *blob_of(const struct file *client, uint32_t id) {
    struct drm_property_blob *blob = blob_find(id);
    return (blob && blob->owner == client) ? blob : NULL;
}

static int64_t ioctl_create_blob(const struct file *client, uint64_t user_argument) {
    struct drm_mode_create_blob request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (!request.data || !request.length || request.length > DRM_MAX_BLOB_BYTES) return -EINVAL;
    for (unsigned index = 0; index < DRM_MAX_BLOBS; index++) {
        if (blobs[index].id) continue;
        if (copy_from_user(blobs[index].data, request.data, request.length) != 0) return -EFAULT;
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
    memset(blob, 0, sizeof(*blob));
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

/* No hardware cursor, but acknowledging this is what allows a software one. */
static int64_t ioctl_cursor(const struct file *client, uint64_t user_argument, int cursor2) {
    struct drm_mode_cursor request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    (void)cursor2;
    if (request.crtc_id != DRM_CRTC_ID) return -ENOENT;
    if ((request.flags & 1U) && request.handle && !buffer_of(client, request.handle))
        return -ENOENT;
    return 0;
}

#define DRM_MODE_PAGE_FLIP_EVENT 0x01U
#define DRM_MODE_ATOMIC_TEST_ONLY 0x100U
#define DRM_MODE_ATOMIC_NONBLOCK 0x200U
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x400U
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02U
#define DRM_ATOMIC_MAX_OBJECTS 16U
#define DRM_ATOMIC_MAX_PROPS 64U

static int present_framebuffer(const struct file *client, uint32_t fb_id);
static void queue_flip_event(uint64_t user_data);

/* Nothing here can scale or move the one plane, so a commit may only ask for
   the whole scanout at the origin. */
static int plane_rectangle_ok(uint32_t property, uint64_t value) {
    switch (property) {
    case DRM_PROP_PLANE_SRC_X:
    case DRM_PROP_PLANE_SRC_Y:
    case DRM_PROP_PLANE_CRTC_X:
    case DRM_PROP_PLANE_CRTC_Y: return value == 0;
    case DRM_PROP_PLANE_SRC_W: return value == ((uint64_t)framebuffer_width() << 16);
    case DRM_PROP_PLANE_SRC_H: return value == ((uint64_t)framebuffer_height() << 16);
    case DRM_PROP_PLANE_CRTC_W: return value == framebuffer_width();
    case DRM_PROP_PLANE_CRTC_H: return value == framebuffer_height();
    default: return 0;
    }
}

/* One description of the whole display, in the arrays libdrm actually sends. */
static int64_t ioctl_atomic(const struct file *client, uint64_t user_argument) {
    struct drm_mode_atomic request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    /* Async flips are not implemented, so asking for one is refused rather
       than quietly answered as if it had happened. */
    if (request.flags & ~(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK |
                          DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT))
        return -EINVAL;
    if (!request.count_objs || request.count_objs > DRM_ATOMIC_MAX_OBJECTS) return -EINVAL;

    uint32_t objects[DRM_ATOMIC_MAX_OBJECTS];
    uint32_t counts[DRM_ATOMIC_MAX_OBJECTS];
    uint64_t bytes = (uint64_t)request.count_objs * sizeof(objects[0]);
    if (copy_from_user(objects, request.objs_ptr, bytes) != 0 ||
        copy_from_user(counts, request.count_props_ptr, bytes) != 0) return -EFAULT;

    uint32_t total = 0;
    for (uint32_t object = 0; object < request.count_objs; object++) {
        if (counts[object] > DRM_ATOMIC_MAX_PROPS ||
            total + counts[object] > DRM_ATOMIC_MAX_PROPS) return -EINVAL;
        total += counts[object];
    }
    if (!total) return -EINVAL;

    uint32_t props[DRM_ATOMIC_MAX_PROPS];
    uint64_t values[DRM_ATOMIC_MAX_PROPS];
    if (copy_from_user(props, request.props_ptr, (uint64_t)total * sizeof(props[0])) != 0 ||
        copy_from_user(values, request.prop_values_ptr,
                       (uint64_t)total * sizeof(values[0])) != 0) return -EFAULT;

    uint32_t new_fb = active_fb_id;
    int new_active = active_fb_id != 0;
    int mode_cleared = 0;
    uint32_t taken = 0;
    for (uint32_t object = 0; object < request.count_objs; object++) {
        for (uint32_t index = 0; index < counts[object]; index++, taken++) {
            uint32_t id = props[taken];
            uint64_t value = values[taken];
            if (objects[object] == DRM_CRTC_ID && id == DRM_PROP_CRTC_ACTIVE) {
                if (value > 1) return -EINVAL;
                new_active = (int)value;
            } else if (objects[object] == DRM_CRTC_ID && id == DRM_PROP_CRTC_MODE_ID) {
                /* Zero is how a commit turns the output off, and it is not a
                   blob that failed to be found. */
                if (!value) { mode_cleared = 1; continue; }
                struct drm_property_blob *blob = blob_of(client, (uint32_t)value);
                if (!blob || blob->length < sizeof(struct drm_mode_modeinfo)) return -EINVAL;
                struct drm_mode_modeinfo mode;
                memcpy(&mode, blob->data, sizeof(mode));
                if (mode.hdisplay != framebuffer_width() ||
                    mode.vdisplay != framebuffer_height()) return -EINVAL;
            } else if (objects[object] == DRM_PLANE_ID && id == DRM_PROP_PLANE_FB_ID) {
                if (value > UINT32_MAX ||
                    (value && !framebuffer_of(client, (uint32_t)value))) return -EINVAL;
                new_fb = (uint32_t)value;
            } else if (objects[object] == DRM_PLANE_ID && id == DRM_PROP_PLANE_CRTC_ID) {
                if (value != 0 && value != DRM_CRTC_ID) return -EINVAL;
            } else if (objects[object] == DRM_PLANE_ID &&
                       id >= DRM_PROP_PLANE_FIRST_RECT && id <= DRM_PROP_PLANE_LAST_RECT) {
                if (!plane_rectangle_ok(id, value)) return -EINVAL;
            } else if (objects[object] == DRM_CONNECTOR_ID &&
                       id == DRM_PROP_CONNECTOR_CRTC_ID) {
                if (value != 0 && value != DRM_CRTC_ID) return -EINVAL;
            } else {
                return -EINVAL;
            }
        }
    }
    if (mode_cleared) new_active = 0;
    if (request.flags & DRM_MODE_ATOMIC_TEST_ONLY) return 0;

    if (!new_active || !new_fb) {
        active_fb_id = 0;
        virtgpu_scanout_disable();
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
    } else {
        int status = present_framebuffer(client, new_fb);
        if (status != 0) return status;
        active_fb_id = new_fb;
    }
    /* The commit has already happened, so the completion is queued rather
       than promised. */
    if (request.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(request.user_data);
    return 0;
}

/* Drop one reference, which an exported descriptor can outlive the handle by. */
static void buffer_release(struct drm_dumb_buffer *buffer) {
    if (!buffer || !buffer->handle) return;
    if (buffer->refs > 1) { buffer->refs--; return; }
    /* Before the pages go: the host is still reading them through the resource. */
    if (buffer->virtio_resource) virtgpu_resource_destroy(buffer->virtio_resource);
    for (uint64_t index = 0; index < buffer->page_count; index++) {
        if (buffer->pages[index]) pmm_free_page((void *)buffer->pages[index]);
    }
    kfree(buffer->pages);
    memset(buffer, 0, sizeof(*buffer));
}

/* --- mode ---------------------------------------------------------------- */

/* One mode, the display as it already is, with synthesised timings. */
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

    /* "1024x768" and so on, built by hand: no snprintf in the kernel. */
    char *out = mode->name;
    size_t limit = sizeof(mode->name) - 1;
    size_t used = 0;
    uint32_t parts[2] = { width, height };
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

/* Copy `count` items into a userspace array, but only if the caller said it had
   room; DRM's convention is to ask with count 0 first, then again with buffers. */
static int copy_array_out(uint64_t user_pointer, uint32_t user_count,
                          const void *source, size_t item_size, uint32_t count) {
    if (!user_pointer || user_count == 0) return 0;
    if (user_count < count) count = user_count;
    if (!count) return 0;
    return copy_to_user(user_pointer, source, item_size * count) == 0 ? 0 : -EFAULT;
}

/* --- ioctls -------------------------------------------------------------- */

static int64_t ioctl_version(uint64_t user_argument) {
    struct drm_version version;
    if (copy_from_user(&version, user_argument, sizeof(version)) != 0) return -EFAULT;

    /* The name is the driver's identity: mesa loads `<name>_dri.so` by it. */
    int rendering = virtgpu_virgl_available();
    static const char virtio_name[] = "virtio_gpu";
    static const char plain_name[] = "tunixdrm";
    const char *name = rendering ? virtio_name : plain_name;
    size_t name_size = (rendering ? sizeof(virtio_name) : sizeof(plain_name)) - 1;
    static const char date[] = "20260721";
    static const char desc[] = "Tunix framebuffer KMS";

    /* mesa reads these as a feature level, and minor 0 says not to ask for a fence. */
    version.version_major = rendering ? 0 : 1;
    version.version_minor = 0;
    version.version_patchlevel = 0;

    /* The caller passes buffers and lengths; we fill what fits and always
       report the true length, which is how libdrm sizes its second call. */
    struct { uint64_t pointer; uint64_t *length; const char *text; size_t size; } fields[] = {
        { version.name, &version.name_len, name, name_size },
        { version.date, &version.date_len, date, sizeof(date) - 1 },
        { version.desc, &version.desc_len, desc, sizeof(desc) - 1 },
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

#define DRM_CLIENT_CAP_STEREO_3D 1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC 3

/* Saying yes to every capability was worse than saying no to the ones we lack. */
static int64_t ioctl_set_client_cap(uint64_t user_argument) {
    struct drm_set_client_cap { uint64_t capability; uint64_t value; } request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (request.capability != DRM_CLIENT_CAP_UNIVERSAL_PLANES &&
        request.capability != DRM_CLIENT_CAP_ATOMIC) return -EOPNOTSUPP;
    return 0;
}

static int64_t ioctl_get_cap(uint64_t user_argument) {
    struct drm_get_cap cap;
    if (copy_from_user(&cap, user_argument, sizeof(cap)) != 0) return -EFAULT;
    switch (cap.capability) {
    case DRM_CAP_DUMB_BUFFER: cap.value = 1; break;
    /* Page-flip completions are stamped from the monotonic uptime clock, which
       is what this claims -- and weston refuses the device outright without it. */
    case DRM_CAP_TIMESTAMP_MONOTONIC: cap.value = 1; break;
    /* There is no hardware cursor; these are the sizes a client should assume
       for a software one, and libdrm's callers expect *some* answer. */
    case DRM_CAP_CURSOR_WIDTH:
    case DRM_CAP_CURSOR_HEIGHT: cap.value = 64; break;
    /* mesa reads this exact capability to decide how GBM allocates. */
    case DRM_CAP_PRIME: cap.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    /* One linear layout and nothing to negotiate. */
    case DRM_CAP_ADDFB2_MODIFIERS: cap.value = 1; break;
    /* Linux has no atomic capability, and 0x15 is the one for async page flips. */
    default: cap.value = 0; break;
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
        copy_array_out(res.connector_id_ptr, res.count_connectors, &connector,
                       sizeof(connector), 1) != 0 ||
        copy_array_out(res.encoder_id_ptr, res.count_encoders, &encoder,
                       sizeof(encoder), 1) != 0)
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

    if (copy_array_out(connector.modes_ptr, connector.count_modes, &mode,
                       sizeof(mode), 1) != 0 ||
        copy_array_out(connector.encoders_ptr, connector.count_encoders, &encoder,
                       sizeof(encoder), 1) != 0)
        return -EFAULT;

    connector.count_modes = 1;
    connector.count_encoders = 1;
    /* No properties: nothing here is adjustable. */
    connector.count_props = 0;
    connector.encoder_id = DRM_ENCODER_ID;
    connector.connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    connector.connector_type_id = 1;
    connector.connection = DRM_MODE_CONNECTED;
    /* Physical size is unknown; 0 is how DRM says so. */
    connector.mm_width = 0;
    connector.mm_height = 0;
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
    crtc.gamma_size = 0;
    crtc.mode_valid = active_fb_id ? 1 : 0;
    fill_mode(&crtc.mode);
    crtc.count_connectors = 0;
    return copy_to_user(user_argument, &crtc, sizeof(crtc)) == 0 ? 0 : -EFAULT;
}

/* Release a reference a PRIME export took, which the handle may already be gone by. */
void drm_buffer_put(uint32_t handle) {
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
    /* Exported, so a handle to it is reachable by whoever holds the
       descriptor. */
    buffer->shared = 1;
    struct file *file = file_create_dmabuf(request.handle, 0);
    if (!file) {
        buffer_release(buffer);
        return -ENOMEM;
    }
    int fd = process_install_file_flags(process_current(), file, 0,
                                       (request.flags & DRM_CLOEXEC) ? PROCESS_FD_CLOEXEC : 0);
    if (fd < 0) {
        file_unref(file);   /* which gives the buffer reference back */
        return -EMFILE;
    }
    request.fd = fd;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_prime_fd_to_handle(uint64_t user_argument) {
    struct drm_prime_handle request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct process *process = process_current();
    if (!process || !process->files || request.fd < 0 || request.fd >= PROCESS_MAX_FDS ||
        !process->files->fds[request.fd]) return -EBADF;
    struct file *file = process->files->fds[request.fd];
    if (file->kind != FILE_KIND_DMABUF) return -EINVAL;
    struct drm_dumb_buffer *buffer = buffer_find(file->dmabuf_handle);
    if (!buffer) return -ENOENT;

    /* A resource from another process has to be granted to this context first. */
    if (buffer->rendered && buffer->virtio_resource) {
        uint32_t context = render_context();
        if (context)
            (void)virtgpu_context_attach(context, buffer->virtio_resource, 1);
    }

    request.handle = file->dmabuf_handle;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

int64_t drm_dmabuf_mmap(struct file *file, uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset, uint64_t page_flags) {
    if (!file || !length || (offset & 0xFFFULL)) return -EINVAL;
    struct drm_dumb_buffer *buffer = buffer_find(file->dmabuf_handle);
    if (!buffer) return -ENOENT;
    if (offset >= buffer->size || length > buffer->size - offset) return -EINVAL;

    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX;
    uint64_t mapped = 0;
    for (; mapped < length; mapped += 4096ULL) {
        uint64_t physical = buffer->pages[(offset + mapped) / 4096ULL];
        /* Same contract as mapping through the card node: the mapping takes its
           own page reference so it outlives the buffer's handle. */
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

static int64_t ioctl_get_plane_resources(uint64_t user_argument) {
    struct drm_mode_get_plane_res res;
    if (copy_from_user(&res, user_argument, sizeof(res)) != 0) return -EFAULT;

    uint32_t plane = DRM_PLANE_ID;
    if (copy_array_out(res.plane_id_ptr, res.count_planes, &plane,
                       sizeof(plane), 1) != 0) return -EFAULT;
    res.count_planes = 1;
    return copy_to_user(user_argument, &res, sizeof(res)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_get_plane(uint64_t user_argument) {
    struct drm_mode_get_plane plane;
    if (copy_from_user(&plane, user_argument, sizeof(plane)) != 0) return -EFAULT;
    if (plane.plane_id != DRM_PLANE_ID) return -ENOENT;

    uint32_t format = DRM_FORMAT_XRGB8888;
    if (copy_array_out(plane.format_type_ptr, plane.count_format_types,
                       &format, sizeof(format), 1) != 0) return -EFAULT;

    plane.crtc_id = active_fb_id ? DRM_CRTC_ID : 0;
    plane.fb_id = active_fb_id;
    plane.possible_crtcs = 1U; /* the CRTC at pipe 0, the only one */
    plane.gamma_size = 0;
    plane.count_format_types = 1;
    return copy_to_user(user_argument, &plane, sizeof(plane)) == 0 ? 0 : -EFAULT;
}

/* The three names Linux gives the plane types, in value order. */
static const char *const plane_type_names[] = { "Overlay", "Primary", "Cursor" };

static int64_t ioctl_get_property(uint64_t user_argument) {
    struct drm_mode_get_property property;
    if (copy_from_user(&property, user_argument, sizeof(property)) != 0) return -EFAULT;
    if (property.prop_id == DRM_PROP_CRTC_ACTIVE) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, "ACTIVE", sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = { 0, 1 };
        if (copy_array_out(property.values_ptr, property.count_values, range,
                           sizeof(range[0]), 2) != 0) return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_CRTC_MODE_ID) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, "MODE_ID", sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_BLOB;
        property.count_values = 0;
        property.count_enum_blobs = 0;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_PLANE_CRTC_ID || property.prop_id == DRM_PROP_PLANE_FB_ID) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, property.prop_id == DRM_PROP_PLANE_CRTC_ID ? "CRTC_ID" : "FB_ID",
                sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = { 0, 0xFFFFFFFFULL };
        if (copy_array_out(property.values_ptr, property.count_values, range,
                           sizeof(range[0]), 2) != 0) return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id == DRM_PROP_CONNECTOR_CRTC_ID) {
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, "CRTC_ID", sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = { 0, DRM_CRTC_ID };
        if (copy_array_out(property.values_ptr, property.count_values, range,
                           sizeof(range[0]), 2) != 0) return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id >= DRM_PROP_PLANE_FIRST_RECT &&
        property.prop_id <= DRM_PROP_PLANE_LAST_RECT) {
        static const char *const rectangle_names[] = {
            "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
            "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H",
        };
        memset(property.name, 0, sizeof(property.name));
        strncpy(property.name, rectangle_names[property.prop_id - DRM_PROP_PLANE_FIRST_RECT],
                sizeof(property.name) - 1);
        property.flags = DRM_MODE_PROP_RANGE;
        property.count_values = 2;
        property.count_enum_blobs = 0;
        uint64_t range[2] = { 0, 0xFFFFFFFFULL };
        if (copy_array_out(property.values_ptr, property.count_values, range,
                           sizeof(range[0]), 2) != 0) return -EFAULT;
        return copy_to_user(user_argument, &property, sizeof(property)) == 0 ? 0 : -EFAULT;
    }
    if (property.prop_id != DRM_PROP_TYPE_ID) return -ENOENT;

    /* An enum property carries no values array; its choices live in the enum
       blob, one drm_mode_property_enum per name. */
    struct drm_mode_property_enum choices[3];
    memset(choices, 0, sizeof(choices));
    for (unsigned index = 0; index < 3U; index++) {
        choices[index].value = index;
        strncpy(choices[index].name, plane_type_names[index], DRM_PROP_NAME_LEN - 1);
    }
    if (copy_array_out(property.enum_blob_ptr, property.count_enum_blobs,
                       choices, sizeof(choices[0]), 3) != 0) return -EFAULT;

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

    /* Only the plane has a property. CRTCs and connectors report none, which is
       legal and which weston copes with -- it only needs the call to succeed. */
    uint32_t count = 0;
    uint32_t ids[16];
    uint64_t values[16];
    if (request.obj_type == DRM_MODE_OBJECT_PLANE) {
        if (request.obj_id != DRM_PLANE_ID) return -ENOENT;
        ids[0] = DRM_PROP_TYPE_ID; values[0] = DRM_PLANE_TYPE_PRIMARY;
        ids[1] = DRM_PROP_PLANE_CRTC_ID; values[1] = DRM_CRTC_ID;
        ids[2] = DRM_PROP_PLANE_FB_ID; values[2] = active_fb_id;
        /* An atomic client sets all eight of these on every commit, and a
           plane that does not have them cannot be committed at all. */
        ids[3] = DRM_PROP_PLANE_SRC_X; values[3] = 0;
        ids[4] = DRM_PROP_PLANE_SRC_Y; values[4] = 0;
        ids[5] = DRM_PROP_PLANE_SRC_W; values[5] = (uint64_t)framebuffer_width() << 16;
        ids[6] = DRM_PROP_PLANE_SRC_H; values[6] = (uint64_t)framebuffer_height() << 16;
        ids[7] = DRM_PROP_PLANE_CRTC_X; values[7] = 0;
        ids[8] = DRM_PROP_PLANE_CRTC_Y; values[8] = 0;
        ids[9] = DRM_PROP_PLANE_CRTC_W; values[9] = framebuffer_width();
        ids[10] = DRM_PROP_PLANE_CRTC_H; values[10] = framebuffer_height();
        count = 11;
    } else if (request.obj_type == DRM_MODE_OBJECT_CRTC) {
        if (request.obj_id != DRM_CRTC_ID) return -ENOENT;
        ids[0] = DRM_PROP_CRTC_ACTIVE; values[0] = active_fb_id != 0;
        ids[1] = DRM_PROP_CRTC_MODE_ID; values[1] = 0; count = 2;
    } else if (request.obj_type == DRM_MODE_OBJECT_CONNECTOR) {
        if (request.obj_id != DRM_CONNECTOR_ID) return -ENOENT;
        ids[0] = DRM_PROP_CONNECTOR_CRTC_ID; values[0] = active_fb_id ? DRM_CRTC_ID : 0; count = 1;
    } else if (request.obj_type == DRM_MODE_OBJECT_ENCODER) {
        if (request.obj_id != DRM_ENCODER_ID) return -ENOENT;
    } else {
        return -EINVAL;
    }

    if (copy_array_out(request.props_ptr, request.count_props, ids,
                       sizeof(ids[0]), count) != 0 ||
        copy_array_out(request.prop_values_ptr, request.count_props, values,
                       sizeof(values[0]), count) != 0) return -EFAULT;
    request.count_props = count;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

/* With a virtio-gpu the buffer's own pages are the resource, so nothing is copied. */
static int present_via_virtgpu(const struct drm_framebuffer *fb,
                               struct drm_dumb_buffer *buffer) {
    uint32_t stride_pixels = buffer->pitch / 4U;
    if (!stride_pixels || !buffer->height) return -1;
    if (!buffer->virtio_resource) {
        buffer->virtio_resource = virtgpu_resource_create(stride_pixels, buffer->height,
                                                          buffer->pages, buffer->page_count);
        if (!buffer->virtio_resource) return -1;
    }
    uint32_t width = fb->width < stride_pixels ? fb->width : stride_pixels;
    uint32_t height = fb->height < buffer->height ? fb->height : buffer->height;
    return virtgpu_present(buffer->virtio_resource, width, height,
                           !buffer->rendered);
}

/* One processor inside this driver at a time, so the kernel lock does not have
   to be: a whole-screen blit reads a client's buffer and writes the scanout,
   and the only thing that would be unsafe beside it is freeing that buffer.
   Measured through /proc/klock: 421 ms for one ioctl on real hardware. */
static int64_t drm_dispatch_ioctl(struct file *file, unsigned long request,
                                  uint64_t user_argument);

static volatile uint32_t drm_busy_holder;   /* processor index + 1, 0 for nobody */

static void drm_enter(void) {
    uint32_t me = cpu_current()->index + 1U;
    for (;;) {
        uint32_t nobody = 0;
        if (__atomic_compare_exchange_n(&drm_busy_holder, &nobody, me, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        /* Waiting with the kernel lock held would put the stall back where it
           was, so it is given up here too. */
        int released = kernel_lock_release_for_wait();
        while (__atomic_load_n(&drm_busy_holder, __ATOMIC_RELAXED)) {
            kernel_lock_wait_tick();
            __asm__ volatile("pause");
        }
        kernel_lock_retake_after_wait(released);
    }
}

static void drm_leave(void) {
    __atomic_store_n(&drm_busy_holder, 0U, __ATOMIC_RELEASE);
}

/* Whether a blit is running on another processor right now, for the paths that
   would rather skip a frame than wait for one. */
static int drm_is_busy(void) {
    return __atomic_load_n(&drm_busy_holder, __ATOMIC_RELAXED) != 0;
}

/* Without a GPU there is no CRTC to reprogram, so presenting means blitting. */
static int present_framebuffer(const struct file *client, uint32_t fb_id) {
    struct drm_framebuffer *fb = client ? framebuffer_of(client, fb_id) : framebuffer_find(fb_id);
    if (!fb) return -ENOENT;
    struct drm_dumb_buffer *buffer = buffer_find(fb->handle);
    if (!buffer) return -ENOENT;

    /* Take the display from the console first, or the two fight over every frame. */
    int status = framebuffer_claim_graphics(&drm_display_owner);
    if (status != 0) return status;

    /* Switched away, so it keeps drawing, nothing reaches the screen, and
       this succeeds. */
    if (!framebuffer_graphics_foreground(&drm_display_owner)) return 0;

    /* The same bargain as the blit below: the host is waited on with the kernel
       lock given up, and drm_enter() is what keeps a second processor out. */
    if (virtgpu_available()) {
        int gpu_released = kernel_lock_release_for_wait();
        int shown = present_via_virtgpu(fb, buffer) == 0;
        kernel_lock_retake_after_wait(gpu_released);
        if (shown) return 0;
    }

    uint8_t *scanout = framebuffer_scanout();
    if (!scanout) return -EPERM;

    uint32_t screen_height = framebuffer_height();
    uint32_t screen_pitch = framebuffer_pitch();
    uint32_t rows = fb->height < screen_height ? fb->height : screen_height;
    uint32_t row_bytes = fb->pitch < screen_pitch ? fb->pitch : screen_pitch;
    uint64_t started_ns = time_uptime_ns();
    /* The copy itself, with the kernel lock given up: drm_enter() is already
       held, so nothing can free the buffer under it, and everything else --
       the tick, the keyboard, another processor's syscall -- runs. */
    int released = kernel_lock_release_for_wait();
    for (uint32_t row = 0; row < rows; row++) {
        uint64_t source_offset = (uint64_t)row * fb->pitch;
        uint64_t page = source_offset / 4096ULL;
        uint64_t within = source_offset % 4096ULL;
        uint8_t *destination = scanout + (uint64_t)row * screen_pitch;
        uint32_t copied = 0;
        while (copied < row_bytes && page < buffer->page_count) {
            uint64_t chunk = 4096ULL - within;
            if (chunk > row_bytes - copied) chunk = row_bytes - copied;
            memcpy(destination + copied,
                   (uint8_t *)vmm_phys_to_virt(buffer->pages[page]) + within,
                   (size_t)chunk);
            copied += (uint32_t)chunk;
            page++;
            within = 0;
        }
        /* Interrupts are off, so a shootdown asked for by another processor is
           only answered here. */
        kernel_lock_wait_tick();
    }
    kernel_lock_retake_after_wait(released);
    framebuffer_present();
    /* How fast the scanout actually takes a whole frame, said a few times:
       a blit that runs at uncached speed and one that runs at write-combining
       speed differ by more than a factor of ten, and only the machine knows
       which it got. */
    {
        static unsigned reported;
        if (reported < 4U) {
            reported++;
            uint64_t elapsed = time_uptime_ns() - started_ns;
            uint64_t bytes = (uint64_t)rows * row_bytes;
            kprintf("DRM: present %u rows of %u bytes in %u us (%u MB/s)\n",
                    (unsigned)rows, (unsigned)row_bytes,
                    (unsigned)(elapsed / 1000ULL),
                    (unsigned)(elapsed ? bytes * 1000ULL / elapsed : 0));
        }
    }
    return 0;
}

static int64_t ioctl_set_crtc(const struct file *client, uint64_t user_argument) {
    struct drm_mode_crtc crtc;
    if (copy_from_user(&crtc, user_argument, sizeof(crtc)) != 0) return -EFAULT;
    if (crtc.crtc_id != DRM_CRTC_ID) return -ENOENT;

    /* fb_id 0 means "turn the output off". We stop presenting and give the
       display back, so the console reappears instead of the last frame. */
    if (!crtc.fb_id) {
        active_fb_id = 0;
        virtgpu_scanout_disable();
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    }
    int status = present_framebuffer(client, crtc.fb_id);
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

/* A display that is a copy needs a dirty flush to mean re-presenting. */
static int64_t ioctl_dirty_fb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_dirty_cmd cmd;
    if (copy_from_user(&cmd, user_argument, sizeof(cmd)) != 0) return -EFAULT;
    return present_framebuffer(client, cmd.fb_id);
}

/* The flip has already been presented by the time this runs, so the completion
   is reported with the current time and simply queued. */
static void queue_flip_event(uint64_t user_data) {
    if (event_count == DRM_MAX_EVENTS) {
        /* A reader that never drains would otherwise block flips forever;
           dropping the oldest keeps the newest frame's completion. */
        event_head = (event_head + 1U) % DRM_MAX_EVENTS;
        event_count--;
    }
    uint64_t now = time_uptime_ns();
    struct drm_event_vblank *event = &events[event_tail];
    memset(event, 0, sizeof(*event));
    event->base.type = DRM_EVENT_FLIP_COMPLETE;
    event->base.length = sizeof(*event);
    event->user_data = user_data;
    event->tv_sec = (uint32_t)(now / 1000000000ULL);
    event->tv_usec = (uint32_t)((now % 1000000000ULL) / 1000ULL);
    event->sequence = ++flip_sequence;
    event->crtc_id = DRM_CRTC_ID;
    event_tail = (event_tail + 1U) % DRM_MAX_EVENTS;
    event_count++;
}

/* Whole events, oldest first, because a partial event is never returned. */
int64_t drm_device_read(struct vfs_node *node, uint64_t offset,
                        size_t size, void *buffer) {
    (void)node;
    (void)offset;
    if (!buffer) return -EINVAL;
    size_t produced = 0;
    uint8_t *out = (uint8_t *)buffer;
    while (event_count && size - produced >= sizeof(struct drm_event_vblank)) {
        memcpy(out + produced, &events[event_head], sizeof(struct drm_event_vblank));
        event_head = (event_head + 1U) % DRM_MAX_EVENTS;
        event_count--;
        produced += sizeof(struct drm_event_vblank);
    }
    /* No events yet is "try again", not end of file: the caller is polling. */
    if (!produced) return -EAGAIN;
    return (int64_t)produced;
}

int drm_device_read_ready(struct vfs_node *node) {
    (void)node;
    return event_count != 0;
}

static int64_t ioctl_page_flip(const struct file *client, uint64_t user_argument) {
    struct drm_mode_crtc_page_flip flip;
    if (copy_from_user(&flip, user_argument, sizeof(flip)) != 0) return -EFAULT;
    if (flip.crtc_id != DRM_CRTC_ID) return -ENOENT;
    int status = present_framebuffer(client, flip.fb_id);
    if (status != 0) return status;
    active_fb_id = flip.fb_id;
    if (flip.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(flip.user_data);
    return 0;
}

static int64_t ioctl_create_dumb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_create_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (!request.width || !request.height || request.bpp != 32) return -EINVAL;
    /* Bounded before they are multiplied, because width * 4 * height
       overflows 64 bits. */
    if (request.width > DRM_MAX_DIMENSION || request.height > DRM_MAX_DIMENSION)
        return -EINVAL;

    uint64_t pitch = (uint64_t)request.width * 4ULL;
    uint64_t size = pitch * request.height;
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (256ULL * 1024ULL * 1024ULL) / 4096ULL) return -EINVAL;

    struct drm_dumb_buffer *slot = NULL;
    uint32_t handle = 0;
    for (int step = 0; step < DRM_MAX_BUFFERS; step++) {
        uint32_t index = (next_handle - 1U + (uint32_t)step) % (uint32_t)DRM_MAX_BUFFERS;
        if (!buffers[index].handle) {
            slot = &buffers[index];
            handle = index + 1U;
            next_handle = (index + 2U) > (uint32_t)DRM_MAX_BUFFERS ? 1U : index + 2U;
            break;
        }
    }
    if (!slot) return -ENOMEM;

    slot->pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!slot->pages) return -ENOMEM;
    memset(slot->pages, 0, page_count * sizeof(uint64_t));

    for (uint64_t index = 0; index < page_count; index++) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) {
            slot->handle = 1; /* so buffer_release frees what we got */
            slot->page_count = index;
            buffer_release(slot);
            return -ENOMEM;
        }
        memset(vmm_phys_to_virt(physical), 0, 4096);
        slot->pages[index] = physical;
    }

    slot->handle = handle;
    slot->owner = client;
    slot->refs = 1;               /* the handle itself */
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
    /* The offset is a token, not a location: mmap turns it back into a handle. */
    request.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_destroy_dumb(const struct file *client, uint64_t user_argument) {
    struct drm_mode_destroy_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, request.handle);
    if (!buffer) return -ENOENT;
    buffer_release(buffer);
    return 0;
}

static int64_t ioctl_add_framebuffer(const struct file *client, uint32_t handle,
                                     uint32_t width, uint32_t height,
                                     uint32_t pitch, uint32_t *fb_id_out) {
    struct drm_dumb_buffer *buffer = buffer_of(client, handle);
    if (!buffer) return -ENOENT;
    struct drm_framebuffer *slot = NULL;
    for (int index = 0; index < DRM_MAX_FRAMEBUFFERS; index++) {
        if (!framebuffers[index].id) { slot = &framebuffers[index]; break; }
    }
    if (!slot) return -ENOMEM;
    /* The framebuffer holds the buffer, or a stale id comes to name new pages. */
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
    int64_t status = ioctl_add_framebuffer(client, request.handle, request.width,
                                           request.height, request.pitch,
                                           &request.fb_id);
    if (status != 0) return status;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_addfb2(const struct file *client, uint64_t user_argument) {
    struct drm_mode_fb_cmd2 request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    /* Single-plane formats only; there is no YUV path here. */
    if (request.handles[1] || request.handles[2] || request.handles[3]) return -EINVAL;
    int64_t status = ioctl_add_framebuffer(client, request.handles[0], request.width,
                                           request.height, request.pitches[0],
                                           &request.fb_id);
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
    struct drm_dumb_buffer *buffer = buffer_of(client, request.handle);
    if (!buffer) return -ENOENT;
    buffer_release(buffer);
    return 0;
}

/* --- rendering: what mesa's virgl driver talks to, and only where virgl exists. */

/* One host context per thread group, torn down when the last descriptor goes. */
static uint32_t render_context(void) {
    if (!virtgpu_virgl_available()) return 0;
    struct process *process = process_current();
    if (!process) return 0;

    for (int index = 0; index < DRM_MAX_CONTEXTS; index++) {
        if (render_contexts[index].context &&
            render_contexts[index].pid == process->pid)
            return render_contexts[index].context;
    }
    for (int index = 0; index < DRM_MAX_CONTEXTS; index++) {
        if (render_contexts[index].context) continue;
        uint32_t context = next_render_context;
        if (virtgpu_context_create(context, "tunix") != 0) return 0;
        next_render_context++;
        render_contexts[index].pid = process->pid;
        render_contexts[index].context = context;
        return context;
    }
    return 0;
}

static void render_contexts_release(void) {
    for (int index = 0; index < DRM_MAX_CONTEXTS; index++) {
        if (!render_contexts[index].context) continue;
        virtgpu_context_destroy(render_contexts[index].context);
        render_contexts[index].context = 0;
        render_contexts[index].pid = 0;
    }
}

/* Pages for a resource: the guest's staging side, and a handle to call it by. */
static struct drm_dumb_buffer *buffer_new(const struct file *client, uint64_t size) {
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (256ULL * 1024ULL * 1024ULL) / 4096ULL) return NULL;

    struct drm_dumb_buffer *slot = NULL;
    uint32_t handle = 0;
    for (int step = 0; step < DRM_MAX_BUFFERS; step++) {
        uint32_t index = (next_handle - 1U + (uint32_t)step) % (uint32_t)DRM_MAX_BUFFERS;
        if (!buffers[index].handle) {
            slot = &buffers[index];
            handle = index + 1U;
            next_handle = (index + 2U) > (uint32_t)DRM_MAX_BUFFERS ? 1U : index + 2U;
            break;
        }
    }
    if (!slot) return NULL;

    slot->pages = (uint64_t *)kmalloc(page_count * sizeof(uint64_t));
    if (!slot->pages) return NULL;
    memset(slot->pages, 0, page_count * sizeof(uint64_t));

    for (uint64_t index = 0; index < page_count; index++) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) {
            slot->handle = 1;   /* so the release frees what was got */
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
    case VIRTGPU_PARAM_HOST_VISIBLE:
    case VIRTGPU_PARAM_CROSS_DEVICE:
    case VIRTGPU_PARAM_CONTEXT_INIT: answer = 0; break;
    /* One bit per capset the host published, which stops mesa probing for the rest. */
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS:
        answer = virtgpu_capset_id() ? (1U << virtgpu_capset_id()) : 0;
        break;
    /* An unknown parameter is answered no rather than refused. */
    default: answer = 0; break;
    }
    return copy_to_user(query.value, &answer, sizeof(answer)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_get_caps(uint64_t user_argument) {
    struct drm_virtgpu_get_caps query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.addr || !query.size) return -EINVAL;
    /* Only the capset the host published, because any other answer is read
       as a promise. */
    if (query.cap_set_id != virtgpu_capset_id()) return -EINVAL;

    /* Both sides expect the shorter of the two lengths. */
    uint32_t size = virtgpu_capset_size();
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

    /* The size mesa asked for, not the size of the pages it landed in. */
    uint32_t resource = virtgpu_resource_create_3d(&spec, buffer->pages,
                                                   buffer->page_count,
                                                   query.size);
    if (!resource) {
        buffer_release(buffer);
        return -ENOMEM;
    }
    /* Until it is attached, naming this resource in a command buffer is a
       protocol error the host refuses the whole submission for. */
    if (virtgpu_context_attach(context, resource, 1) != 0) {
        virtgpu_resource_destroy(resource);
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

static int64_t ioctl_virtgpu_resource_info(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_resource_info query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.bo_handle);
    if (!buffer) return -ENOENT;

    query.res_handle = buffer->virtio_resource;
    query.size = (uint32_t)buffer->size;
    query.blob_mem = 0;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_map(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_map query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_of(client, query.handle);
    if (!buffer) return -ENOENT;
    /* The same token MAP_DUMB hands out, and mmap turns either back into the
       same buffer. There is one offset space and one kind of object in it. */
    query.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_transfer(const struct file *client, uint64_t user_argument, int to_host) {
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
    if (virtgpu_transfer_3d(render_context(), buffer->virtio_resource, &box,
                            query.offset, query.level, query.stride,
                            query.layer_stride, to_host) != 0) return -EIO;
    return 0;
}

/* A generous ceiling on one submission, because mesa packs uploads inline. */
#define DRM_MAX_COMMAND_BYTES (1024U * 1024U)

static int64_t ioctl_virtgpu_execbuffer(uint64_t user_argument) {
    struct drm_virtgpu_execbuffer query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.command || !query.size) return -EINVAL;
    if (query.size > DRM_MAX_COMMAND_BYTES) return -EINVAL;

    uint32_t context = render_context();
    if (!context) return -ENODEV;

    void *staging = kmalloc(query.size);
    if (!staging) return -ENOMEM;
    if (copy_from_user(staging, query.command, query.size) != 0) {
        kfree(staging);
        return -EFAULT;
    }
    /* The handles are not looked at: the submission has finished by the
       time this returns. */
    int submitted = virtgpu_submit_3d(context, staging, query.size);
    kfree(staging);
    if (submitted != 0) return -EIO;

    /* Nothing was left outstanding, so there is nothing to wait on. */
    query.fence_fd = -1;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

/* Idle always, because submission does not return until the host has finished. */
static int64_t ioctl_virtgpu_wait(const struct file *client, uint64_t user_argument) {
    struct drm_virtgpu_3d_wait query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!buffer_of(client, query.handle)) return -ENOENT;
    return virtgpu_flush_pending() == 0 ? 0 : -EBUSY;
}

int64_t drm_file_ioctl(struct file *file, unsigned long request,
                       uint64_t user_argument) {
    if (!drm_ready) return -ENOTTY;
    if (IOCTL_TYPE(request) != (unsigned)DRM_IOCTL_TYPE) return -ENOTTY;
    /* Which request, so a lock held for a fifth of a second has a name; see
       /proc/klock. */
    klock_note(KLOCK_NOTE_IOCTL | (uint32_t)IOCTL_NR(request));
    drm_enter();
    int64_t answer = drm_dispatch_ioctl(file, request, user_argument);
    drm_leave();
    return answer;
}

static int64_t drm_dispatch_ioctl(struct file *file, unsigned long request,
                                  uint64_t user_argument) {

    switch (IOCTL_NR(request)) {
    case DRM_NR_VERSION: return ioctl_version(user_argument);
    case DRM_NR_GET_CAP: return ioctl_get_cap(user_argument);
    case DRM_NR_SET_CLIENT_CAP: return ioctl_set_client_cap(user_argument);
    case DRM_NR_SET_VERSION: return 0;
    /* No authentication to do, but dropping master does hand the display back. */
    case DRM_NR_DROP_MASTER:
        active_fb_id = 0;
        virtgpu_scanout_disable();
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    case DRM_NR_SET_MASTER:
    case DRM_NR_GET_MAGIC:
    case DRM_NR_AUTH_MAGIC: return 0;
    case DRM_NR_PRIME_HANDLE_TO_FD: return ioctl_prime_handle_to_fd(file, user_argument);
    case DRM_NR_PRIME_FD_TO_HANDLE: return ioctl_prime_fd_to_handle(user_argument);
    case DRM_NR_MODE_GETPLANERESOURCES: return ioctl_get_plane_resources(user_argument);
    case DRM_NR_MODE_GETPLANE: return ioctl_get_plane(user_argument);
    case DRM_NR_MODE_GETPROPERTY: return ioctl_get_property(user_argument);
    case DRM_NR_MODE_OBJ_GETPROPERTIES: return ioctl_obj_get_properties(user_argument);
    /* Every property here is immutable and the one plane cannot be moved. */
    case DRM_NR_MODE_SETPROPERTY:
    case DRM_NR_MODE_OBJ_SETPROPERTY:
    case DRM_NR_MODE_SETPLANE: return -EINVAL;
    case DRM_NR_MODE_GETRESOURCES: return ioctl_get_resources(user_argument);
    case DRM_NR_MODE_GETCONNECTOR: return ioctl_get_connector(user_argument);
    case DRM_NR_MODE_GETENCODER: return ioctl_get_encoder(user_argument);
    case DRM_NR_MODE_GETCRTC: return ioctl_get_crtc(user_argument);
    case DRM_NR_MODE_GETFB: return ioctl_getfb(file, user_argument);
    case DRM_NR_MODE_CURSOR: return ioctl_cursor(file, user_argument, 0);
    case DRM_NR_MODE_CURSOR2: return ioctl_cursor(file, user_argument, 1);
    case DRM_NR_MODE_ATOMIC: return ioctl_atomic(file, user_argument);
    case DRM_NR_MODE_CREATEPROPBLOB: return ioctl_create_blob(file, user_argument);
    case DRM_NR_MODE_DESTROYPROPBLOB: return ioctl_destroy_blob(file, user_argument);
    case DRM_NR_MODE_SETCRTC: return ioctl_set_crtc(file, user_argument);
    case DRM_NR_MODE_PAGE_FLIP: return ioctl_page_flip(file, user_argument);
    case DRM_NR_MODE_DIRTYFB: return ioctl_dirty_fb(file, user_argument);
    case DRM_NR_MODE_CREATE_DUMB: return ioctl_create_dumb(file, user_argument);
    case DRM_NR_MODE_MAP_DUMB: return ioctl_map_dumb(file, user_argument);
    case DRM_NR_MODE_DESTROY_DUMB: return ioctl_destroy_dumb(file, user_argument);
    case DRM_NR_MODE_ADDFB: return ioctl_addfb(file, user_argument);
    case DRM_NR_MODE_ADDFB2: return ioctl_addfb2(file, user_argument);
    case DRM_NR_MODE_RMFB: return ioctl_rmfb(file, user_argument);
    /* Dumb buffers are GEM objects, so libdrm frees them either way. */
    case DRM_NR_GEM_CLOSE: return ioctl_gem_close(file, user_argument);

    /* Refused on a device without virgl, where these numbers belong to nobody. */
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
        case DRM_NR_VIRTGPU_RESOURCE_INFO:
            return ioctl_virtgpu_resource_info(file, user_argument);
        case DRM_NR_VIRTGPU_MAP: return ioctl_virtgpu_map(file, user_argument);
        case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
            return ioctl_virtgpu_transfer(file, user_argument, 0);
        case DRM_NR_VIRTGPU_TRANSFER_TO_HOST:
            return ioctl_virtgpu_transfer(file, user_argument, 1);
        case DRM_NR_VIRTGPU_EXECBUFFER: return ioctl_virtgpu_execbuffer(user_argument);
        default: return ioctl_virtgpu_wait(file, user_argument);
        }
    /* Refused on purpose, and GETPARAM has already said neither is here. */
    case DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB:
    case DRM_NR_VIRTGPU_CONTEXT_INIT: return -EINVAL;
    default: return -ENOTTY;
    }
}

/* mmap of a dumb buffer, looked up by the token MAP_DUMB produced. */
int64_t drm_device_mmap(struct vfs_node *node, struct file *file,
                        uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset,
                        uint64_t page_flags) {
    (void)node;
    if (!drm_ready || !length) return -EINVAL;
    if (offset < DRM_MAP_OFFSET_BASE) return -EINVAL;

    uint32_t handle = (uint32_t)((offset - DRM_MAP_OFFSET_BASE) / 4096ULL);
    struct drm_dumb_buffer *buffer = buffer_of(file, handle);
    if (!buffer) return -ENOENT;
    if (length > buffer->size) return -EINVAL;

    uint64_t flags = page_flags | PAGE_USER | PAGE_PRESENT | PAGE_NX;
    uint64_t mapped = 0;
    for (; mapped < length; mapped += 4096ULL) {
        uint64_t physical = buffer->pages[mapped / 4096ULL];
        /* The buffer keeps its own reference; the mapping takes another so the
           pages survive a close with the mapping still live. */
        if (pmm_page_ref(physical) != 0 ||
            vmm_map_page_in(cr3, virtual_address + mapped, physical, flags) != 0) {
            while (mapped) {
                mapped -= 4096ULL;
                (void)vmm_unmap_page_in(cr3, virtual_address + mapped);
                pmm_free_page((void *)buffer->pages[mapped / 4096ULL]);
            }
            return -EINVAL;
        }
    }
    return 0;
}

void drm_device_open(struct vfs_node *node) {
    (void)node;
    open_count++;
}

/* The last descriptor is gone, so the console gets the display back. */
void drm_device_close(struct vfs_node *node) {
    (void)node;
    /* Takes the scanout away, which a blit is writing into. */
    drm_enter();
    if (open_count) open_count--;
    if (!open_count) {
        active_fb_id = 0;
        event_head = event_tail = event_count = 0;
        render_contexts_release();
        virtgpu_scanout_disable();
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
    }
    drm_leave();
}

/* Everything this client made goes with it, and the pages are reference counted. */
void drm_file_close(struct file *file) {
    if (!file) return;
    /* Frees the very buffers a blit may be reading. */
    drm_enter();
    for (int index = 0; index < DRM_MAX_FRAMEBUFFERS; index++) {
        if (!framebuffers[index].id || framebuffers[index].owner != file) continue;
        if (active_fb_id == framebuffers[index].id) active_fb_id = 0;
        struct drm_dumb_buffer *buffer = buffer_find(framebuffers[index].handle);
        memset(&framebuffers[index], 0, sizeof(framebuffers[index]));
        if (buffer) buffer_release(buffer);
    }
    for (unsigned index = 0; index < DRM_MAX_BLOBS; index++)
        if (blobs[index].id && blobs[index].owner == file)
            memset(&blobs[index], 0, sizeof(blobs[index]));
    for (int index = 0; index < DRM_MAX_BUFFERS; index++)
        if (buffers[index].handle && buffers[index].owner == file)
            buffer_release(&buffers[index]);
    drm_leave();
}

/* Switched away, so a virtio-gpu's scanout has to be handed back explicitly. */
void drm_display_suspend(void) {
    if (!drm_ready) return;
    drm_console_present();
}

/* The console is scanned out like any other buffer, and re-sent as it changes. */
void drm_console_present(void) {
    if (!virtgpu_available()) return;
    /* Thirty times a second from the tick: a frame skipped while a client's
       blit is in flight costs nothing, and waiting would put the stall back. */
    if (drm_is_busy()) return;
    uint32_t pitch = framebuffer_pitch();
    if (!pitch) return;
    (void)virtgpu_console_present(framebuffer_physical_address() +
                                      framebuffer_memory_offset(),
                                  pitch / 4U, framebuffer_width(),
                                  framebuffer_height());
}

void drm_display_resume(void) {
    if (!drm_ready || !active_fb_id) return;
    drm_enter();
    (void)present_framebuffer(NULL, active_fb_id);
    drm_leave();
}
