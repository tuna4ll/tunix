#include <stddef.h>
#include <stdint.h>
#include "include/block.h"
#include "include/devfs.h"
#include "include/devnum.h"
#include "include/klog.h"
#include "include/kstring.h"
#include "include/input.h"
#include "include/framebuffer.h"
#include "include/drm.h"
#include "include/pty.h"
#include "include/random.h"
#include "include/sound.h"
#include "include/time.h"
#include "include/tty.h"
#include "include/usercopy.h"
#include "include/vfs.h"
#include "include/vt.h"
#include "include/tunix/input_event.h"

#define EFAULT 14
#define EINVAL 22
#define ENOSPC 28
#define EIO 5
#define EAGAIN 11
#define RTC_RD_TIME 0x80247009UL

struct linux_rtc_time {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

static int always_ready(struct vfs_node *node) {
    (void)node;
    return 1;
}

static int64_t null_read(struct vfs_node *node, uint64_t offset,
                         size_t size, void *buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    return 0;
}

static int64_t discard_write(struct vfs_node *node, uint64_t offset,
                             size_t size, const void *buffer) {
    (void)node;
    (void)offset;
    (void)buffer;
    return (int64_t)size;
}

static int64_t zero_read(struct vfs_node *node, uint64_t offset,
                         size_t size, void *buffer) {
    (void)node;
    (void)offset;
    if (!buffer) return -1;
    memset(buffer, 0, size);
    return (int64_t)size;
}

static int64_t full_write(struct vfs_node *node, uint64_t offset,
                          size_t size, const void *buffer) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buffer;
    return -ENOSPC;
}

static int64_t random_read(struct vfs_node *node, uint64_t offset,
                           size_t size, void *buffer) {
    (void)node;
    (void)offset;
    if (!buffer) return -1;
    random_get_bytes(buffer, size);
    return (int64_t)size;
}

static int64_t random_write(struct vfs_node *node, uint64_t offset,
                            size_t size, const void *buffer) {
    (void)node;
    (void)offset;
    if (!buffer) return -1;
    random_mix(buffer, size);
    return (int64_t)size;
}

static int random_ready(struct vfs_node *node) {
    (void)node;
    return random_is_seeded();
}

static int64_t kmsg_read(struct vfs_node *node, uint64_t offset,
                         size_t size, void *buffer) {
    (void)node;
    return klog_read(offset, size, buffer);
}

static int64_t kmsg_write(struct vfs_node *node, uint64_t offset,
                          size_t size, const void *buffer) {
    (void)node;
    (void)offset;
    return klog_write(size, buffer);
}

static int kmsg_ready(struct vfs_node *node) {
    (void)node;
    return klog_size() != 0;
}

static int64_t rtc_read(struct vfs_node *node, uint64_t offset,
                        size_t size, void *buffer) {
    (void)node;
    uint64_t epoch = time_epoch_seconds();
    if (!buffer || offset >= sizeof(epoch)) return 0;
    size_t available = sizeof(epoch) - (size_t)offset;
    if (size > available) size = available;
    memcpy(buffer, (const uint8_t *)&epoch + offset, size);
    return (int64_t)size;
}

static int64_t rtc_ioctl(struct vfs_node *node, unsigned long request,
                         uint64_t user_argument) {
    (void)node;
    if (request != RTC_RD_TIME) return -EINVAL;
    if (!user_argument) return -EFAULT;
    struct tunix_rtc_time now;
    if (time_get_rtc(&now) != 0) return -EIO;
    struct linux_rtc_time value = {
        .tm_sec = now.second,
        .tm_min = now.minute,
        .tm_hour = now.hour,
        .tm_mday = now.day,
        .tm_mon = now.month - 1,
        .tm_year = now.year - 1900,
        .tm_wday = now.weekday,
        .tm_yday = now.yearday,
        .tm_isdst = 0
    };
    return copy_to_user(user_argument, &value, sizeof(value)) == 0 ? 0 : -EFAULT;
}

/*
 * /dev/sda, /dev/sda1, /dev/sdb, ... one node per device the block layer holds,
 * disks and partitions alike. The name is the block layer's, looked up rather
 * than derived, so the node and the device cannot drift apart.
 */
static const struct block_device *disk_of(const struct vfs_node *node) {
    return block_device_at(block_device_index_by_name(node->name));
}

static int64_t disk_read(struct vfs_node *node, uint64_t offset,
                         size_t size, void *buffer) {
    return block_device_read_bytes(disk_of(node), offset, size, buffer) == 0
        ? (int64_t)size : -1;
}

static int64_t disk_write(struct vfs_node *node, uint64_t offset,
                          size_t size, const void *buffer) {
    return block_device_write_bytes(disk_of(node), offset, size, buffer) == 0
        ? (int64_t)size : -1;
}

static int64_t keyboard_read(struct vfs_node *node, uint64_t offset,
                             size_t size, void *buffer) {
    (void)node;
    (void)offset;
    return input_read_scancodes(size, buffer);
}

static int keyboard_ready(struct vfs_node *node) {
    (void)node;
    return input_scancodes_ready();
}

static void keyboard_open(struct vfs_node *node) {
    (void)node;
    input_scancode_open();
}

static void keyboard_close(struct vfs_node *node) {
    (void)node;
    input_scancode_close();
}

static int64_t input_event_ioctl(struct vfs_node *node, unsigned long request,
                                 uint64_t user_argument) {
    if (!node || request != TUNIX_EVIOCGINFO) return -EINVAL;
    if (!user_argument) return -EFAULT;
    struct tunix_input_device_info info;
    unsigned device_id = (unsigned)(uintptr_t)node->data;
    int status = input_get_device_info(device_id, &info);
    if (status != 0) return status;
    return copy_to_user(user_argument, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
}

/*
 * The terminal devices.
 *
 * /dev/tty1../dev/ttyN are the virtual terminals themselves; /dev/tty0 and
 * /dev/console both mean whichever one is active, which is where a program that
 * wants to drive the display asks its VT questions; /dev/tty means the caller's
 * own controlling terminal. All four go through the same operations, and which
 * terminal is meant is carried in the node.
 */
static struct vfs_node *attach_terminal(struct vfs_node *dev, const char *name,
                                        unsigned index, uint32_t major,
                                        uint32_t minor) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_CHARDEVICE);
    if (!node) return NULL;
    node->mode = 0666;
    node->data = (void *)(uintptr_t)index;
    node->read = vt_node_read;
    node->write = vt_node_write;
    node->read_ready = vt_node_ready;
    node->ioctl = vt_node_ioctl;
    node->dev_major = major;
    node->dev_minor = minor;
    node->gid = DEV_GROUP_TTY;
    if (vfs_attach(dev, node) != 0) return NULL;
    return node;
}

static struct vfs_node *attach_device(struct vfs_node *dev, const char *name,
                                      uint32_t flags, uint32_t mode,
                                      vfs_read_fn read, vfs_write_fn write,
                                      vfs_ready_fn ready) {
    struct vfs_node *node = vfs_alloc_node(name, flags);
    if (!node) return NULL;
    node->mode = mode;
    node->read = read;
    node->write = write;
    node->read_ready = ready;
    if (vfs_attach(dev, node) != 0) return NULL;
    return node;
}

void devfs_init(void) {
    struct vfs_node *dev = vfs_mkdir_p("/dev");
    if (!dev) return;
    vfs_mount_builtin("devtmpfs", "/dev", "devtmpfs", dev);
    pty_init();

    /* /dev/console is the active terminal, as it is on Linux when the kernel
       console is a VT: a service that writes to it writes to whatever the user
       is looking at. */
    (void)attach_terminal(dev, "console", VT_NODE_ACTIVE, DEV_MAJOR_TTYAUX,
                          DEV_MINOR_TTYAUX_CONSOLE);
    (void)attach_terminal(dev, "tty0", VT_NODE_ACTIVE, DEV_MAJOR_TTY, 0);
    (void)attach_terminal(dev, "tty", VT_NODE_CURRENT, DEV_MAJOR_TTYAUX,
                          DEV_MINOR_TTYAUX_CURRENT);
    for (unsigned index = 1U; index <= VT_COUNT; index++) {
        char name[8];
        name[0] = 't'; name[1] = 't'; name[2] = 'y';
        if (index < 10U) {
            name[3] = (char)('0' + index);
            name[4] = 0;
        } else {
            name[3] = (char)('0' + index / 10U);
            name[4] = (char)('0' + index % 10U);
            name[5] = 0;
        }
        (void)attach_terminal(dev, name, index, DEV_MAJOR_TTY, index);
    }

    (void)attach_device(dev, "null", VFS_CHARDEVICE, 0666,
                        null_read, discard_write, always_ready);
    (void)attach_device(dev, "zero", VFS_CHARDEVICE, 0666,
                        zero_read, discard_write, always_ready);
    (void)attach_device(dev, "full", VFS_CHARDEVICE, 0666,
                        zero_read, full_write, always_ready);
    (void)attach_device(dev, "random", VFS_CHARDEVICE, 0666,
                        random_read, random_write, random_ready);
    (void)attach_device(dev, "urandom", VFS_CHARDEVICE, 0666,
                        random_read, random_write, random_ready);
    (void)attach_device(dev, "kmsg", VFS_CHARDEVICE, 0600,
                        kmsg_read, kmsg_write, kmsg_ready);

    struct vfs_node *rtc = attach_device(dev, "rtc", VFS_CHARDEVICE, 0660,
                                         rtc_read, NULL, always_ready);
    if (rtc) rtc->ioctl = rtc_ioctl;

    for (int index = 0; index < block_device_count(); index++) {
        const struct block_device *device = block_device_at(index);
        if (!device) break;
        struct vfs_node *disk = attach_device(dev, device->dev_name, VFS_BLOCKDEVICE, 0660,
                                              disk_read,
                                              device->write ? disk_write : NULL,
                                              NULL);
        if (!disk) continue;
        disk->length = device->sectors * BLOCK_SECTOR_SIZE;
        disk->gid = DEV_GROUP_DISK;
    }

    if (framebuffer_available()) {
        struct vfs_node *fb = attach_device(dev, "fb0",
            VFS_CHARDEVICE | VFS_FRAMEBUFFER, 0660, NULL, NULL, always_ready);
        if (fb) {
            fb->length = framebuffer_byte_length();
            fb->mmap = framebuffer_device_mmap;
            fb->gid = DEV_GROUP_VIDEO;
        }
    }

    /* The DRM device sits beside /dev/fb0 and drives the same display; it is
       what lets unmodified Linux graphics software run here. */
    drm_init();
    if (drm_available()) {
        struct vfs_node *dri = vfs_mkdir_p("/dev/dri");
        if (dri) {
            /* read() delivers page-flip completions, so readiness is whether
               any are queued rather than always. */
            struct vfs_node *card = attach_device(dri, "card0", VFS_CHARDEVICE,
                                                  0660, drm_device_read, NULL,
                                                  drm_device_read_ready);
            if (card) {
                card->dev_major = DEV_MAJOR_DRM;
                card->dev_minor = DEV_MINOR_DRM_CARD0;
                card->gid = DEV_GROUP_VIDEO;
                card->ioctl = drm_node_ioctl;
                card->mmap = drm_device_mmap;
                /* Open/close counting is how the console gets the display back
                   when the last client goes away. */
                card->open = drm_device_open;
                card->close = drm_device_close;
            }
        }
    }

    /* /dev/snd, laid out and numbered as ALSA does it: alsa-lib opens these
       paths by name and nothing else will do. */
    sound_init();
    if (sound_card_available()) {
        struct vfs_node *snd = vfs_mkdir_p("/dev/snd");
        if (snd) {
            /* No read-readiness: read() on a control device delivers element
               change events, and this driver never generates one. Claiming
               POLLIN would spin any mixer that waits on it. */
            struct vfs_node *control = attach_device(snd, "controlC0",
                VFS_CHARDEVICE, 0660, NULL, NULL, NULL);
            if (control) {
                control->dev_major = DEV_MAJOR_SOUND;
                control->dev_minor = DEV_MINOR_SOUND_CONTROL;
                control->ioctl = sound_control_ioctl;
                control->gid = DEV_GROUP_AUDIO;
            }

            struct vfs_node *playback = attach_device(snd, "pcmC0D0p",
                VFS_CHARDEVICE, 0660, NULL, sound_pcm_write, NULL);
            if (playback) {
                playback->dev_major = DEV_MAJOR_SOUND;
                playback->dev_minor = DEV_MINOR_SOUND_PCM_PLAYBACK;
                playback->ioctl = sound_pcm_ioctl;
                playback->mmap = sound_pcm_mmap;
                /* Without this a full ring still reports POLLOUT, and a
                   blocked write turns into a busy loop. */
                playback->write_ready = sound_pcm_write_ready;
                playback->open = sound_pcm_open;
                playback->close = sound_pcm_close;
                playback->gid = DEV_GROUP_AUDIO;
            }
        }
    }

    struct vfs_node *input = vfs_mkdir_p("/dev/input");
    if (input) {
        /* Keep the legacy raw-scancode node for console tooling. */
        struct vfs_node *keyboard = attach_device(input, "keyboard", VFS_CHARDEVICE, 0440,
                                                  keyboard_read, NULL, keyboard_ready);
        if (keyboard) {
            keyboard->open = keyboard_open;
            keyboard->close = keyboard_close;
        }

        /* 0660 rather than 0440: evdev is opened read-write, because ioctls
           like EVIOCGRAB and EVIOCSCLOCKID are writes to the device. */
        struct vfs_node *event0 = attach_device(input, "event0",
            VFS_CHARDEVICE | VFS_INPUTDEVICE, 0660, NULL, NULL, NULL);
        if (event0) {
            event0->data = (void *)(uintptr_t)TUNIX_INPUT_DEVICE_KEYBOARD;
            event0->ioctl = input_event_ioctl;
            event0->dev_major = DEV_MAJOR_INPUT;
            event0->dev_minor = DEV_MINOR_INPUT_EVENT_BASE + 0U;
            event0->gid = DEV_GROUP_INPUT;
        }

        if (input_mouse_available()) {
            struct vfs_node *event1 = attach_device(input, "event1",
                VFS_CHARDEVICE | VFS_INPUTDEVICE, 0660, NULL, NULL, NULL);
            if (event1) {
                event1->data = (void *)(uintptr_t)TUNIX_INPUT_DEVICE_MOUSE;
                event1->ioctl = input_event_ioctl;
                event1->dev_major = DEV_MAJOR_INPUT;
                event1->dev_minor = DEV_MINOR_INPUT_EVENT_BASE + 1U;
                event1->gid = DEV_GROUP_INPUT;
            }
            (void)vfs_create_symlink("/dev/input/mouse0", "/dev/input/event1", 0);
        }
    }
    (void)vfs_create_symlink("/dev/rtc0", "/dev/rtc", 0);

    /* Where shm_open(3) puts its files. A tmpfs of its own so that nothing
       under it is ever written to the disk, and a mount so that the init
       scripts see one already there. */
    struct vfs_node *shm = vfs_mkdir_p("/dev/shm");
    if (shm) {
        shm->mode = 01777;
        shm->flags |= VFS_VOLATILE;
        vfs_mount_builtin("shm", "/dev/shm", "tmpfs", shm);
    }

    const struct block_device *root = block_root();
    if (root) {
        char target[5 + BLOCK_NAME_BYTES] = "/dev/";
        memcpy(target + 5, root->dev_name, sizeof root->dev_name);
        (void)vfs_create_symlink("/dev/root", target, 0);
    }
}
