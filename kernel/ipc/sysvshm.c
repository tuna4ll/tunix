#include <stddef.h>
#include <stdint.h>
#include <tunix/cred.h>
#include <tunix/file.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/memfd.h>
#include <tunix/sysvshm.h>
#include <tunix/time.h>
#include <tunix/lock.h>
#include <tunix/syscall.h>

static struct lock shm_lock = LOCK_INITIALIZER("sysv shm", LOCK_RANK_OBJECT);

static void shm_guard_release(int *unused) {
    (void)unused;
    lock_release(&shm_lock);
}

#define SHM_LOCKED \
    __attribute__((cleanup(shm_guard_release))) int shm_guard = (lock_acquire(&shm_lock), 0)

#define SHM_MAX_BYTES 0x00007FFFFFFFFFFFULL

#define EPERM  1
#define ENOENT 2
#define EINVAL 22
#define ENOSPC 28
#define EEXIST 17
#define ENOMEM 12

struct shm_segment {
    int used;
    int destroyed;
    int id;
    int32_t key;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
    uint64_t size;
    uint32_t cpid, lpid;
    int64_t atime, dtime, ctime;
    struct file *file;
};

static struct shm_segment *segments;
static int segment_capacity;
static int next_id = 1;

static int64_t now_seconds(void) { return (int64_t)(time_realtime_ns() / 1000000000ULL); }

static struct shm_segment *find_by_id(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < segment_capacity; i++)
        if (segments[i].used && segments[i].id == id) return &segments[i];
    return NULL;
}

static struct shm_segment *find_by_key(int32_t key) {
    if (key == IPC_PRIVATE) return NULL;
    for (int i = 0; i < segment_capacity; i++)
        if (segments[i].used && !segments[i].destroyed && segments[i].key == key)
            return &segments[i];
    return NULL;
}

static uint64_t attach_count(const struct shm_segment *segment) {
    return segment->file->refs > 1 ? (uint64_t)(segment->file->refs - 1) : 0;
}

static void release(struct shm_segment *segment) {
    if (segment->file) syscall_unref_later(segment->file);
    memset(segment, 0, sizeof(*segment));
}

void sysvshm_reap(void) {
    SHM_LOCKED;
    for (int i = 0; i < segment_capacity; i++)
        if (segments[i].used && segments[i].destroyed && attach_count(&segments[i]) == 0)
            release(&segments[i]);
}

int sysvshm_get(int32_t key, uint64_t size, int flags, uint32_t pid) {
    SHM_LOCKED;
    sysvshm_reap();
    struct shm_segment *existing = find_by_key(key);
    if (existing) {
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -EEXIST;

        if (size && size > existing->size) return -EINVAL;
        return existing->id;
    }
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -ENOENT;
    if (!size || size > SHM_MAX_BYTES) return -EINVAL;

    struct shm_segment *slot = NULL;
    for (int i = 0; i < segment_capacity; i++)
        if (!segments[i].used) {
            slot = &segments[i];
            break;
        }
    if (!slot) {
        int capacity = segment_capacity ? segment_capacity * 2 : 32;
        struct shm_segment *grown = kmalloc((size_t)capacity * sizeof(*grown));
        if (!grown) return -ENOSPC;
        memset(grown, 0, (size_t)capacity * sizeof(*grown));
        if (segment_capacity) memcpy(grown, segments, (size_t)segment_capacity * sizeof(*grown));
        kfree(segments);
        slot = &grown[segment_capacity];
        segments = grown;
        segment_capacity = capacity;
    }

    struct memfd_object *object = memfd_create_object();
    if (!object) return -ENOMEM;
    uint64_t rounded = (size + 4095ULL) & ~4095ULL;
    if (memfd_truncate(object, rounded) != 0) {
        memfd_destroy(object);
        return -ENOMEM;
    }
    struct file *file = file_create_memfd(object, 0);
    if (!file) {
        memfd_destroy(object);
        return -ENOMEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->id = next_id++;
    if (next_id <= 0) next_id = 1;
    slot->key = key;
    slot->mode = (uint32_t)flags & 0777U;
    slot->size = size;
    slot->cpid = pid;
    struct credentials *creator = cred_current();
    if (creator) {
        slot->uid = slot->cuid = creator->euid;
        slot->gid = slot->cgid = creator->egid;
    }
    slot->ctime = now_seconds();
    slot->file = file;
    return slot->id;
}

struct file *sysvshm_acquire(int id, uint64_t *size_out) {
    SHM_LOCKED;
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return NULL;
    if (size_out) *size_out = segment->size;
    file_ref(segment->file);
    return segment->file;
}

void sysvshm_touch(int id, uint32_t pid, int attaching) {
    SHM_LOCKED;
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return;
    segment->lpid = pid;
    if (attaching) segment->atime = now_seconds();
    else segment->dtime = now_seconds();
}

int sysvshm_stat(int id, struct shm_id_ds *out) {
    SHM_LOCKED;
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->key = segment->key;
    out->uid = segment->uid;
    out->gid = segment->gid;
    out->cuid = segment->cuid;
    out->cgid = segment->cgid;
    out->mode = segment->mode | (segment->destroyed ? SHM_DEST : 0U);
    out->segsz = segment->size;
    out->atime = segment->atime;
    out->dtime = segment->dtime;
    out->ctime = segment->ctime;
    out->cpid = (int32_t)segment->cpid;
    out->lpid = (int32_t)segment->lpid;
    out->nattch = attach_count(segment);
    return 0;
}

int sysvshm_set(int id, uint32_t mode, uint32_t uid, uint32_t gid) {
    SHM_LOCKED;
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return -EINVAL;
    segment->mode = mode & 0777U;
    segment->uid = uid;
    segment->gid = gid;
    segment->ctime = now_seconds();
    return 0;
}

int sysvshm_remove(int id) {
    SHM_LOCKED;
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return -EINVAL;

    if (attach_count(segment) > 0) {
        segment->destroyed = 1;
        segment->ctime = now_seconds();
        return 0;
    }
    release(segment);
    return 0;
}
