typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef int s32;

#define SYS_read 0
#define SYS_write 1
#define SYS_poll 7
#define SYS_open 2
#define SYS_close 3
#define SYS_mmap 9
#define SYS_ioctl 16
#define SYS_fork 57
#define SYS_exit_group 231
#define SYS_wait4 61
#define SYS_nanosleep 35
#define SYS_pipe 22
#define SYS_setuid 105
#define SYS_clock_gettime 228
#define CLOCK_MONOTONIC 1

#define O_RDWR 2
#define O_NONBLOCK 04000

#include "tunix_syscall.h"

static int results_fd = -1;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_write, 1, (s64)text, (s64)length);
    if (results_fd >= 0) (void)syscall3(SYS_write, results_fd, (s64)text, (s64)length);
}

#define O_WRONLY_CREAT_TRUNC 0x241

static void open_results(void) {
    results_fd = (int)syscall3(SYS_open, (s64)"/tunix-drmtest-results.txt",
                               O_WRONLY_CREAT_TRUNC, 0644);
}

static void put_signed(s64 value) {
    char buffer[24];
    int index = (int)sizeof(buffer);
    buffer[--index] = 0;
    int negative = value < 0;
    u64 magnitude = negative ? (u64)(-value) : (u64)value;
    do { buffer[--index] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (negative) buffer[--index] = '-';
    put(buffer + index);
}

#define IOC(dir, type, nr, size) \
    (((u64)(dir) << 30) | ((u64)(size) << 16) | ((u64)(type) << 8) | (u64)(nr))
#define IOC_WRITE 1U
#define IOC_READ 2U
#define IOWR(nr, type) IOC(IOC_READ | IOC_WRITE, 'd', nr, sizeof(type))

#define NR_VERSION 0x00
#define NR_GET_CAP 0x0c
#define NR_SET_CLIENT_CAP 0x0d
#define NR_MODE_GETPLANERESOURCES 0xb5
#define NR_MODE_OBJ_GETPROPERTIES 0xb9
#define NR_MODE_GETPROPERTY 0xaa
#define NR_MODE_CREATE_DUMB 0xb2
#define NR_MODE_MAP_DUMB 0xb3
#define NR_MODE_DESTROY_DUMB 0xb4
#define NR_MODE_ADDFB2 0xb8
#define NR_MODE_ATOMIC 0xbc
#define NR_MODE_GETCRTC 0xa1
#define NR_MODE_SETCRTC 0xa2
#define NR_MODE_GETFB 0xad
#define NR_MODE_CREATEPROPBLOB 0xbd
#define NR_MODE_DESTROYPROPBLOB 0xbe
#define NR_MODE_GETCONNECTOR 0xa7
#define NR_MODE_SETPROPERTY 0xab
#define NR_MODE_CLOSEFB 0xd0
#define NR_SET_MASTER 0x1e
#define NR_MODE_CURSOR2 0xbb
#define NR_GEM_CLOSE 0x09
#define NR_PRIME_HANDLE_TO_FD 0x2d
#define NR_PRIME_FD_TO_HANDLE 0x2e

#define DRM_MODE_OBJECT_CRTC 0xcccccccc
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee

#define DRM_CLIENT_CAP_ATOMIC 3
#define DRM_CLIENT_CAP_WRITEBACK 5
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_CAP_SYNCOBJ 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE 0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15

struct drm_version {
    s32 version_major, version_minor, version_patchlevel;
    u64 name_len; u64 name;
    u64 date_len; u64 date;
    u64 desc_len; u64 desc;
};

struct drm_get_cap { u64 capability; u64 value; };
struct drm_set_client_cap { u64 capability; u64 value; };
struct drm_mode_get_plane_res { u64 plane_id_ptr; u32 count_planes; };
struct drm_mode_obj_get_properties {
    u64 props_ptr; u64 prop_values_ptr; u32 count_props; u32 obj_id; u32 obj_type;
};
struct drm_mode_get_property {
    u64 values_ptr; u64 enum_blob_ptr;
    u32 prop_id; u32 flags; char name[32];
    u32 count_values; u32 count_enum_blobs;
};
struct drm_mode_create_blob { u64 data; u32 length; u32 blob_id; };
struct drm_mode_destroy_blob { u32 blob_id; };
struct drm_mode_create_dumb {
    u32 height, width, bpp, flags; u32 handle, pitch; u64 size;
};
struct drm_mode_map_dumb { u32 handle, pad; u64 offset; };
struct drm_mode_destroy_dumb { u32 handle; u32 pad; };
struct drm_gem_close { u32 handle; u32 pad; };
struct drm_prime_handle { u32 handle; u32 flags; s32 fd; };
struct drm_mode_fb_cmd { u32 fb_id, width, height, pitch, bpp, depth, handle; };
struct drm_mode_crtc {
    u64 set_connectors_ptr; u32 count_connectors;
    u32 crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo_fwd { char bytes[68]; } mode;
};
struct drm_mode_fb_cmd2 {
    u32 fb_id, width, height, pixel_format, flags;
    u32 handles[4]; u32 pitches[4]; u32 offsets[4]; u64 modifier[4];
};
struct drm_mode_modeinfo {
    u32 clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
    u32 vrefresh, flags, type; char name[32];
};

struct drm_mode_get_connector {
    u64 encoders_ptr; u64 modes_ptr; u64 props_ptr; u64 prop_values_ptr;
    u32 count_modes, count_props, count_encoders;
    u32 encoder_id, connector_id, connector_type, connector_type_id;
    u32 connection, mm_width, mm_height, subpixel, pad;
};

struct drm_mode_atomic {
    u32 flags; u32 count_objs;
    u64 objs_ptr;
    u64 count_props_ptr;
    u64 props_ptr;
    u64 prop_values_ptr;
    u64 reserved;
    u64 user_data;
};

static int card = -1;

static s64 call(u64 request, void *argument) {
    return syscall3(SYS_ioctl, card, (s64)request, (s64)argument);
}

static void report(const char *tag, s64 result) {
    put(tag);
    put(result < 0 ? " errno=" : " ok=");
    put_signed(result < 0 ? -result : result);
    put("\n");
}

static s64 read_event(int fd, void *event, u64 size) {
    struct { s32 fd; short events, revents; } watch = { fd, 1, 0 };
    (void)syscall3(SYS_poll, (s64)&watch, 1, 200);
    return syscall3(SYS_read, fd, (s64)event, (s64)size);
}

static void test_version(void) {
    char name[32];
    for (int i = 0; i < 32; i++) name[i] = 0;
    struct drm_version version;
    for (unsigned i = 0; i < sizeof(version); i++) ((char *)&version)[i] = 0;
    version.name_len = sizeof(name) - 1;
    version.name = (u64)name;
    s64 result = call(IOWR(NR_VERSION, struct drm_version), &version);
    put("VERSION name=");
    put(result == 0 ? name : "?");
    put(" result=");
    put_signed(result);
    put("\n");
}

static void test_atomic_advertised(void) {
    static const struct { u64 number; const char *name; } asked[] = {
        { DRM_CAP_SYNCOBJ, "syncobj(0x13)" },
        { DRM_CAP_SYNCOBJ_TIMELINE, "syncobj_timeline(0x14)" },
        { DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, "atomic_async_page_flip(0x15)" },
    };
    put("CAP");
    for (unsigned index = 0; index < 3; index++) {
        struct drm_get_cap cap = { asked[index].number, 0 };
        s64 got = call(IOWR(NR_GET_CAP, struct drm_get_cap), &cap);
        put(" ");
        put(asked[index].name);
        put("=");
        put_signed(got == 0 ? (s64)cap.value : -1);
    }
    put("\n");

    struct drm_set_client_cap client = { DRM_CLIENT_CAP_ATOMIC, 1 };
    report("CLIENTCAP atomic", call(IOWR(NR_SET_CLIENT_CAP, struct drm_set_client_cap), &client));
    struct drm_set_client_cap unknown = { DRM_CLIENT_CAP_WRITEBACK, 1 };
    report("CLIENTCAP writeback", call(IOWR(NR_SET_CLIENT_CAP, struct drm_set_client_cap), &unknown));
}

static void test_present_latency(u32 fb_id, u32 blob_id, unsigned short width,
                                 unsigned short height, unsigned commits);

static void test_damage_clips(u32 fb_id, u32 blob_id, unsigned short width,
                              unsigned short height, unsigned commits);

static void test_atomic_modeset(void) {
    struct drm_mode_crtc crtc;
    for (unsigned i = 0; i < sizeof(crtc); i++) ((char *)&crtc)[i] = 0;
    crtc.crtc_id = 1;
    if (call(IOWR(NR_MODE_GETCRTC, struct drm_mode_crtc), &crtc) != 0) {
        put("MODESET getcrtc failed\n");
        return;
    }
    unsigned short width = *(unsigned short *)(crtc.mode.bytes + 4);
    unsigned short height = *(unsigned short *)(crtc.mode.bytes + 14);
    put("MODESET display=");
    put_signed(width);
    put("x");
    put_signed(height);
    put("\n");

    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = width; create.height = height; create.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &create) != 0) {
        put("MODESET create failed\n"); return;
    }
    struct drm_mode_fb_cmd2 fb;
    for (unsigned i = 0; i < sizeof(fb); i++) ((char *)&fb)[i] = 0;
    fb.width = width; fb.height = height; fb.pixel_format = 0x34325258;
    fb.handles[0] = create.handle; fb.pitches[0] = create.pitch;
    if (call(IOWR(NR_MODE_ADDFB2, struct drm_mode_fb_cmd2), &fb) != 0) {
        put("MODESET addfb failed\n"); return;
    }
    struct drm_mode_create_blob blob;
    for (unsigned i = 0; i < sizeof(blob); i++) ((char *)&blob)[i] = 0;
    blob.data = (u64)crtc.mode.bytes;
    blob.length = 68;
    if (call(IOWR(NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob), &blob) != 0) {
        put("MODESET blob failed\n"); return;
    }

    u32 objs[3]   = { 1, 2, 4 };
    u32 counts[3] = { 2, 1, 10 };
    u32 props[13] = { 11, 12, 15, 14, 13, 16, 17, 18, 19, 20, 21, 22, 23 };
    u64 values[13] = {
        1, blob.blob_id,
        1,
        fb.fb_id, 1,
        0, 0, (u64)width << 16, (u64)height << 16,
        0, 0, width, height,
    };
    struct drm_mode_atomic atomic;
    for (unsigned i = 0; i < sizeof(atomic); i++) ((char *)&atomic)[i] = 0;
    atomic.count_objs = 3;
    atomic.objs_ptr = (u64)objs;
    atomic.count_props_ptr = (u64)counts;
    atomic.props_ptr = (u64)props;
    atomic.prop_values_ptr = (u64)values;
    atomic.user_data = 0xABCDEF;
    atomic.flags = DRM_MODE_PAGE_FLIP_EVENT;
    report("MODESET commit", call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic));

    unsigned char event[64];
    s64 got = read_event(card, event, sizeof(event));
    put("MODESET event ");
    put(got > 0 ? "bytes=" : "errno=");
    put_signed(got > 0 ? got : -got);
    if (got >= 16) {
        put(" user_data=");
        put_signed((s64)*(u64 *)(event + 8));
    }
    put("\n");

    int other = (int)syscall3(SYS_open, (s64)"/dev/dri/card0", O_RDWR | O_NONBLOCK, 0);
    s64 stolen = syscall3(SYS_read, other, (s64)event, sizeof(event));
    report("MODESET commit-again", call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic));
    got = read_event(card, event, sizeof(event));
    stolen = syscall3(SYS_read, other, (s64)event, sizeof(event));
    put("CLIENTEVENTS other_read=");
    put_signed(stolen);
    put(" owner_read=");
    put_signed(got);
    put(other >= 0 && stolen == -11 && got >= 16 && *(u64 *)(event + 8) == 0xABCDEF ? " PASS\n" : " FAIL\n");
    (void)syscall1(SYS_close, other);

    u64 previous_us = 0, min_gap_us = ~0ULL;
    u32 previous_sequence = 0, delivered = 0, sequence_ok = 1;
    for (int frame = 0; frame < 10; frame++) {
        if (call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic) != 0) break;
        if (read_event(card, event, sizeof(event)) < 32) break;
        u64 at_us = (u64)*(u32 *)(event + 16) * 1000000ULL + *(u32 *)(event + 20);
        u32 sequence = *(u32 *)(event + 24);
        if (delivered) {
            if (at_us - previous_us < min_gap_us) min_gap_us = at_us - previous_us;
            if (sequence <= previous_sequence) sequence_ok = 0;
        }
        previous_us = at_us;
        previous_sequence = sequence;
        delivered++;
    }
    put("VBLANK events=");
    put_signed(delivered);
    put(" min_gap_us=");
    put_signed((s64)min_gap_us);
    put(delivered == 10 && min_gap_us >= 16000 && sequence_ok ? " PASS\n" : " FAIL\n");

    test_present_latency(fb.fb_id, blob.blob_id, width, height, 200);
    test_damage_clips(fb.fb_id, blob.blob_id, width, height, 200);

    struct drm_mode_destroy_blob kill = { blob.blob_id };
    (void)call(IOWR(NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob), &kill);
    u32 rmfb = fb.fb_id;
    (void)call(IOWR(0xaf, u32), &rmfb);
    struct drm_mode_destroy_dumb drop = { create.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
}

static void test_plane_properties(void) {
    u32 planes[8];
    struct drm_mode_get_plane_res res;
    for (unsigned i = 0; i < sizeof(res); i++) ((char *)&res)[i] = 0;
    res.plane_id_ptr = (u64)planes;
    res.count_planes = 8;
    if (call(IOWR(NR_MODE_GETPLANERESOURCES, struct drm_mode_get_plane_res), &res) != 0) {
        put("PLANES failed\n");
        return;
    }
    put("PLANES count=");
    put_signed(res.count_planes);
    put("\n");
    if (!res.count_planes) return;

    u32 ids[32];
    u64 values[32];
    struct drm_mode_obj_get_properties query;
    for (unsigned i = 0; i < sizeof(query); i++) ((char *)&query)[i] = 0;
    query.props_ptr = (u64)ids;
    query.prop_values_ptr = (u64)values;
    query.count_props = 32;
    query.obj_id = planes[0];
    query.obj_type = DRM_MODE_OBJECT_PLANE;
    s64 result = call(IOWR(NR_MODE_OBJ_GETPROPERTIES, struct drm_mode_obj_get_properties), &query);
    if (result != 0) { report("PLANEPROPS", result); return; }

    put("PLANEPROPS count=");
    put_signed(query.count_props);
    put(" names=");
    for (u32 index = 0; index < query.count_props && index < 32; index++) {
        struct drm_mode_get_property property;
        for (unsigned i = 0; i < sizeof(property); i++) ((char *)&property)[i] = 0;
        property.prop_id = ids[index];
        if (call(IOWR(NR_MODE_GETPROPERTY, struct drm_mode_get_property), &property) == 0) {
            property.name[31] = 0;
            put(property.name);
        } else {
            put("?");
        }
        put(" ");
    }
    put("\n");
}

static void test_atomic_commit(void) {
    u32 objs[1] = { 1  };
    u32 counts[1] = { 1 };
    u32 props[1] = { 11  };
    u64 values[1] = { 0 };

    struct drm_mode_atomic atomic;
    for (unsigned i = 0; i < sizeof(atomic); i++) ((char *)&atomic)[i] = 0;
    atomic.count_objs = 1;
    atomic.objs_ptr = (u64)objs;
    atomic.count_props_ptr = (u64)counts;
    atomic.props_ptr = (u64)props;
    atomic.prop_values_ptr = (u64)values;
    atomic.user_data = 0x1234;
    report("ATOMIC linux-layout", call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic));
    put("ATOMIC sizeof_linux=");
    put_signed((s64)sizeof(struct drm_mode_atomic));
    put("\n");
}

static void test_blob_zero(void) {
    struct drm_mode_destroy_blob destroy = { 0 };
    report("BLOB destroy-zero", call(IOWR(NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob), &destroy));
}

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define SECRET 0x5ec3e7u

static void test_handle_isolation(void) {
    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = 64;
    create.height = 64;
    create.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &create) != 0) {
        put("ISOLATION create failed\n");
        return;
    }

    struct drm_mode_map_dumb map;
    for (unsigned i = 0; i < sizeof(map); i++) ((char *)&map)[i] = 0;
    map.handle = create.handle;
    if (call(IOWR(NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), &map) != 0) {
        put("ISOLATION parent map failed\n");
        return;
    }
    s64 mapped = syscall6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                          card, (s64)map.offset);
    if ((u64)mapped >= (u64)-4095L) {
        put("ISOLATION parent mmap failed errno=");
        put_signed(-mapped);
        put("\n");
        return;
    }
    *(volatile u32 *)mapped = SECRET;

    s64 child = syscall1(SYS_fork, 0);
    if (child == 0) {
        int own = (int)syscall3(SYS_open, (s64)"/dev/dri/card0", O_RDWR, 0);
        if (own < 0) { put("ISOLATION child open failed\n"); (void)syscall1(SYS_exit_group, 0); }
        struct drm_mode_map_dumb theirs;
        for (unsigned i = 0; i < sizeof(theirs); i++) ((char *)&theirs)[i] = 0;
        theirs.handle = create.handle;
        s64 result = syscall3(SYS_ioctl, own,
                              (s64)IOWR(NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), (s64)&theirs);
        put("ISOLATION child ");
        if (result != 0) {
            put("map-dumb refused errno=");
            put_signed(-result);
            put("\n");
            (void)syscall1(SYS_exit_group, 0);
        }
        s64 theirs_mapped = syscall6(SYS_mmap, 0, 4096, PROT_READ, MAP_SHARED,
                                     own, (s64)theirs.offset);
        if ((u64)theirs_mapped >= (u64)-4095L) {
            put("map-dumb allowed but mmap refused errno=");
            put_signed(-theirs_mapped);
            put("\n");
            (void)syscall1(SYS_exit_group, 0);
        }
        u32 seen = *(volatile u32 *)theirs_mapped;
        put(seen == SECRET ? "READ ANOTHER PROCESS'S BUFFER" : "mapped but read nothing");
        put(" value=");
        put_signed((s64)seen);
        put("\n");
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall4(SYS_wait4, child, 0, 0, 0);

    struct drm_mode_destroy_dumb destroy = { create.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &destroy);
}

static void test_framebuffer_lifetime(void) {
    struct drm_mode_create_dumb first;
    for (unsigned i = 0; i < sizeof(first); i++) ((char *)&first)[i] = 0;
    first.width = 64; first.height = 64; first.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &first) != 0) return;

    struct drm_mode_fb_cmd2 fb;
    for (unsigned i = 0; i < sizeof(fb); i++) ((char *)&fb)[i] = 0;
    fb.width = 64; fb.height = 64; fb.pixel_format = 0x34325258 ;
    fb.handles[0] = first.handle; fb.pitches[0] = first.pitch;
    if (call(IOWR(NR_MODE_ADDFB2, struct drm_mode_fb_cmd2), &fb) != 0) return;

    struct drm_mode_destroy_dumb destroy = { first.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &destroy);

    struct drm_mode_fb_cmd query;
    for (unsigned i = 0; i < sizeof(query); i++) ((char *)&query)[i] = 0;
    query.fb_id = fb.fb_id;
    s64 alive = call(IOWR(NR_MODE_GETFB, struct drm_mode_fb_cmd), &query);
    put("FBLIFETIME buffer_destroyed fb_getfb=");
    put_signed(alive);
    put(" fb_names_handle=");
    put_signed(alive == 0 ? (s64)query.handle : -1);
    put("\n");

    u32 reused = 0;
    for (int attempt = 0; attempt < 6000 && !reused; attempt++) {
        struct drm_mode_create_dumb again;
        for (unsigned i = 0; i < sizeof(again); i++) ((char *)&again)[i] = 0;
        again.width = 64; again.height = 64; again.bpp = 32;
        if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &again) != 0) break;
        if (again.handle == first.handle) { reused = 1; }
        struct drm_mode_destroy_dumb drop = { again.handle, 0 };
        if (!reused) (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
    }
    put("FBLIFETIME handle_reused=");
    put(reused ? "YES" : "no");
    put(" so_fb_aliases_new_buffer=");
    put(reused ? "YES" : "not-observed");
    put("\n");
    if (reused) {
        struct drm_mode_destroy_dumb drop = { first.handle, 0 };
        (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
    }
}

static s64 map_word(int fd, u32 handle, u32 *value) {
    struct drm_mode_map_dumb map;
    for (unsigned i = 0; i < sizeof(map); i++) ((char *)&map)[i] = 0;
    map.handle = handle;
    s64 status = syscall3(SYS_ioctl, fd, (s64)IOWR(NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), (s64)&map);
    if (status != 0) return status;
    s64 mapped = syscall6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (s64)map.offset);
    if ((u64)mapped >= (u64)-4095L) return mapped;
    *value = *(volatile u32 *)mapped;
    return 0;
}

static void test_prime_references(void) {
    struct drm_mode_create_dumb first;
    for (unsigned i = 0; i < sizeof(first); i++) ((char *)&first)[i] = 0;
    first.width = 64; first.height = 64; first.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &first) != 0) {
        put("PRIMEREF create failed\n");
        return;
    }
    u32 seen = 0;
    struct drm_mode_map_dumb map;
    for (unsigned i = 0; i < sizeof(map); i++) ((char *)&map)[i] = 0;
    map.handle = first.handle;
    if (call(IOWR(NR_MODE_MAP_DUMB, struct drm_mode_map_dumb), &map) != 0) return;
    s64 mapped = syscall6(SYS_mmap, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, card, (s64)map.offset);
    if ((u64)mapped >= (u64)-4095L) return;
    *(volatile u32 *)mapped = SECRET;

    struct drm_prime_handle exported = { first.handle, 0, -1 };
    if (call(IOWR(NR_PRIME_HANDLE_TO_FD, struct drm_prime_handle), &exported) != 0) {
        put("PRIMEREF export failed\n");
        return;
    }
    int other = (int)syscall3(SYS_open, (s64)"/dev/dri/card0", O_RDWR, 0);
    struct drm_prime_handle imported = { 0, 0, exported.fd };
    s64 status = syscall3(SYS_ioctl, other, (s64)IOWR(NR_PRIME_FD_TO_HANDLE, struct drm_prime_handle),
                          (s64)&imported);
    struct drm_prime_handle again = { 0, 0, exported.fd };
    (void)syscall3(SYS_ioctl, other, (s64)IOWR(NR_PRIME_FD_TO_HANDLE, struct drm_prime_handle),
                   (s64)&again);
    put("PRIMEREF import ok=");
    put_signed(status);
    put(" same_handle=");
    put(again.handle == imported.handle ? "yes" : "NO");
    put("\n");
    struct drm_gem_close close_imported = { imported.handle, 0 };
    (void)syscall3(SYS_ioctl, other, (s64)IOWR(NR_GEM_CLOSE, struct drm_gem_close), (s64)&close_imported);
    s64 twice = syscall3(SYS_ioctl, other, (s64)IOWR(NR_GEM_CLOSE, struct drm_gem_close),
                         (s64)&close_imported);
    struct drm_mode_destroy_dumb destroy = { first.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &destroy);

    s64 view = syscall6(SYS_mmap, 0, 4096, PROT_READ, MAP_SHARED, exported.fd, 0);
    seen = (u64)view >= (u64)-4095L ? 0 : *(volatile u32 *)view;
    put("PRIMEREF importer_close_twice=");
    put_signed(twice);
    put(" dmabuf_keeps_buffer=");
    put(seen == SECRET ? "yes" : "NO");
    put("\n");

    struct drm_mode_create_dumb next;
    for (unsigned i = 0; i < sizeof(next); i++) ((char *)&next)[i] = 0;
    next.width = 64; next.height = 64; next.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &next) != 0) return;
    (void)syscall1(SYS_close, exported.fd);
    (void)syscall1(SYS_close, other);
    seen = 0;
    s64 alive = map_word(card, next.handle, &seen);
    put("PRIMEREF closing_dmabuf_spares_new_buffer=");
    put(alive == 0 ? "yes" : "NO");
    put(" new_reused_handle=");
    put(next.handle == first.handle ? "yes" : "no");
    put("\n");
    struct drm_mode_destroy_dumb drop = { next.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
}

static void test_size_overflow(void) {
    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = 2548958670u;
    create.height = 1809243152u;
    create.bpp = 32;
    s64 result = call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &create);
    put("OVERFLOW create=");
    put_signed(result);
    if (result != 0) { put(" refused\n"); return; }
    put(" ACCEPTED handle=");
    put_signed(create.handle);
    put(" pitch=");
    put_signed(create.pitch);
    put(" size=");
    put_signed((s64)create.size);
    put("\n");

    struct drm_mode_fb_cmd2 fb;
    for (unsigned i = 0; i < sizeof(fb); i++) ((char *)&fb)[i] = 0;
    fb.width = create.width; fb.height = create.height;
    fb.pixel_format = 0x34325258;
    fb.handles[0] = create.handle; fb.pitches[0] = create.pitch;
    s64 added = call(IOWR(NR_MODE_ADDFB2, struct drm_mode_fb_cmd2), &fb);
    put("OVERFLOW addfb=");
    put_signed(added);
    put("\n");
    if (added == 0) {
        struct drm_mode_crtc crtc;
        for (unsigned i = 0; i < sizeof(crtc); i++) ((char *)&crtc)[i] = 0;
        crtc.crtc_id = 1;
        crtc.fb_id = fb.fb_id;
        crtc.mode_valid = 1;
        put("OVERFLOW presenting...\n");
        put("OVERFLOW setcrtc=");
        put_signed(call(IOWR(NR_MODE_SETCRTC, struct drm_mode_crtc), &crtc));
        put("\n");
    }

    struct drm_mode_destroy_dumb drop = { create.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
}

#define LONG_GAP_NS 150000UL

static u64 now_ns(void) {
    struct { s64 seconds, nanoseconds; } value = {0, 0};
    (void)syscall2(SYS_clock_gettime, CLOCK_MONOTONIC, (s64)&value);
    return (u64)value.seconds * 1000000000UL + (u64)value.nanoseconds;
}

static void test_present_latency(u32 fb_id, u32 blob_id, unsigned short width,
                                 unsigned short height, unsigned commits) {
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) { put("LATENCY pipe failed\n"); return; }

    s64 child = syscall1(SYS_fork, 0);
    if (child == 0) {
        (void)syscall1(SYS_close, channel[0]);
        u64 report[2] = { 0, 0 };
        u64 started = now_ns();
        u64 last = started;
        while (now_ns() - started < 400000000UL) {
            u64 at = now_ns();
            u64 gap = at - last;
            if (gap > report[0]) report[0] = gap;
            if (gap > LONG_GAP_NS) report[1]++;
            last = at;
        }
        (void)syscall3(SYS_write, channel[1], (s64)report, sizeof(report));
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall1(SYS_close, channel[1]);

    u32 objs[3]   = { 1, 2, 4 };
    u32 counts[3] = { 2, 1, 10 };
    u32 props[13] = { 11, 12, 15, 14, 13, 16, 17, 18, 19, 20, 21, 22, 23 };
    u64 values[13] = {
        1, blob_id, 1, fb_id, 1,
        0, 0, (u64)width << 16, (u64)height << 16, 0, 0, width, height,
    };
    struct drm_mode_atomic atomic;
    for (unsigned i = 0; i < sizeof(atomic); i++) ((char *)&atomic)[i] = 0;
    atomic.count_objs = 3;
    atomic.objs_ptr = (u64)objs;
    atomic.count_props_ptr = (u64)counts;
    atomic.props_ptr = (u64)props;
    atomic.prop_values_ptr = (u64)values;

    u64 begun = now_ns();
    unsigned done = 0;
    for (unsigned round = 0; round < commits; round++) {
        if (call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic) != 0) break;
        done++;
    }
    u64 elapsed = now_ns() - begun;

    u64 report[2] = { 0, 0 };
    (void)syscall3(SYS_read, channel[0], (s64)report, sizeof(report));
    (void)syscall1(SYS_close, channel[0]);
    (void)syscall4(SYS_wait4, child, 0, 0, 0);

    put("LATENCY commits=");
    put_signed((s64)done);
    put(" us_each=");
    put_signed(done ? (s64)(elapsed / done / 1000UL) : 0);
    put(" stalls_over_");
    put_signed(LONG_GAP_NS / 1000UL);
    put("us=");
    put_signed((s64)report[1]);
    put(" worst_us=");
    put_signed((s64)(report[0] / 1000UL));
    put("\n");
}

static void test_damage_clips(u32 fb_id, u32 blob_id, unsigned short width,
                              unsigned short height, unsigned commits) {
    s32 rect[4] = { 16, 16, 80, 80 };
    struct drm_mode_create_blob clips;
    for (unsigned i = 0; i < sizeof(clips); i++) ((char *)&clips)[i] = 0;
    clips.data = (u64)rect;
    clips.length = sizeof(rect);
    if (call(IOWR(NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob), &clips) != 0) {
        put("DAMAGE blob failed\n");
        return;
    }
    struct drm_mode_create_blob odd = clips;
    odd.length = sizeof(rect) - 1;
    odd.blob_id = 0;
    (void)call(IOWR(NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob), &odd);

    u32 objs[3]   = { 1, 2, 4 };
    u32 counts[3] = { 2, 1, 11 };
    u32 props[14] = { 11, 12, 15, 14, 13, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    u64 values[14] = {
        1, blob_id, 1, fb_id, 1,
        0, 0, (u64)width << 16, (u64)height << 16, 0, 0, width, height, odd.blob_id,
    };
    struct drm_mode_atomic atomic;
    for (unsigned i = 0; i < sizeof(atomic); i++) ((char *)&atomic)[i] = 0;
    atomic.count_objs = 3;
    atomic.objs_ptr = (u64)objs;
    atomic.count_props_ptr = (u64)counts;
    atomic.props_ptr = (u64)props;
    atomic.prop_values_ptr = (u64)values;
    report("DAMAGE ragged", call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic));

    struct drm_mode_create_blob empty;
    for (unsigned i = 0; i < sizeof(empty); i++) ((char *)&empty)[i] = 0;
    report("DAMAGE empty-blob", call(IOWR(NR_MODE_CREATEPROPBLOB, struct drm_mode_create_blob), &empty));
    values[13] = empty.blob_id;
    report("DAMAGE empty-commit", call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic));

    values[13] = clips.blob_id;
    u64 begun = now_ns();
    unsigned done = 0;
    for (unsigned round = 0; round < commits; round++) {
        if (call(IOWR(NR_MODE_ATOMIC, struct drm_mode_atomic), &atomic) != 0) break;
        done++;
    }
    u64 elapsed = now_ns() - begun;
    put("DAMAGE commits=");
    put_signed((s64)done);
    put(" us_each=");
    put_signed(done ? (s64)(elapsed / done / 1000UL) : 0);
    put("\n");

    struct drm_mode_destroy_blob kill = { clips.blob_id };
    (void)call(IOWR(NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob), &kill);
    kill.blob_id = empty.blob_id;
    (void)call(IOWR(NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob), &kill);
    kill.blob_id = odd.blob_id;
    (void)call(IOWR(NR_MODE_DESTROYPROPBLOB, struct drm_mode_destroy_blob), &kill);
}

static int property_named(u32 id, const char *want) {
    struct drm_mode_get_property property;
    for (unsigned i = 0; i < sizeof(property); i++) ((char *)&property)[i] = 0;
    property.prop_id = id;
    if (call(IOWR(NR_MODE_GETPROPERTY, struct drm_mode_get_property), &property) != 0) return 0;
    for (unsigned i = 0; i < 32; i++) {
        if (property.name[i] != want[i]) return 0;
        if (!want[i]) return 1;
    }
    return 0;
}

struct drm_connector_set_property { u64 value; u32 prop_id; u32 connector_id; };

static void test_connector_props(void) {
    u32 props[8];
    u64 values[8];
    struct drm_mode_get_connector connector;
    for (unsigned i = 0; i < sizeof(connector); i++) ((char *)&connector)[i] = 0;
    connector.connector_id = 2;
    connector.props_ptr = (u64)props;
    connector.prop_values_ptr = (u64)values;
    connector.count_props = 8;
    s64 result = call(IOWR(NR_MODE_GETCONNECTOR, struct drm_mode_get_connector), &connector);
    int crtc = 0, dpms = 0;
    u32 dpms_id = 0;
    for (u32 index = 0; result == 0 && index < connector.count_props && index < 8; index++) {
        if (property_named(props[index], "CRTC_ID")) crtc = 1;
        if (property_named(props[index], "DPMS")) { dpms = 1; dpms_id = props[index]; }
    }
    struct drm_connector_set_property on = { 0, dpms_id, 2 };
    s64 set = dpms ? call(IOWR(NR_MODE_SETPROPERTY, struct drm_connector_set_property), &on) : -1;
    struct drm_connector_set_property bad = { 9, dpms_id, 2 };
    s64 refused = call(IOWR(NR_MODE_SETPROPERTY, struct drm_connector_set_property), &bad);
    put("CONNPROPS result=");
    put_signed(result);
    put(" count=");
    put_signed(connector.count_props);
    put(" set=");
    put_signed(set);
    put(result == 0 && crtc && dpms && set == 0 && refused == -22 ? " PASS\n" : " FAIL\n");
}

static void test_close_fb(void) {
    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = 64; create.height = 64; create.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &create) != 0) return;
    struct drm_mode_fb_cmd2 fb;
    for (unsigned i = 0; i < sizeof(fb); i++) ((char *)&fb)[i] = 0;
    fb.width = 64; fb.height = 64; fb.pixel_format = 0x34325258;
    fb.handles[0] = create.handle; fb.pitches[0] = create.pitch;
    if (call(IOWR(NR_MODE_ADDFB2, struct drm_mode_fb_cmd2), &fb) != 0) return;
    u32 closing[2] = { fb.fb_id, 0 };
    s64 closed = call(IOWR(NR_MODE_CLOSEFB, u32[2]), closing);
    struct drm_mode_fb_cmd query;
    for (unsigned i = 0; i < sizeof(query); i++) ((char *)&query)[i] = 0;
    query.fb_id = fb.fb_id;
    s64 gone = call(IOWR(NR_MODE_GETFB, struct drm_mode_fb_cmd), &query);
    put("CLOSEFB closed=");
    put_signed(closed);
    put(" getfb=");
    put_signed(gone);
    put(closed == 0 && gone != 0 ? " PASS\n" : " FAIL\n");
    struct drm_mode_destroy_dumb drop = { create.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
}

struct drm_mode_cursor2 {
    u32 flags, crtc_id; s32 x, y; u32 width, height, handle; s32 hot_x, hot_y;
};

static void test_cursor(void) {
    struct drm_mode_create_dumb create;
    for (unsigned i = 0; i < sizeof(create); i++) ((char *)&create)[i] = 0;
    create.width = 64; create.height = 64; create.bpp = 32;
    if (call(IOWR(NR_MODE_CREATE_DUMB, struct drm_mode_create_dumb), &create) != 0) return;
    struct drm_mode_cursor2 cursor = { 1, 1, 100, 80, 64, 64, create.handle, 3, 1 };
    s64 set = call(IOWR(NR_MODE_CURSOR2, struct drm_mode_cursor2), &cursor);
    struct drm_mode_cursor2 move = { 2, 1, 140, 90, 0, 0, 0, 0, 0 };
    s64 moved = call(IOWR(NR_MODE_CURSOR2, struct drm_mode_cursor2), &move);
    struct drm_mode_cursor2 wrong = { 1, 1, 0, 0, 32, 32, create.handle, 0, 0 };
    s64 refused = call(IOWR(NR_MODE_CURSOR2, struct drm_mode_cursor2), &wrong);
    struct drm_mode_cursor2 hide = { 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    s64 hidden = call(IOWR(NR_MODE_CURSOR2, struct drm_mode_cursor2), &hide);
    int hardware = set == 0 && moved == 0 && refused == -22 && hidden == 0;
    int absent = set == -6 && moved == -6 && refused == -6 && hidden == -6;
    put("CURSOR set=");
    put_signed(set);
    put(" move=");
    put_signed(moved);
    put(hardware ? " hardware" : absent ? " absent" : " mixed");
    put(hardware || absent ? " PASS\n" : " FAIL\n");
    struct drm_mode_destroy_dumb drop = { create.handle, 0 };
    (void)call(IOWR(NR_MODE_DESTROY_DUMB, struct drm_mode_destroy_dumb), &drop);
}

static void test_set_master(void) {
    s64 root = syscall3(SYS_ioctl, card, (s64)IOC(0, 'd', NR_SET_MASTER, 0), 0);
    int channel[2];
    if (syscall1(SYS_pipe, (s64)channel) != 0) return;
    s64 child = syscall1(SYS_fork, 0);
    if (child == 0) {
        (void)syscall1(SYS_close, channel[0]);
        (void)syscall1(SYS_setuid, 1000);
        s64 user = syscall3(SYS_ioctl, card, (s64)IOC(0, 'd', NR_SET_MASTER, 0), 0);
        (void)syscall3(SYS_write, channel[1], (s64)&user, sizeof(user));
        (void)syscall1(SYS_exit_group, 0);
    }
    (void)syscall1(SYS_close, channel[1]);
    s64 user = 0;
    (void)syscall3(SYS_read, channel[0], (s64)&user, sizeof(user));
    (void)syscall1(SYS_close, channel[0]);
    (void)syscall4(SYS_wait4, child, 0, 0, 0);
    put("MASTER root=");
    put_signed(root);
    put(" user=");
    put_signed(user);
    put(root == 0 && user == -1 ? " PASS\n" : " FAIL\n");
}

static int run(void) {
    card = (int)syscall3(SYS_open, (s64)"/dev/dri/card0", O_RDWR | O_NONBLOCK, 0);
    open_results();
    put("DRMTEST START card=");
    put_signed(card);
    put("\n");
    if (card < 0) { put("DRMTEST DONE\n"); return 0; }

    test_version();
    test_atomic_advertised();
    test_plane_properties();
    test_atomic_commit();
    test_connector_props();
    test_close_fb();
    test_set_master();
    test_cursor();
    test_atomic_modeset();
    test_blob_zero();
    test_handle_isolation();
    test_framebuffer_lifetime();
    test_prime_references();
    test_size_overflow();

    put("DRMTEST DONE\n");
    return 0;
}

static void sleep_forever(void) {
    struct { s64 seconds, nanoseconds; } request = { 1, 0 };
    for (;;) (void)syscall2(SYS_nanosleep, (s64)&request, 0);
}

static void run_and_park(void) __attribute__((noreturn, used));
static void run_and_park(void) {
    (void)run();
    sleep_forever();
    __builtin_unreachable();
}

TUNIX_START(run_and_park)
