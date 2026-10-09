#define TEST_NAME "hwreport"
#include "../tests/test.h"

#define DRM_IOWR(nr, size)    ((3UL << 30) | ((u64)(size) << 16) | ('d' << 8) | (nr))
#define DRM_GETCRTC           0xA1
#define DRM_SETCRTC           0xA2
#define DRM_ADDFB             0xAE
#define DRM_PAGE_FLIP         0xB0
#define DRM_CREATE_DUMB       0xB2
#define DRM_MAP_DUMB          0xB3
#define DRM_PAGE_FLIP_EVENT   1U
#define DRM_EVENT_FLIP        2U
#define DRM_CRTC              1U
#define FRAMES                120
#define SQUARE                96U
#define REPORT_PATH           "/tunix-display.txt"

struct drm_mode_info {
    u32 clock;
    u16 hdisplay, hsync_start, hsync_end, htotal, hskew;
    u16 vdisplay, vsync_start, vsync_end, vtotal, vscan;
    u32 vrefresh, flags, type;
    char name[32];
};

struct drm_mode_crtc {
    u64 set_connectors_ptr;
    u32 count_connectors;
    u32 crtc_id;
    u32 fb_id;
    u32 x, y;
    u32 gamma_size;
    u32 mode_valid;
    struct drm_mode_info mode;
};

struct drm_create_dumb {
    u32 height, width, bpp, flags, handle, pitch;
    u64 size;
};

struct drm_map_dumb {
    u32 handle, pad;
    u64 offset;
};

struct drm_fb_cmd {
    u32 fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_page_flip {
    u32 crtc_id, fb_id, flags, reserved;
    u64 user_data;
};

struct drm_event_vblank {
    u32 type, length;
    u64 user_data;
    u32 tv_sec, tv_usec, sequence, crtc_id;
};

struct surface {
    u32 fb;
    u32 pitch;
    u32 *pixels;
    s64 square_x;
};

static char summary[2048];
static u64 summary_used;

static void note(const char *text) {
    print(text);
    while (*text && summary_used + 1 < sizeof(summary)) summary[summary_used++] = *text++;
}

static void note_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[sizeof(digits) - 1 - count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) digits[sizeof(digits) - 1 - count++] = '-';
    char text[25];
    memcpy(text, digits + sizeof(digits) - count, (u64)count);
    text[count] = '\0';
    note(text);
}

static void fill(struct surface *surface, u32 x, u32 width, u32 height, u32 color) {
    for (u32 row = 0; row < height; row++)
        for (u32 column = x; column < x + width; column++)
            surface->pixels[(u64)row * (surface->pitch / 4U) + column] = color;
}

static int make_surface(int fd, u32 width, u32 height, struct surface *out) {
    struct drm_create_dumb create = {height, width, 32, 0, 0, 0, 0};
    if (ioctl(fd, DRM_IOWR(DRM_CREATE_DUMB, sizeof(create)), &create) != 0) return -1;
    struct drm_map_dumb map = {create.handle, 0, 0};
    if (ioctl(fd, DRM_IOWR(DRM_MAP_DUMB, sizeof(map)), &map) != 0) return -1;
    void *pixels = mmap_file(create.size, PROT_READ | PROT_WRITE, fd, (s64)map.offset);
    if ((s64)pixels < 0) return -1;
    struct drm_fb_cmd fb = {0, width, height, create.pitch, 32, 24, create.handle};
    if (ioctl(fd, DRM_IOWR(DRM_ADDFB, sizeof(fb)), &fb) != 0) return -1;
    out->fb = fb.fb_id;
    out->pitch = create.pitch;
    out->pixels = pixels;
    out->square_x = -1;
    fill(out, 0, width, height, 0x00102040U);
    return 0;
}

static s64 wait_flip(int fd) {
    struct drm_event_vblank event;
    s64 start = now_ms();
    while (now_ms() - start < 500) {
        if (read(fd, &event, sizeof(event)) == (s64)sizeof(event) && event.type == DRM_EVENT_FLIP)
            return now_ms() - start;
        sleep_ms(1);
    }
    return -1;
}

static void display_check(void) {
    int fd = (int)open("/dev/dri/card0", O_RDWR, 0);
    if (fd < 0) {
        note("display: no /dev/dri/card0\n");
        return;
    }
    struct drm_mode_crtc crtc = {0};
    crtc.crtc_id = DRM_CRTC;
    if (ioctl(fd, DRM_IOWR(DRM_GETCRTC, sizeof(crtc)), &crtc) != 0 || !crtc.mode.hdisplay) {
        note("display: no mode on the crtc\n");
        return;
    }
    u32 width = crtc.mode.hdisplay, height = crtc.mode.vdisplay;
    note("display: ");
    note_number(width);
    note("x");
    note_number(height);
    note("\n");

    struct surface surfaces[2];
    if (make_surface(fd, width, height, &surfaces[0]) != 0 ||
        make_surface(fd, width, height, &surfaces[1]) != 0) {
        note("display: dumb buffers failed\n");
        return;
    }
    crtc.fb_id = surfaces[0].fb;
    if (ioctl(fd, DRM_IOWR(DRM_SETCRTC, sizeof(crtc)), &crtc) != 0) {
        note("display: setcrtc failed\n");
        return;
    }

    s64 shortest = 1000000, longest = 0, total = 0;
    int missed = 0;
    s64 started = now_ms();
    for (int frame = 0; frame < FRAMES; frame++) {
        struct surface *surface = &surfaces[(frame + 1) % 2];
        if (surface->square_x >= 0) fill(surface, (u32)surface->square_x, SQUARE, SQUARE, 0x00102040U);
        surface->square_x = (s64)((u64)frame * (width - SQUARE) / (FRAMES - 1));
        fill(surface, (u32)surface->square_x, SQUARE, SQUARE, 0x00FFFFFFU);
        struct drm_page_flip flip = {DRM_CRTC, surface->fb, DRM_PAGE_FLIP_EVENT, 0, (u64)frame};
        if (ioctl(fd, DRM_IOWR(DRM_PAGE_FLIP, sizeof(flip)), &flip) != 0) {
            missed++;
            continue;
        }
        s64 waited = wait_flip(fd);
        if (waited < 0) {
            missed++;
            continue;
        }
        total += waited;
        if (waited < shortest) shortest = waited;
        if (waited > longest) longest = waited;
    }
    s64 elapsed = now_ms() - started;
    note("display: ");
    note_number(FRAMES);
    note(" flips in ");
    note_number(elapsed);
    note(" ms, wait min ");
    note_number(shortest);
    note(" avg ");
    note_number(FRAMES - missed ? total / (FRAMES - missed) : 0);
    note(" max ");
    note_number(longest);
    note(" ms, missed ");
    note_number(missed);
    note("\n");
    sleep_ms(2000);
    crtc.fb_id = 0;
    (void)ioctl(fd, DRM_IOWR(DRM_SETCRTC, sizeof(crtc)), &crtc);
    close(fd);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    display_check();
    int fd = (int)open(REPORT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, summary, summary_used);
        close(fd);
    }
    sync();
    print("\nTunix hardware report\n\n"
          "  /tunix-hwreport.txt       the report\n"
          "  /tunix-kmsg.txt           the kernel log\n"
          "  /tunix-display.txt        the page flip check\n"
          "  /tunix-vbios-*.rom        video bios images that were found\n"
          "  /tunix-gpu-regs.bin       nvidia register dump\n\n"
          "They are on the stick's tunix-root partition and already on disk.\n"
          "Switch the machine off with the power button.\n");
    for (;;) sleep_ms(60000);
}
