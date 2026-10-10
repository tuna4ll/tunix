#define TEST_NAME "hwreport"
#include "../../testsuites/kernel-tests/test.h"

#define DRM_IOWR(nr, size)  ((3UL << 30) | ((u64)(size) << 16) | ('d' << 8) | (nr))
#define DRM_GETCRTC         0xA1
#define DRM_SETCRTC         0xA2
#define DRM_ADDFB           0xAE
#define DRM_PAGE_FLIP       0xB0
#define DRM_CREATE_DUMB     0xB2
#define DRM_MAP_DUMB        0xB3
#define DRM_PAGE_FLIP_EVENT 1U
#define DRM_EVENT_FLIP      2U
#define DRM_CRTC            1U
#define FRAMES              120
#define SQUARE              96U
#define REPORT_PATH         "/tunix-display.txt"
#define KMSG_PATH           "/tunix-kmsg.txt"
#define BACKLIGHT           "/sys/class/backlight/nv_backlight/"
#define SMI_COUNT           "/sys/devices/system/cpu/smi_count"
#define KEY_WATCH_MS        20000
#define KERNEL_DONE         "TUNIX: starting"
#define KERNEL_WAIT_MS      120000

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

static s64 now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.sec * 1000000 + now.nsec / 1000;
}

struct span {
    s64 shortest, longest, total, count;
};

static void span_add(struct span *span, s64 value) {
    if (!span->count || value < span->shortest) span->shortest = value;
    if (value > span->longest) span->longest = value;
    span->total += value;
    span->count++;
}

static void note_span(const char *name, const struct span *span) {
    note("display: ");
    note(name);
    note(" min ");
    note_number(span->shortest);
    note(" avg ");
    note_number(span->count ? span->total / span->count : 0);
    note(" max ");
    note_number(span->longest);
    note(" us\n");
}

struct vblanks {
    u32 first_sequence, last_sequence, events;
    s64 first_us, last_us;
};

static struct vblanks vblanks;

static void count_vblank(const struct drm_event_vblank *event) {
    s64 at = (s64)event->tv_sec * 1000000 + event->tv_usec;
    if (!vblanks.events++) {
        vblanks.first_sequence = event->sequence;
        vblanks.first_us = at;
    }
    vblanks.last_sequence = event->sequence;
    vblanks.last_us = at;
}

static void note_vblanks(void) {
    u32 frames = vblanks.last_sequence - vblanks.first_sequence;
    note("display: vblank period ");
    note_number(frames ? (vblanks.last_us - vblanks.first_us) / frames : 0);
    note(" us over ");
    note_number(frames);
    note(" vblanks\n");
}

static void note_file(const char *path) {
    static char text[4096];
    int fd = (int)open(path, O_RDONLY, 0);
    if (fd < 0) return;
    s64 total = 0, got;
    while (total < (s64)sizeof(text) - 1 &&
           (got = read(fd, text + total, sizeof(text) - 1 - (u64)total)) > 0)
        total += got;
    close(fd);
    text[total] = '\0';
    note(text);
}

static s64 wait_flip(int fd, int epoll) {
    struct drm_event_vblank event;
    struct epoll_event ready;
    s64 start = now_ms();
    while (now_ms() - start < 500) {
        if (read(fd, &event, sizeof(event)) == (s64)sizeof(event) && event.type == DRM_EVENT_FLIP) {
            count_vblank(&event);
            return now_ms() - start;
        }
        (void)epoll_wait(epoll, &ready, 1, 500);
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

    int epoll = (int)epoll_create();
    if (epoll < 0 || epoll_add(epoll, fd, EPOLLIN, 0) != 0) {
        note("display: epoll failed\n");
        return;
    }
    struct span drawing = {0}, flipping = {0}, waiting = {0};
    int missed = 0;
    s64 started = now_ms();
    for (int frame = 0; frame < FRAMES; frame++) {
        struct surface *surface = &surfaces[(frame + 1) % 2];
        s64 mark = now_us();
        if (surface->square_x >= 0)
            fill(surface, (u32)surface->square_x, SQUARE, SQUARE, 0x00102040U);
        surface->square_x = (s64)((u64)frame * (width - SQUARE) / (FRAMES - 1));
        fill(surface, (u32)surface->square_x, SQUARE, SQUARE, 0x00FFFFFFU);
        span_add(&drawing, now_us() - mark);
        struct drm_page_flip flip = {DRM_CRTC, surface->fb, DRM_PAGE_FLIP_EVENT, 0, (u64)frame};
        mark = now_us();
        s64 status = ioctl(fd, DRM_IOWR(DRM_PAGE_FLIP, sizeof(flip)), &flip);
        span_add(&flipping, now_us() - mark);
        if (status != 0) {
            missed++;
            continue;
        }
        mark = now_us();
        if (wait_flip(fd, epoll) < 0) {
            missed++;
            continue;
        }
        span_add(&waiting, now_us() - mark);
    }
    s64 elapsed = now_ms() - started;
    note("display: ");
    note_number(FRAMES);
    note(" flips in ");
    note_number(elapsed);
    note(" ms, missed ");
    note_number(missed);
    note("\n");
    note_span("draw", &drawing);
    note_span("page flip ioctl", &flipping);
    note_span("event wait", &waiting);
    note_vblanks();
    note_file("/proc/interrupts");
    sleep_ms(2000);
    crtc.fb_id = 0;
    (void)ioctl(fd, DRM_IOWR(DRM_SETCRTC, sizeof(crtc)), &crtc);
    close(epoll);
    close(fd);
}

static char log[256 * 1024];

static s64 read_kernel_log(void) {
    int in = (int)open("/dev/kmsg", O_RDONLY, 0);
    if (in < 0) return 0;
    s64 total = 0, got;
    while (total < (s64)sizeof(log) && (got = read(in, log + total, sizeof(log) - (u64)total)) > 0)
        total += got;
    close(in);
    return total;
}

static int same(const char *left, const char *right, u64 size) {
    for (u64 index = 0; index < size; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int log_contains(s64 length, const char *text) {
    u64 size = strlen(text);
    for (s64 at = 0; at + (s64)size <= length; at++)
        if (same(log + at, text, size)) return 1;
    return 0;
}

static void wait_for_kernel_report(void) {
    s64 start = now_ms();
    while (!log_contains(read_kernel_log(), KERNEL_DONE) && now_ms() - start < KERNEL_WAIT_MS)
        sleep_ms(100);
}

static s64 read_number(const char *path) {
    char text[32];
    int fd = (int)open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    s64 got = read(fd, text, sizeof(text));
    close(fd);
    if (got <= 0 || text[0] < '0' || text[0] > '9') return -1;
    s64 value = 0;
    for (s64 index = 0; index < got && text[index] >= '0' && text[index] <= '9'; index++)
        value = value * 10 + (text[index] - '0');
    return value;
}

static int write_text(const char *path, const char *text) {
    int fd = (int)open(path, O_WRONLY, 0);
    if (fd < 0) return -1;
    s64 length = (s64)strlen(text);
    s64 wrote = write(fd, text, (u64)length);
    close(fd);
    return wrote == length ? 0 : -1;
}

static void format_number(char *out, s64 value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) *out++ = digits[--count];
    *out++ = '\n';
    *out = '\0';
}

static int set_backlight(s64 level) {
    char text[24];
    format_number(text, level);
    return write_text(BACKLIGHT "brightness", text);
}

static void backlight_check(void) {
    s64 most = read_number(BACKLIGHT "max_brightness");
    if (most <= 0) {
        note("backlight: none\n");
        return;
    }
    s64 start = read_number(BACKLIGHT "brightness");
    note("backlight: max ");
    note_number(most);
    note(", at ");
    note_number(start);
    note(", type ");
    note_file(BACKLIGHT "type");
    int refused = set_backlight(most + 1) != 0 && write_text(BACKLIGHT "brightness", "dim\n") != 0;
    note(refused ? "backlight: bad values refused\n" : "backlight: a bad value was taken\n");
    const s64 levels[] = {most / 5, most * 3 / 5, start};
    for (unsigned index = 0; index < sizeof(levels) / sizeof(levels[0]); index++) {
        int status = set_backlight(levels[index]);
        sleep_ms(1000);
        note("backlight: set ");
        note_number(levels[index]);
        note(status ? " failed" : " reads ");
        if (!status) note_number(read_number(BACKLIGHT "actual_brightness"));
        note("\n");
    }
}

static void note_hex(u8 value) {
    static const char alphabet[] = "0123456789abcdef";
    char text[4] = {alphabet[value >> 4], alphabet[value & 15U], ' ', '\0'};
    note(text);
}

static void key_watch(void) {
    int keyboard = (int)open("/dev/input/keyboard", O_RDONLY | O_NONBLOCK, 0);
    if (keyboard < 0) {
        note("keys: no /dev/input/keyboard\n");
        return;
    }
    print("\n>>> Press Fn + brightness DOWN three times, then Fn + brightness UP three times.\n"
          ">>> Watching the keyboard and the panel for 20 seconds.\n\n");
    (void)write_text("/dev/kmsg", "HWREPORT: key watch begins\n");
    s64 smi_before = read_number(SMI_COUNT);
    s64 level = read_number(BACKLIGHT "actual_brightness");
    s64 start = now_ms();
    unsigned bytes = 0, changes = 0;
    note("keys: bytes ");
    while (now_ms() - start < KEY_WATCH_MS) {
        u8 data[32];
        s64 got = read(keyboard, data, sizeof(data));
        for (s64 index = 0; index < got && bytes < 96U; index++, bytes++) note_hex(data[index]);
        s64 now = read_number(BACKLIGHT "actual_brightness");
        if (now != level) {
            changes++;
            level = now;
        }
        sleep_ms(50);
    }
    close(keyboard);
    (void)write_text("/dev/kmsg", "HWREPORT: key watch ends\n");
    note("\nkeys: ");
    note_number(bytes);
    note(" bytes, panel level changed ");
    note_number(changes);
    note(" times by itself, ends at ");
    note_number(level);
    note(", smis ");
    note_number(smi_before >= 0 ? read_number(SMI_COUNT) - smi_before : -1);
    note("\n");
}

static void save_kernel_log(void) {
    s64 total = read_kernel_log();
    int out = (int)open(KMSG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return;
    write(out, log, (u64)total);
    close(out);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    wait_for_kernel_report();
    display_check();
    backlight_check();
    key_watch();
    int fd = (int)open(REPORT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, summary, summary_used);
        close(fd);
    }
    save_kernel_log();
    sync();
    print("\nTunix hardware report\n\n"
          "  /tunix-hwreport.txt       the report\n"
          "  /tunix-kmsg.txt           the kernel log\n"
          "  /tunix-display.txt        the page flip check\n"
          "  /tunix-vbios-*.rom        video bios images that were found\n"
          "  /tunix-acpi-*.aml         acpi tables\n"
          "  /tunix-gpu-regs.bin       nvidia register dump\n\n"
          "They are on the stick's tunix-root partition and already on disk.\n"
          "Switch the machine off with the power button.\n");
    for (;;) sleep_ms(60000);
}
