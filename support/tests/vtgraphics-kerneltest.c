typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_FSTAT 5
#define SYS_IOCTL 16
#define SYS_FCHOWNAT 260
#define SYS_EXIT_GROUP 231

#define O_RDWR 2
#define AT_EMPTY_PATH 0x1000
#define KDSETMODE 0x4B3A
#define KD_TEXT 0
#define KD_GRAPHICS 1
#define VT_ACTIVATE 0x5606

#define IOC(dir, type, nr, size) \
    (((u64)(dir) << 30) | ((u64)(size) << 16) | ((u64)(type) << 8) | (u64)(nr))
#define IOWR(nr, type) IOC(3U, 'd', nr, sizeof(type))
#define SET_MASTER IOC(0, 'd', 0x1e, 0)
#define DROP_MASTER IOC(0, 'd', 0x1f, 0)

struct drm_mode_create_dumb { u32 height, width, bpp, flags; u32 handle, pitch; u64 size; };
struct drm_mode_fb_cmd2 {
    u32 fb_id, width, height, pixel_format, flags;
    u32 handles[4]; u32 pitches[4]; u32 offsets[4]; u64 modifier[4];
};
struct drm_mode_crtc {
    u64 set_connectors_ptr; u32 count_connectors;
    u32 crtc_id, fb_id, x, y, gamma_size, mode_valid;
    char mode[68];
};

static unsigned failures;

static void put(const char *text) {
    u64 length = 0;
    while (text[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)text, (s64)length);
}

static void put_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0) put("-");
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static void check(const char *name, int ok, s64 detail) {
    put("VTGRAPHICS ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static void zero(void *memory, u64 size) {
    for (u64 index = 0; index < size; index++) ((char *)memory)[index] = 0;
}

static s64 present(int card, u32 fb_id) {
    struct drm_mode_crtc crtc;
    zero(&crtc, sizeof(crtc));
    crtc.crtc_id = 1;
    crtc.fb_id = fb_id;
    return syscall3(SYS_IOCTL, card, (s64)IOWR(0xa2, struct drm_mode_crtc), (s64)&crtc);
}

static u32 framebuffer(int card) {
    struct drm_mode_create_dumb create;
    zero(&create, sizeof(create));
    create.width = 64; create.height = 64; create.bpp = 32;
    if (syscall3(SYS_IOCTL, card, (s64)IOWR(0xb2, struct drm_mode_create_dumb), (s64)&create) != 0)
        return 0;
    struct drm_mode_fb_cmd2 fb;
    zero(&fb, sizeof(fb));
    fb.width = 64; fb.height = 64; fb.pixel_format = 0x34325258;
    fb.handles[0] = create.handle; fb.pitches[0] = create.pitch;
    if (syscall3(SYS_IOCTL, card, (s64)IOWR(0xb8, struct drm_mode_fb_cmd2), (s64)&fb) != 0) return 0;
    return fb.fb_id;
}

void run(void) {
    int greeter = (int)syscall3(SYS_OPEN, (s64)"/dev/tty7", O_RDWR, 0);
    int session = (int)syscall3(SYS_OPEN, (s64)"/dev/tty2", O_RDWR, 0);
    check("open-vts", greeter >= 0 && session >= 0, greeter < 0 ? greeter : session);

    s64 status = syscall6(SYS_FCHOWNAT, session, (s64)"", 0, 5, AT_EMPTY_PATH, 0);
    unsigned char info[144];
    zero(info, sizeof(info));
    s64 stat = syscall2(SYS_FSTAT, session, (s64)info);
#if defined(__aarch64__)
    u32 gid = *(u32 *)(info + 28);
#else
    u32 gid = *(u32 *)(info + 32);
#endif
    check("fchownat-empty-path", status == 0 && stat == 0 && gid == 5, status ? status : (s64)gid);

    int card = (int)syscall3(SYS_OPEN, (s64)"/dev/dri/card0", O_RDWR, 0);
    if (card < 0) {
        put("VTGRAPHICS no display, drm checks skipped\n");
    } else {
        u32 fb = framebuffer(card);
        check("framebuffer", fb != 0, 0);

        status = syscall3(SYS_IOCTL, greeter, VT_ACTIVATE, 7);
        s64 mode = syscall3(SYS_IOCTL, greeter, KDSETMODE, KD_GRAPHICS);
        s64 master = syscall3(SYS_IOCTL, card, (s64)SET_MASTER, 0);
        s64 shown = present(card, fb);
        check("greeter-presents", status == 0 && mode == 0 && master == 0 && shown == 0,
              shown ? shown : mode);

        (void)syscall3(SYS_IOCTL, card, (s64)DROP_MASTER, 0);
        status = syscall3(SYS_IOCTL, session, VT_ACTIVATE, 2);
        mode = syscall3(SYS_IOCTL, session, KDSETMODE, KD_GRAPHICS);
        master = syscall3(SYS_IOCTL, card, (s64)SET_MASTER, 0);
        shown = present(card, fb);
        check("second-session-presents", status == 0 && mode == 0 && master == 0 && shown == 0,
              shown ? shown : mode);

        (void)syscall3(SYS_IOCTL, card, (s64)DROP_MASTER, 0);
        status = syscall3(SYS_IOCTL, greeter, VT_ACTIVATE, 7);
        master = syscall3(SYS_IOCTL, card, (s64)SET_MASTER, 0);
        shown = present(card, fb);
        check("greeter-presents-again", status == 0 && master == 0 && shown == 0, shown);

        (void)syscall3(SYS_IOCTL, card, (s64)DROP_MASTER, 0);
        status = syscall3(SYS_IOCTL, session, VT_ACTIVATE, 2);
        master = syscall3(SYS_IOCTL, card, (s64)SET_MASTER, 0);
        shown = present(card, fb);
        check("session-presents-again", status == 0 && master == 0 && shown == 0, shown);

        s64 background = syscall3(SYS_IOCTL, greeter, KDSETMODE, KD_TEXT);
        mode = syscall3(SYS_IOCTL, greeter, KDSETMODE, KD_GRAPHICS);
        check("background-vt-cannot-take-display", background == 0 && mode == -16, mode);

        (void)syscall3(SYS_IOCTL, session, KDSETMODE, KD_TEXT);
        (void)syscall3(SYS_IOCTL, card, (s64)DROP_MASTER, 0);
        (void)syscall1(SYS_CLOSE, card);
    }

    put(failures ? "VTGRAPHICS FAIL\n" : "VTGRAPHICS PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
