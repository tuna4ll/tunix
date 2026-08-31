#include <stddef.h>
#include <stdint.h>
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
#define ENODEV 19

/* DRM_CLOEXEC in <drm/drm.h> is O_CLOEXEC by another name. */
#define DRM_CLOEXEC 02000000

/*
 * See include/drm.h for what this is and is not. The ABI below is Linux's, from
 * <drm/drm.h> and <drm/drm_mode.h>; userspace reaches it through libdrm, so the
 * layouts have to match exactly and are asserted where it matters.
 */

/* ioctls are decoded rather than matched against fully encoded constants: the
   number is the stable part, while the size field differs between libdrm
   versions and between 32- and 64-bit callers. */
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
#define DRM_NR_MODE_ADDFB 0xae
#define DRM_NR_MODE_RMFB 0xaf
#define DRM_NR_MODE_PAGE_FLIP 0xb0
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

/*
 * The driver-private range. Every DRM driver puts its own calls here, and
 * which driver's they are is decided by the name VERSION reports -- so these
 * numbers mean what virtio_gpu means by them, and only because we answer to
 * that name.
 */
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

/*
 * What mesa asks about before it decides how to talk to us. Answering 3D
 * FEATURES with anything but 1 makes it give up and fall back to llvmpipe,
 * which is the failure this whole path exists to end.
 *
 * CAPSET_QUERY_FIX says a capset may be asked for by id rather than by index,
 * which every mesa in living memory assumes. The rest name features this
 * driver does not have -- blob resources, host-visible memory, cross-device
 * sharing -- and answering zero is how mesa is told to use the older paths
 * that only need what is here.
 */
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

/*
 * `value` is where to put the answer, not the answer.
 *
 * It is a pointer into the caller's memory, and the answer written through it
 * is four bytes wide however wide the field is. Filling the field in instead
 * leaves the caller reading the zero it started with -- which for the very
 * first question, whether there is 3D at all, reads as no, and mesa quietly
 * goes back to the software rasteriser without ever saying why.
 */
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

/*
 * The one property this device exposes.
 *
 * Weston refuses to run without DRM_CLIENT_CAP_UNIVERSAL_PLANES, and a universal
 * plane is only usable if its "type" says what it is -- weston reads that
 * property and bails out on a plane whose type it cannot resolve. So a single
 * enum property, on a single primary plane, is the whole property system here.
 */
#define DRM_PROP_TYPE_ID 10

#define DRM_MODE_OBJECT_CRTC 0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_ENCODER 0xe0e0e0e0
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee

#define DRM_MODE_PROP_IMMUTABLE (1 << 2)
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

typedef char drm_modeinfo_size_check[
    (sizeof(struct drm_mode_modeinfo) == 68) ? 1 : -1];
typedef char drm_create_dumb_size_check[
    (sizeof(struct drm_mode_create_dumb) == 32) ? 1 : -1];

/* --- objects ------------------------------------------------------------ */

/*
 * A compositor holds a handful of buffers. A GL client holds one per texture,
 * vertex buffer and render target it has live, which for a game is hundreds --
 * so this ceiling stopped being generous the moment mesa started allocating
 * through it.
 */
#define DRM_MAX_BUFFERS 512
#define DRM_MAX_FRAMEBUFFERS 64

/*
 * A dumb buffer: a run of ordinary pages that userspace maps and draws into.
 * `map_offset` is the fake mmap offset MAP_DUMB hands out; it is just the
 * handle scaled by a page so the offset alone identifies the buffer.
 */
struct drm_dumb_buffer {
    uint32_t handle;      /* 0 when the slot is free */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;        /* page-aligned byte count */
    uint64_t page_count;
    uint64_t *pages;      /* physical addresses */
    /* The host resource these pages back, created on first present and 0 on a
       machine with no virtio-gpu. */
    uint32_t virtio_resource;
    /* Set when the resource was made by RESOURCE_CREATE rather than grown out
       of a dumb buffer: its contents are the host's, and the guest pages are a
       staging area that transfers move to and from on demand. */
    uint8_t rendered;
    /* Holders beyond the handle itself: every PRIME descriptor exported from
       this buffer counts. DESTROY_DUMB drops the handle's reference, but the
       pages stay until the last descriptor is closed. */
    uint32_t refs;
};

struct drm_framebuffer {
    uint32_t id;          /* 0 when the slot is free */
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
};

/*
 * Completion events, read back off the device descriptor.
 *
 * A compositor flips with DRM_MODE_PAGE_FLIP_EVENT and then waits for the
 * completion before drawing the next frame; weston does this every frame, so
 * without a queue it stalls forever. Presentation here is synchronous -- the
 * flip is a blit that has already finished by the time the ioctl returns -- so
 * the event is queued immediately and the reader never actually waits.
 */
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

/*
 * Display arbitration.
 *
 * The console and DRM draw into the same scanout, so one of them has to stand
 * down. `drm_display_owner` is nothing but a unique address to hand
 * framebuffer_claim_graphics() as an identity; `open_count` tracks how many
 * descriptors are open on the card so the console can come back when the last
 * one goes away -- which is what makes weston exiting leave a usable shell
 * rather than a frozen picture.
 */
static const char drm_display_owner;
static uint32_t open_count;

static struct drm_dumb_buffer buffers[DRM_MAX_BUFFERS];
static struct drm_framebuffer framebuffers[DRM_MAX_FRAMEBUFFERS];
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
    next_handle = 1;
    next_fb_id = 1;
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
    if (!handle) return NULL;
    for (int index = 0; index < DRM_MAX_BUFFERS; index++) {
        if (buffers[index].handle == handle) return &buffers[index];
    }
    return NULL;
}

static struct drm_framebuffer *framebuffer_find(uint32_t id) {
    if (!id) return NULL;
    for (int index = 0; index < DRM_MAX_FRAMEBUFFERS; index++) {
        if (framebuffers[index].id == id) return &framebuffers[index];
    }
    return NULL;
}

/*
 * Drop one reference. The buffer's memory goes away with the last one, which is
 * not necessarily the handle: an exported PRIME descriptor keeps it alive after
 * DESTROY_DUMB, which is the entire point of exporting it.
 */
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

/*
 * One mode, describing the display exactly as it already is. The timings are
 * synthesised: there is no real CRTC to program, and userspace only reads them
 * to pick a mode.
 */
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

    /*
     * The name is not a label, it is the driver's identity: mesa looks up
     * `<name>_dri.so` by it and speaks that driver's private ioctls at us. So
     * it is only virtio_gpu where the host has actually granted virgl -- claim
     * it on a 2D device and mesa would load the virgl driver and then find
     * none of the calls it needs.
     */
    int rendering = virtgpu_virgl_available();
    static const char virtio_name[] = "virtio_gpu";
    static const char plain_name[] = "tunixdrm";
    const char *name = rendering ? virtio_name : plain_name;
    size_t name_size = (rendering ? sizeof(virtio_name) : sizeof(plain_name)) - 1;
    static const char date[] = "20260721";
    static const char desc[] = "Tunix framebuffer KMS";

    /*
     * mesa turns these into a feature level rather than reading them as a
     * version, so they are a statement about what this driver can do.
     *
     * Minor 1 is where linux's virtio_gpu started handing out sync
     * descriptors for a submission, and mesa reads that as permission to ask
     * for one and then wait on it. Submitting here is synchronous -- the work
     * is done before the call returns -- so there is no descriptor to give,
     * and saying minor 0 is how mesa is told not to ask. Claiming minor 1 and
     * answering with no descriptor leaves mesa polling -1 for ever, which
     * looks from outside like a compositor that renders one frame and then
     * stops.
     */
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
    /*
     * Buffer sharing by descriptor, both directions. This is not decoration:
     * mesa reads this exact capability to decide how GBM allocates. Reporting
     * zero makes it fall back to a path whose buffers have no DRI image, and it
     * then dereferences that NULL itself.
     */
    case DRM_CAP_PRIME: cap.value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; break;
    /* One linear layout and nothing to negotiate. */
    case DRM_CAP_ADDFB2_MODIFIERS:
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

/*
 * Planes.
 *
 * There is exactly one, the primary plane of the only CRTC, and it cannot be
 * moved, scaled or composited -- SETPLANE is refused. It exists because
 * DRM_CLIENT_CAP_UNIVERSAL_PLANES is not optional for weston: it enumerates
 * planes, and a plane whose "type" property it cannot read is a fatal error.
 */
/* --- PRIME ---------------------------------------------------------------
 *
 * Exporting a buffer as a descriptor. There is no separate dma-buf object here:
 * the descriptor simply names a dumb buffer and holds a reference to it, which
 * is enough for what mesa does with it -- allocate through GBM, hand the buffer
 * to another process or map it, and hand the handle back later.
 *
 * The handle namespace is per-device rather than per-file, so importing a
 * descriptor gives back the handle it was exported from.
 */
void drm_buffer_put(uint32_t handle) {
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (buffer) buffer_release(buffer);
}

static int64_t ioctl_prime_handle_to_fd(uint64_t user_argument) {
    struct drm_prime_handle request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(request.handle);
    if (!buffer) return -ENOENT;
    if (buffer->refs == 0xFFFFFFFFU) return -EMFILE;

    buffer->refs++;
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

    /*
     * A resource arriving from another process has to be granted to this one's
     * context before it may be named in a command buffer.
     *
     * This is the compositor's side of a client handing over a frame: the
     * client rendered into a host resource and passed the descriptor along,
     * and without this the host refuses every command that mentions it. The
     * client, waiting to be told its buffer was used, waits forever -- which
     * is what a hung GL client on an otherwise working compositor looks like.
     */
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
    uint32_t ids[1];
    uint64_t values[1];
    if (request.obj_type == DRM_MODE_OBJECT_PLANE) {
        if (request.obj_id != DRM_PLANE_ID) return -ENOENT;
        ids[0] = DRM_PROP_TYPE_ID;
        values[0] = DRM_PLANE_TYPE_PRIMARY;
        count = 1;
    } else if (request.obj_type == DRM_MODE_OBJECT_CRTC) {
        if (request.obj_id != DRM_CRTC_ID) return -ENOENT;
    } else if (request.obj_type == DRM_MODE_OBJECT_CONNECTOR) {
        if (request.obj_id != DRM_CONNECTOR_ID) return -ENOENT;
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

/*
 * With a virtio-gpu behind the display there is a real scanout to point at, so
 * the buffer's own pages become the host resource and presenting costs three
 * commands instead of a screenful of memcpy.
 */
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

/*
 * Present a framebuffer. Without a GPU the display is a fixed region the
 * bootloader handed over and there is no CRTC to reprogram, so presenting
 * means blitting into it.
 */
static int present_framebuffer(uint32_t fb_id) {
    struct drm_framebuffer *fb = framebuffer_find(fb_id);
    if (!fb) return -ENOENT;
    struct drm_dumb_buffer *buffer = buffer_find(fb->handle);
    if (!buffer) return -ENOENT;

    /* Take the display away from the text console before touching a pixel:
       otherwise the console keeps writing into the same scanout and the two
       fight over every frame. Claiming here rather than at open() means a
       client that only queries the device leaves the console alone. */
    int status = framebuffer_claim_graphics(&drm_display_owner);
    if (status != 0) return status;

    /*
     * The client is on a virtual terminal the user has switched away from. It
     * keeps drawing -- a compositor has no reason to stop, and stopping it is
     * not this driver's business -- but none of it reaches the screen, and the
     * call still succeeds: a frame that was refused would be reported as a
     * device error, and the last one presented goes back up on the switch back.
     */
    if (!framebuffer_graphics_foreground(&drm_display_owner)) return 0;

    if (virtgpu_available() && present_via_virtgpu(fb, buffer) == 0) return 0;

    uint8_t *scanout = framebuffer_scanout();
    if (!scanout) return -EPERM;

    uint32_t screen_height = framebuffer_height();
    uint32_t screen_pitch = framebuffer_pitch();
    uint32_t rows = fb->height < screen_height ? fb->height : screen_height;
    uint32_t row_bytes = fb->pitch < screen_pitch ? fb->pitch : screen_pitch;
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
    }
    framebuffer_present();
    return 0;
}

static int64_t ioctl_set_crtc(uint64_t user_argument) {
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
    int status = present_framebuffer(crtc.fb_id);
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

/*
 * DIRTYFB flushes a framebuffer whose contents userspace changed in place.
 * Drivers that scan out the buffer directly need do nothing, but this display
 * is a copy, so a dirty flush means re-presenting. The Xorg modesetting DDX
 * (no glamor, no ShadowFB) draws straight into its dumb buffer and relies on
 * this to reach the screen; the clip rectangles are only a hint, so copy whole.
 */
static int64_t ioctl_dirty_fb(uint64_t user_argument) {
    struct drm_mode_fb_dirty_cmd cmd;
    if (copy_from_user(&cmd, user_argument, sizeof(cmd)) != 0) return -EFAULT;
    return present_framebuffer(cmd.fb_id);
}

#define DRM_MODE_PAGE_FLIP_EVENT 0x01

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

/*
 * read() on the device hands back whole events, oldest first. Partial events
 * are never returned: DRM's contract is that a reader with room for one event
 * gets exactly one, and a reader with no room gets nothing.
 */
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

static int64_t ioctl_page_flip(uint64_t user_argument) {
    struct drm_mode_crtc_page_flip flip;
    if (copy_from_user(&flip, user_argument, sizeof(flip)) != 0) return -EFAULT;
    if (flip.crtc_id != DRM_CRTC_ID) return -ENOENT;
    int status = present_framebuffer(flip.fb_id);
    if (status != 0) return status;
    active_fb_id = flip.fb_id;
    if (flip.flags & DRM_MODE_PAGE_FLIP_EVENT) queue_flip_event(flip.user_data);
    return 0;
}

static int64_t ioctl_create_dumb(uint64_t user_argument) {
    struct drm_mode_create_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    if (!request.width || !request.height || request.bpp != 32) return -EINVAL;

    uint64_t pitch = (uint64_t)request.width * 4ULL;
    uint64_t size = pitch * request.height;
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (256ULL * 1024ULL * 1024ULL) / 4096ULL) return -EINVAL;

    struct drm_dumb_buffer *slot = NULL;
    for (int index = 0; index < DRM_MAX_BUFFERS; index++) {
        if (!buffers[index].handle) { slot = &buffers[index]; break; }
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

    slot->handle = next_handle++;
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

static int64_t ioctl_map_dumb(uint64_t user_argument) {
    struct drm_mode_map_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(request.handle);
    if (!buffer) return -ENOENT;
    /* The offset is a token, not a location: mmap turns it back into a handle. */
    request.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_destroy_dumb(uint64_t user_argument) {
    struct drm_mode_destroy_dumb request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(request.handle);
    if (!buffer) return -ENOENT;
    buffer_release(buffer);
    return 0;
}

static int64_t ioctl_add_framebuffer(uint32_t handle, uint32_t width, uint32_t height,
                                     uint32_t pitch, uint32_t *fb_id_out) {
    struct drm_dumb_buffer *buffer = buffer_find(handle);
    if (!buffer) return -ENOENT;
    struct drm_framebuffer *slot = NULL;
    for (int index = 0; index < DRM_MAX_FRAMEBUFFERS; index++) {
        if (!framebuffers[index].id) { slot = &framebuffers[index]; break; }
    }
    if (!slot) return -ENOMEM;
    slot->id = next_fb_id++;
    slot->handle = handle;
    slot->width = width;
    slot->height = height;
    slot->pitch = pitch ? pitch : buffer->pitch;
    *fb_id_out = slot->id;
    return 0;
}

static int64_t ioctl_addfb(uint64_t user_argument) {
    struct drm_mode_fb_cmd request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    int64_t status = ioctl_add_framebuffer(request.handle, request.width,
                                           request.height, request.pitch,
                                           &request.fb_id);
    if (status != 0) return status;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_addfb2(uint64_t user_argument) {
    struct drm_mode_fb_cmd2 request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    /* Single-plane formats only; there is no YUV path here. */
    if (request.handles[1] || request.handles[2] || request.handles[3]) return -EINVAL;
    int64_t status = ioctl_add_framebuffer(request.handles[0], request.width,
                                           request.height, request.pitches[0],
                                           &request.fb_id);
    if (status != 0) return status;
    return copy_to_user(user_argument, &request, sizeof(request)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_rmfb(uint64_t user_argument) {
    uint32_t fb_id;
    if (copy_from_user(&fb_id, user_argument, sizeof(fb_id)) != 0) return -EFAULT;
    struct drm_framebuffer *fb = framebuffer_find(fb_id);
    if (!fb) return -ENOENT;
    if (active_fb_id == fb_id) active_fb_id = 0;
    memset(fb, 0, sizeof(*fb));
    return 0;
}

static int64_t ioctl_gem_close(uint64_t user_argument) {
    struct drm_gem_close request;
    if (copy_from_user(&request, user_argument, sizeof(request)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(request.handle);
    if (!buffer) return -ENOENT;
    buffer_release(buffer);
    return 0;
}

/* The VFS calls ioctl with the node, not the file; nothing here is per-file
   yet, so the node form is the one devfs installs. */
int64_t drm_node_ioctl(struct vfs_node *node, unsigned long request,
                       uint64_t user_argument) {
    (void)node;
    return drm_file_ioctl(NULL, request, user_argument);
}

/* --- rendering ------------------------------------------------------------
 *
 * Everything below exists so that mesa's virgl driver has something to talk
 * to. It is the render half of virtio_gpu: resources that live on the host,
 * and command buffers that tell the host what to draw into them.
 *
 * None of it is reachable unless VERSION said virtio_gpu, which it only does
 * when the host granted virgl -- so a machine without one behaves exactly as
 * it did before any of this existed.
 */

/*
 * One host context per process.
 *
 * A context is a client's GL state, and two clients sharing one would see each
 * other's. The process is the right grain: mesa opens the device once per
 * screen and a process has one screen. Two descriptors in the same process
 * share a context, which is what they would want anyway.
 *
 * They are all torn down when the last descriptor on the card closes. Nothing
 * here can see a process exit, so this is the one moment we are told that
 * clients have gone -- and by then none of the contexts can still be in use.
 */
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

/*
 * Pages for a resource, and a handle to call them by.
 *
 * The pages are the guest's side of a host resource: a staging area transfers
 * move bytes through, not the resource itself. They are allocated even for a
 * resource the guest will never look at, because the host has to be told where
 * its backing is before it will accept one at all.
 */
static struct drm_dumb_buffer *buffer_new(uint64_t size) {
    size = (size + 4095ULL) & ~4095ULL;
    uint64_t page_count = size / 4096ULL;
    if (!page_count || page_count > (256ULL * 1024ULL * 1024ULL) / 4096ULL) return NULL;

    struct drm_dumb_buffer *slot = NULL;
    for (int index = 0; index < DRM_MAX_BUFFERS; index++) {
        if (!buffers[index].handle) { slot = &buffers[index]; break; }
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

    slot->handle = next_handle++;
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
    /*
     * One bit per capset the host published, which for us is the one. Saying
     * so here is what stops mesa asking for the others: it probes for the
     * native-context capsets first, and a driver that answers those gets
     * driven down a path it cannot follow.
     */
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDS:
        answer = virtgpu_capset_id() ? (1U << virtgpu_capset_id()) : 0;
        break;
    /* An unknown parameter is not an error to answer no to. mesa probes for
       things newer than the driver it is talking to and expects to be told
       they are absent. */
    default: answer = 0; break;
    }
    return copy_to_user(query.value, &answer, sizeof(answer)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_get_caps(uint64_t user_argument) {
    struct drm_virtgpu_get_caps query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.addr || !query.size) return -EINVAL;
    /*
     * Only the capset the host actually published. mesa asks about capsets it
     * hopes for before the one it can definitely use, and any answer but a
     * refusal is taken as "this exists" -- so answering a capset we do not
     * have sends it down a path with nothing at the end of it.
     */
    if (query.cap_set_id != virtgpu_capset_id()) return -EINVAL;

    /* mesa asks for its own idea of how big a capset is, which is bigger than
       the host's whenever the host is older. Both sides expect the shorter of
       the two, and mesa has already zeroed the rest. */
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

static int64_t ioctl_virtgpu_resource_create(uint64_t user_argument) {
    struct drm_virtgpu_resource_create query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!query.size) return -EINVAL;

    uint32_t context = render_context();
    if (!context) return -ENODEV;

    struct drm_dumb_buffer *buffer = buffer_new(query.size);
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

static int64_t ioctl_virtgpu_resource_info(uint64_t user_argument) {
    struct drm_virtgpu_resource_info query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(query.bo_handle);
    if (!buffer) return -ENOENT;

    query.res_handle = buffer->virtio_resource;
    query.size = (uint32_t)buffer->size;
    query.blob_mem = 0;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_map(uint64_t user_argument) {
    struct drm_virtgpu_map query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(query.handle);
    if (!buffer) return -ENOENT;
    /* The same token MAP_DUMB hands out, and mmap turns either back into the
       same buffer. There is one offset space and one kind of object in it. */
    query.offset = DRM_MAP_OFFSET_BASE + (uint64_t)buffer->handle * 4096ULL;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

static int64_t ioctl_virtgpu_transfer(uint64_t user_argument, int to_host) {
    struct drm_virtgpu_3d_transfer query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    struct drm_dumb_buffer *buffer = buffer_find(query.bo_handle);
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

/*
 * A ceiling on one submission, and it has to be a generous one.
 *
 * The guess that mesa's command stream stays inside 64 KiB was wrong: it packs
 * texture uploads into the same buffer, and SuperTuxKart loading a track was
 * measured sending 266224 bytes -- just past a 256 KiB limit, which is the
 * worst place for a limit to be. A rejected submission is not a dropped frame
 * either; mesa prints "expect bad rendering" and carries on with a broken
 * context.
 *
 * A megabyte is four times the largest seen. It costs nothing in the image --
 * the staging buffer is zero-initialised, so it is address space rather than
 * bytes on disk.
 */
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
    /* The handles the submission mentions are not looked at. They exist so a
       driver with a real fence can hold the buffers until the host is done
       with them; here the submission has already finished by the time it
       returns, and nothing can be freed underneath it. */
    int submitted = virtgpu_submit_3d(context, staging, query.size);
    kfree(staging);
    if (submitted != 0) return -EIO;

    /* Nothing was left outstanding, so there is nothing to wait on. */
    query.fence_fd = -1;
    return copy_to_user(user_argument, &query, sizeof(query)) == 0 ? 0 : -EFAULT;
}

/*
 * Wait for a resource to be idle, which it always is: submission does not
 * return until the host has finished, so there is never work outstanding to
 * wait for. This is the whole cost of a synchronous queue, paid here as an
 * answer that is true but was expensive to make true.
 */
static int64_t ioctl_virtgpu_wait(uint64_t user_argument) {
    struct drm_virtgpu_3d_wait query;
    if (copy_from_user(&query, user_argument, sizeof(query)) != 0) return -EFAULT;
    if (!buffer_find(query.handle)) return -ENOENT;
    return 0;
}

int64_t drm_file_ioctl(struct file *file, unsigned long request,
                       uint64_t user_argument) {
    (void)file;
    if (!drm_ready) return -ENOTTY;
    if (IOCTL_TYPE(request) != (unsigned)DRM_IOCTL_TYPE) return -ENOTTY;

    switch (IOCTL_NR(request)) {
    case DRM_NR_VERSION: return ioctl_version(user_argument);
    case DRM_NR_GET_CAP: return ioctl_get_cap(user_argument);
    /* Accepted and ignored: every capability a client can set is one we do not
       implement, and refusing would stop libdrm rather than degrade it. */
    case DRM_NR_SET_CLIENT_CAP: return 0;
    case DRM_NR_SET_VERSION: return 0;
    /* Single-master device with no authentication: there is nothing to hand
       out magic tokens for. Dropping master does mean giving the display back,
       though -- that is how a compositor hands the console over on VT switch. */
    case DRM_NR_DROP_MASTER:
        active_fb_id = 0;
        virtgpu_scanout_disable();
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    case DRM_NR_SET_MASTER:
    case DRM_NR_GET_MAGIC:
    case DRM_NR_AUTH_MAGIC: return 0;
    case DRM_NR_PRIME_HANDLE_TO_FD: return ioctl_prime_handle_to_fd(user_argument);
    case DRM_NR_PRIME_FD_TO_HANDLE: return ioctl_prime_fd_to_handle(user_argument);
    case DRM_NR_MODE_GETPLANERESOURCES: return ioctl_get_plane_resources(user_argument);
    case DRM_NR_MODE_GETPLANE: return ioctl_get_plane(user_argument);
    case DRM_NR_MODE_GETPROPERTY: return ioctl_get_property(user_argument);
    case DRM_NR_MODE_OBJ_GETPROPERTIES: return ioctl_obj_get_properties(user_argument);
    /* Every property here is immutable, and the one plane cannot be moved:
       there is a single scanout and SETCRTC/PAGE_FLIP are the only ways to
       change what is on it. */
    case DRM_NR_MODE_SETPROPERTY:
    case DRM_NR_MODE_OBJ_SETPROPERTY:
    case DRM_NR_MODE_SETPLANE: return -EINVAL;
    case DRM_NR_MODE_GETRESOURCES: return ioctl_get_resources(user_argument);
    case DRM_NR_MODE_GETCONNECTOR: return ioctl_get_connector(user_argument);
    case DRM_NR_MODE_GETENCODER: return ioctl_get_encoder(user_argument);
    case DRM_NR_MODE_GETCRTC: return ioctl_get_crtc(user_argument);
    case DRM_NR_MODE_SETCRTC: return ioctl_set_crtc(user_argument);
    case DRM_NR_MODE_PAGE_FLIP: return ioctl_page_flip(user_argument);
    case DRM_NR_MODE_DIRTYFB: return ioctl_dirty_fb(user_argument);
    case DRM_NR_MODE_CREATE_DUMB: return ioctl_create_dumb(user_argument);
    case DRM_NR_MODE_MAP_DUMB: return ioctl_map_dumb(user_argument);
    case DRM_NR_MODE_DESTROY_DUMB: return ioctl_destroy_dumb(user_argument);
    case DRM_NR_MODE_ADDFB: return ioctl_addfb(user_argument);
    case DRM_NR_MODE_ADDFB2: return ioctl_addfb2(user_argument);
    case DRM_NR_MODE_RMFB: return ioctl_rmfb(user_argument);
    /* Dumb buffers are GEM objects, so libdrm frees them either way. */
    case DRM_NR_GEM_CLOSE: return ioctl_gem_close(user_argument);

    /* The driver-private calls, which mean virtio_gpu's calls only because
       VERSION said that is who we are. On a device without virgl the name is
       different and these numbers belong to nobody, so they are refused the
       same way an unknown ioctl is. */
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
            return ioctl_virtgpu_resource_create(user_argument);
        case DRM_NR_VIRTGPU_RESOURCE_INFO:
            return ioctl_virtgpu_resource_info(user_argument);
        case DRM_NR_VIRTGPU_MAP: return ioctl_virtgpu_map(user_argument);
        case DRM_NR_VIRTGPU_TRANSFER_FROM_HOST:
            return ioctl_virtgpu_transfer(user_argument, 0);
        case DRM_NR_VIRTGPU_TRANSFER_TO_HOST:
            return ioctl_virtgpu_transfer(user_argument, 1);
        case DRM_NR_VIRTGPU_EXECBUFFER: return ioctl_virtgpu_execbuffer(user_argument);
        default: return ioctl_virtgpu_wait(user_argument);
        }
    /* Refused on purpose, and GETPARAM already said so: blob resources and
       explicit context types are how a newer mesa would ask for memory shared
       with the host, which this driver does not have. */
    case DRM_NR_VIRTGPU_RESOURCE_CREATE_BLOB:
    case DRM_NR_VIRTGPU_CONTEXT_INIT: return -EINVAL;
    default: return -ENOTTY;
    }
}

/*
 * mmap of a dumb buffer. The offset is the token MAP_DUMB produced, so the
 * lookup is by handle rather than by address.
 */
int64_t drm_device_mmap(struct vfs_node *node, struct file *file,
                        uint64_t cr3, uint64_t virtual_address,
                        uint64_t length, uint64_t offset,
                        uint64_t page_flags) {
    (void)node;
    (void)file;
    if (!drm_ready || !length) return -EINVAL;
    if (offset < DRM_MAP_OFFSET_BASE) return -EINVAL;

    uint32_t handle = (uint32_t)((offset - DRM_MAP_OFFSET_BASE) / 4096ULL);
    struct drm_dumb_buffer *buffer = buffer_find(handle);
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

/*
 * The last descriptor on the card is gone, so whoever was driving the display
 * is gone with it: hand the scanout back and let the console redraw. Without
 * this, a compositor that exits or crashes leaves its final frame frozen on
 * screen with a live shell invisible underneath it.
 */
void drm_device_close(struct vfs_node *node) {
    (void)node;
    if (open_count) open_count--;
    if (open_count) return;
    active_fb_id = 0;
    event_head = event_tail = event_count = 0;
    render_contexts_release();
    virtgpu_scanout_disable();
    (void)framebuffer_release_graphics(&drm_display_owner, 0);
}

void drm_file_close(struct file *file) {
    (void)file;
    /* Buffers outlive the descriptor deliberately: a mapping may still be in
       use, and the pages are reference counted. Ownership of the display is
       handled by drm_device_close(), which the VFS calls with the node. */
}

/*
 * The user has switched to another virtual terminal.
 *
 * A virtio-gpu is scanning out the client's buffer directly, so the scanout has
 * to be handed back before the console draws anything -- the console paints
 * into the framebuffer the bootloader set up, and on a virtio-vga that is only
 * on the screen while no resource is bound. Without the GPU there is nothing to
 * undo: presenting is a copy, and simply not copying is enough.
 */
void drm_display_suspend(void) {
    if (!drm_ready) return;
    drm_console_present();
}

/*
 * Put the text console on the screen.
 *
 * Without a virtio-gpu this is nothing at all: the console draws into the
 * scanout the bootloader set up and the host sees it there. With one, the
 * console has to be scanned out like any other buffer -- and it has to be done
 * again as the console changes, because a host resource is a copy of the guest
 * pages rather than a window onto them. The timer calls this while the console
 * is in front; a screenful of transfer at that rate is the same cost the
 * display had before virtio-gpu, and only while nothing else wants the screen.
 */
void drm_console_present(void) {
    if (!virtgpu_available()) return;
    uint32_t pitch = framebuffer_pitch();
    if (!pitch) return;
    (void)virtgpu_console_present(framebuffer_physical_address() +
                                      framebuffer_memory_offset(),
                                  pitch / 4U, framebuffer_width(),
                                  framebuffer_height());
}

void drm_display_resume(void) {
    if (!drm_ready || !active_fb_id) return;
    (void)present_framebuffer(active_fb_id);
}
