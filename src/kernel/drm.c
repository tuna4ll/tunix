#include <stddef.h>
#include <stdint.h>
#include "include/drm.h"
#include "include/file.h"
#include "include/framebuffer.h"
#include "include/heap.h"
#include "include/time.h"
#include "include/kstring.h"
#include "include/pmm.h"
#include "include/usercopy.h"
#include "include/vmm.h"

extern void kprintf(const char *fmt, ...);

#define EINVAL 22
#define ENOENT 2
#define ENOMEM 12
#define ENOTTY 25
#define EFAULT 14
#define EPERM 1
#define EAGAIN 11

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

#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_PRIME 0x5
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

#define DRM_MAX_BUFFERS 64
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

static void buffer_release(struct drm_dumb_buffer *buffer) {
    if (!buffer || !buffer->handle) return;
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

    static const char name[] = "tunixdrm";
    static const char date[] = "20260721";
    static const char desc[] = "Tunix framebuffer KMS";

    version.version_major = 1;
    version.version_minor = 0;
    version.version_patchlevel = 0;

    /* The caller passes buffers and lengths; we fill what fits and always
       report the true length, which is how libdrm sizes its second call. */
    struct { uint64_t pointer; uint64_t *length; const char *text; size_t size; } fields[] = {
        { version.name, &version.name_len, name, sizeof(name) - 1 },
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
    /* No buffer sharing between processes yet, so no PRIME and no modifiers. */
    case DRM_CAP_PRIME:
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
 * Present a framebuffer: copy it into the scanout. A real driver would point
 * the CRTC at the buffer instead, but the display here is a fixed region handed
 * over by the bootloader, so presenting means blitting.
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
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    }
    int status = present_framebuffer(crtc.fb_id);
    if (status != 0) return status;
    active_fb_id = crtc.fb_id;
    return 0;
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
        (void)framebuffer_release_graphics(&drm_display_owner, 0);
        return 0;
    case DRM_NR_SET_MASTER:
    case DRM_NR_GET_MAGIC:
    case DRM_NR_AUTH_MAGIC: return 0;
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
    case DRM_NR_MODE_CREATE_DUMB: return ioctl_create_dumb(user_argument);
    case DRM_NR_MODE_MAP_DUMB: return ioctl_map_dumb(user_argument);
    case DRM_NR_MODE_DESTROY_DUMB: return ioctl_destroy_dumb(user_argument);
    case DRM_NR_MODE_ADDFB: return ioctl_addfb(user_argument);
    case DRM_NR_MODE_ADDFB2: return ioctl_addfb2(user_argument);
    case DRM_NR_MODE_RMFB: return ioctl_rmfb(user_argument);
    /* Dumb buffers are GEM objects, so libdrm frees them either way. */
    case DRM_NR_GEM_CLOSE: return ioctl_gem_close(user_argument);
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
    (void)framebuffer_release_graphics(&drm_display_owner, 0);
}

void drm_file_close(struct file *file) {
    (void)file;
    /* Buffers outlive the descriptor deliberately: a mapping may still be in
       use, and the pages are reference counted. Ownership of the display is
       handled by drm_device_close(), which the VFS calls with the node. */
}
