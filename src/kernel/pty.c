#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/kstring.h"
#include "include/process.h"
#include "include/pty.h"
#include "include/signal.h"
#include "include/tty.h"
#include "include/usercopy.h"
#include "include/vfs.h"

#define EAGAIN 11
#define EFAULT 14
#define EINVAL 22
#define EIO 5
#define ENOENT 2
#define ENOTTY 25
#define ENXIO 6

#define TIOCSCTTY   0x540EUL
#define TIOCGPTN    0x80045430UL
#define TIOCSPTLCK  0x40045431UL
#define TIOCGPTLCK  0x80045439UL
#define TIOCSWINSZ  0x5414UL
#define FIONREAD    0x541BUL

#define PTY_QUEUE_CAPACITY 8192U
#define PTY_CANON_CAPACITY 1024U
#define TTY_ICRNL 0x00000100U
#define TTY_OPOST 0x00000001U
#define TTY_ONLCR 0x00000004U

struct pty_queue {
    uint8_t bytes[PTY_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
};

struct pty_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
};

struct pty_pair {
    int number;
    int allocated;
    int locked;
    int master_files;
    int slave_files;
    int slave_ever_opened;
    int foreground_pgid;
    int eof_pending;
    struct tunix_termios termios;
    struct pty_winsize winsize;
    struct pty_queue to_master;
    struct pty_queue to_slave;
    uint8_t canonical[PTY_CANON_CAPACITY];
    size_t canonical_length;
    struct vfs_node *slave_node;
};

static struct pty_pair pairs[PTY_MAX_PAIRS];
static struct vfs_node *ptmx_node;
static struct vfs_node *tty_node;

static void queue_reset(struct pty_queue *queue) {
    queue->head = queue->tail = queue->count = 0;
}

static int queue_push(struct pty_queue *queue, uint8_t value) {
    if (queue->count >= PTY_QUEUE_CAPACITY) return -1;
    queue->bytes[queue->head] = value;
    queue->head = (queue->head + 1U) % PTY_QUEUE_CAPACITY;
    queue->count++;
    return 0;
}

static int queue_pop(struct pty_queue *queue) {
    if (!queue->count) return -1;
    int value = queue->bytes[queue->tail];
    queue->tail = (queue->tail + 1U) % PTY_QUEUE_CAPACITY;
    queue->count--;
    return value;
}

static void initialize_termios(struct tunix_termios *termios) {
    memset(termios, 0, sizeof(*termios));
    termios->iflag = 0x00000500U;
    termios->oflag = TTY_OPOST | TTY_ONLCR;
    termios->cflag = 0x000000BFU;
    termios->lflag = TTY_ECHO | TTY_ECHOE | TTY_ECHOK |
                     TTY_ICANON | TTY_ISIG | TTY_IEXTEN;
    termios->cc[TTY_VINTR] = 3;
    termios->cc[TTY_VQUIT] = 28;
    termios->cc[TTY_VERASE] = 127;
    termios->cc[TTY_VKILL] = 21;
    termios->cc[TTY_VEOF] = 4;
    termios->cc[TTY_VTIME] = 0;
    termios->cc[TTY_VMIN] = 1;
    termios->cc[TTY_VSTART] = 17;
    termios->cc[TTY_VSTOP] = 19;
    termios->cc[TTY_VSUSP] = 26;
    termios->ispeed = 38400;
    termios->ospeed = 38400;
}

static void reset_pair(struct pty_pair *pty) {
    pty->locked = 1;
    pty->master_files = 0;
    pty->slave_files = 0;
    pty->slave_ever_opened = 0;
    pty->foreground_pgid = 0;
    pty->eof_pending = 0;
    pty->canonical_length = 0;
    queue_reset(&pty->to_master);
    queue_reset(&pty->to_slave);
    initialize_termios(&pty->termios);
    pty->winsize.rows = 25;
    pty->winsize.cols = 80;
    pty->winsize.xpixel = 0;
    pty->winsize.ypixel = 0;
}

static void echo_byte(struct pty_pair *pty, uint8_t value) {
    if (!(pty->termios.lflag & TTY_ECHO)) return;
    if (value == '\n' && (pty->termios.oflag & TTY_OPOST) &&
        (pty->termios.oflag & TTY_ONLCR)) {
        (void)queue_push(&pty->to_master, '\r');
    }
    (void)queue_push(&pty->to_master, value);
}

static void echo_erase(struct pty_pair *pty) {
    if (!(pty->termios.lflag & TTY_ECHO)) return;
    (void)queue_push(&pty->to_master, '\b');
    (void)queue_push(&pty->to_master, ' ');
    (void)queue_push(&pty->to_master, '\b');
}

static void flush_canonical(struct pty_pair *pty) {
    for (size_t index = 0; index < pty->canonical_length; index++) {
        if (queue_push(&pty->to_slave, pty->canonical[index]) != 0) break;
    }
    pty->canonical_length = 0;
}

static void signal_foreground(struct pty_pair *pty, int signal_number) {
    if (pty->foreground_pgid > 0)
        (void)process_send_signal(-(int64_t)pty->foreground_pgid, signal_number);
}

static size_t master_feed_input(struct pty_pair *pty, const uint8_t *bytes,
                                size_t size) {
    size_t completed = 0;
    for (; completed < size; completed++) {
        uint8_t value = bytes[completed];
        if ((pty->termios.iflag & TTY_ICRNL) && value == '\r') value = '\n';

        if ((pty->termios.lflag & TTY_ISIG) && value == pty->termios.cc[TTY_VINTR]) {
            signal_foreground(pty, SIGINT);
            pty->canonical_length = 0;
            if (pty->termios.lflag & TTY_ECHO) {
                (void)queue_push(&pty->to_master, '^');
                (void)queue_push(&pty->to_master, 'C');
                (void)queue_push(&pty->to_master, '\r');
                (void)queue_push(&pty->to_master, '\n');
            }
            continue;
        }

        if ((pty->termios.lflag & TTY_ISIG) && value == pty->termios.cc[TTY_VSUSP]) {
            signal_foreground(pty, SIGTSTP);
            pty->canonical_length = 0;
            if (pty->termios.lflag & TTY_ECHO) {
                (void)queue_push(&pty->to_master, '^');
                (void)queue_push(&pty->to_master, 'Z');
                (void)queue_push(&pty->to_master, '\r');
                (void)queue_push(&pty->to_master, '\n');
            }
            continue;
        }

        if (!(pty->termios.lflag & TTY_ICANON)) {
            if (queue_push(&pty->to_slave, value) != 0) break;
            echo_byte(pty, value);
            continue;
        }

        if (value == pty->termios.cc[TTY_VERASE]) {
            if (pty->canonical_length) {
                pty->canonical_length--;
                if (pty->termios.lflag & TTY_ECHOE) echo_erase(pty);
            }
            continue;
        }
        if (value == pty->termios.cc[TTY_VKILL]) {
            while (pty->canonical_length) {
                pty->canonical_length--;
                if (pty->termios.lflag & TTY_ECHOE) echo_erase(pty);
            }
            continue;
        }
        if (value == pty->termios.cc[TTY_VEOF]) {
            if (pty->canonical_length) flush_canonical(pty);
            else pty->eof_pending = 1;
            continue;
        }
        if (pty->canonical_length >= PTY_CANON_CAPACITY) break;
        pty->canonical[pty->canonical_length++] = value;
        echo_byte(pty, value);
        if (value == '\n') flush_canonical(pty);
    }
    return completed;
}

void pty_init(void) {
    struct vfs_node *dev = vfs_mkdir_p("/dev");
    struct vfs_node *pts = vfs_mkdir_p("/dev/pts");
    ptmx_node = vfs_alloc_node("ptmx", VFS_CHARDEVICE);
    if (ptmx_node) {
        ptmx_node->mode = 0666;
        (void)vfs_attach(dev, ptmx_node);
    }
    tty_node = vfs_alloc_node("tty", VFS_CHARDEVICE);
    if (tty_node) {
        tty_node->mode = 0666;
        (void)vfs_attach(dev, tty_node);
    }
    for (int index = 0; index < PTY_MAX_PAIRS; index++) {
        struct pty_pair *pty = &pairs[index];
        memset(pty, 0, sizeof(*pty));
        pty->number = index;
        char name[4];
        name[0] = (char)('0' + index);
        name[1] = '\0';
        pty->slave_node = vfs_alloc_node(name, VFS_CHARDEVICE);
        if (pty->slave_node) {
            pty->slave_node->mode = 0620;
            pty->slave_node->data = pty;
            (void)vfs_attach(pts, pty->slave_node);
        }
    }
}

struct file *pty_open_master(struct vfs_node *node, uint32_t flags) {
    if (!node || node != ptmx_node) return NULL;
    for (int index = 0; index < PTY_MAX_PAIRS; index++) {
        struct pty_pair *pty = &pairs[index];
        if (pty->allocated) continue;
        pty->allocated = 1;
        reset_pair(pty);
        struct file *file = file_create_pty_endpoint(pty, 1, node, flags);
        if (!file) {
            pty->allocated = 0;
            return NULL;
        }
        pty->master_files = 1;
        return file;
    }
    return NULL;
}

struct file *pty_open_slave(struct vfs_node *node, uint32_t flags) {
    if (!node || !node->data) return NULL;
    struct pty_pair *pty = (struct pty_pair *)node->data;
    if (!pty->allocated || pty->locked || pty->master_files <= 0) return NULL;
    struct file *file = file_create_pty_endpoint(pty, 0, node, flags);
    if (!file) return NULL;
    pty->slave_files++;
    pty->slave_ever_opened = 1;
    return file;
}


struct file *pty_open_controlling(struct vfs_node *node, uint32_t flags) {
    struct process *process = process_current();
    if (!node || node != tty_node || !process || !process->controlling_pty) return NULL;
    struct pty_pair *pty = process->controlling_pty;
    if (!pty->allocated || pty->master_files <= 0) return NULL;
    struct file *file = file_create_pty_endpoint(pty, 0, node, flags);
    if (!file) return NULL;
    pty->slave_files++;
    pty->slave_ever_opened = 1;
    return file;
}

void pty_ref_endpoint(struct pty_pair *pty, int master) {
    if (!pty) return;
    if (master) pty->master_files++;
    else pty->slave_files++;
}

void pty_close_endpoint(struct pty_pair *pty, int master) {
    if (!pty) return;
    if (master) {
        if (pty->master_files > 0) pty->master_files--;
        if (pty->master_files == 0 && pty->foreground_pgid > 0)
            signal_foreground(pty, SIGHUP);
    } else if (pty->slave_files > 0) {
        pty->slave_files--;
    }
    if (pty->master_files == 0 && pty->slave_files == 0) {
        pty->allocated = 0;
        reset_pair(pty);
    }
}

int64_t pty_read(struct pty_pair *pty, int master, size_t size, void *buffer) {
    if (!pty || !buffer) return -EINVAL;
    struct pty_queue *queue = master ? &pty->to_master : &pty->to_slave;
    if (!queue->count) {
        if (!master && pty->eof_pending) {
            pty->eof_pending = 0;
            return 0;
        }
        if (master && pty->slave_ever_opened && pty->slave_files == 0) return 0;
        if (!master && pty->master_files == 0) return 0;
        return -EAGAIN;
    }
    uint8_t *out = (uint8_t *)buffer;
    size_t completed = 0;
    while (completed < size && queue->count) out[completed++] = (uint8_t)queue_pop(queue);
    return (int64_t)completed;
}

int64_t pty_write(struct pty_pair *pty, int master, size_t size, const void *buffer) {
    if (!pty || !buffer) return -EINVAL;
    if ((master && pty->slave_ever_opened && pty->slave_files == 0) ||
        (!master && pty->master_files == 0)) return -EIO;
    const uint8_t *bytes = (const uint8_t *)buffer;
    if (master) {
        size_t completed = master_feed_input(pty, bytes, size);
        return completed ? (int64_t)completed : -EAGAIN;
    }
    size_t completed = 0;
    for (; completed < size; completed++) {
        uint8_t value = bytes[completed];
        if (value == '\n' && (pty->termios.oflag & TTY_OPOST) &&
            (pty->termios.oflag & TTY_ONLCR)) {
            if (queue_push(&pty->to_master, '\r') != 0) break;
        }
        if (queue_push(&pty->to_master, value) != 0) break;
    }
    return completed ? (int64_t)completed : -EAGAIN;
}

int pty_read_ready(struct pty_pair *pty, int master) {
    if (!pty) return 0;
    if (master) return pty->to_master.count > 0 ||
                       (pty->slave_ever_opened && pty->slave_files == 0);
    return pty->to_slave.count > 0 || pty->eof_pending || pty->master_files == 0;
}

int pty_write_ready(struct pty_pair *pty, int master) {
    if (!pty) return 0;
    if (master) return !(pty->slave_ever_opened && pty->slave_files == 0) &&
                       pty->to_slave.count < PTY_QUEUE_CAPACITY &&
                       pty->canonical_length < PTY_CANON_CAPACITY;
    return pty->to_master.count < PTY_QUEUE_CAPACITY && pty->master_files > 0;
}

int64_t pty_ioctl(struct pty_pair *pty, int master, unsigned long request,
                  uint64_t user_argument) {
    if (!pty) return -ENXIO;
    if (request == TIOCGPTN && master) {
        int number = pty->number;
        return copy_to_user(user_argument, &number, sizeof(number)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSPTLCK && master) {
        int locked;
        if (copy_from_user(&locked, user_argument, sizeof(locked)) != 0) return -EFAULT;
        pty->locked = locked != 0;
        return 0;
    }
    if (request == TIOCGPTLCK && master) {
        int locked = pty->locked;
        return copy_to_user(user_argument, &locked, sizeof(locked)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSCTTY) {
        struct process *process = process_current();
        if (process) {
            process->controlling_pty = pty;
            pty->foreground_pgid = (int)process->pgid;
        }
        return 0;
    }
    if (request == TCGETS) {
        return copy_to_user(user_argument, &pty->termios, sizeof(pty->termios)) == 0 ? 0 : -EFAULT;
    }
    if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
        struct tunix_termios value;
        if (copy_from_user(&value, user_argument, sizeof(value)) != 0) return -EFAULT;
        pty->termios = value;
        if (request == TCSETSF) {
            pty->canonical_length = 0;
            pty->eof_pending = 0;
            queue_reset(&pty->to_slave);
        }
        return 0;
    }
    if (request == TIOCGPGRP) {
        int pgid = pty->foreground_pgid;
        return copy_to_user(user_argument, &pgid, sizeof(pgid)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSPGRP) {
        int pgid;
        if (copy_from_user(&pgid, user_argument, sizeof(pgid)) != 0) return -EFAULT;
        pty->foreground_pgid = pgid;
        return 0;
    }
    if (request == TIOCGWINSZ) {
        return copy_to_user(user_argument, &pty->winsize, sizeof(pty->winsize)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSWINSZ) {
        return copy_from_user(&pty->winsize, user_argument, sizeof(pty->winsize)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCGETD) {
        int discipline = 0;
        return copy_to_user(user_argument, &discipline, sizeof(discipline)) == 0 ? 0 : -EFAULT;
    }
    if (request == TIOCSETD) {
        int discipline;
        if (copy_from_user(&discipline, user_argument, sizeof(discipline)) != 0) return -EFAULT;
        return discipline == 0 ? 0 : -EINVAL;
    }
    if (request == FIONREAD) {
        int available = (int)(master ? pty->to_master.count : pty->to_slave.count);
        return copy_to_user(user_argument, &available, sizeof(available)) == 0 ? 0 : -EFAULT;
    }
    return -ENOTTY;
}
