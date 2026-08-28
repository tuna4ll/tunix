#include <stddef.h>
#include <stdint.h>
#include "../include/cred.h"
#include "../include/file.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/memfd.h"
#include "../include/sysvshm.h"
#include "../include/time.h"

/*
 * See include/sysvshm.h for what this is for. A fixed table of segments, each
 * holding one reference to a memfd-backed struct file; the attach count is read
 * back out of that file's reference count rather than tracked here, so a
 * process that exits without shmdt() cannot leave the number wrong.
 */

#define SHM_MAX_SEGMENTS 128
#define SHM_MAX_BYTES (64ULL * 1024ULL * 1024ULL)

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

static struct shm_segment segments[SHM_MAX_SEGMENTS];
static int next_id = 1;

static int64_t now_seconds(void) {
    return (int64_t)(time_realtime_ns() / 1000000000ULL);
}

static struct shm_segment *find_by_id(int id) {
    if (id <= 0) return NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (segments[i].used && segments[i].id == id) return &segments[i];
    return NULL;
}

static struct shm_segment *find_by_key(int32_t key) {
    if (key == IPC_PRIVATE) return NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (segments[i].used && !segments[i].destroyed && segments[i].key == key)
            return &segments[i];
    return NULL;
}

/* One reference is the table's own, the rest are attached mappings. */
static uint64_t attach_count(const struct shm_segment *segment) {
    return segment->file->refs > 1 ? (uint64_t)(segment->file->refs - 1) : 0;
}

static void release(struct shm_segment *segment) {
    if (segment->file) file_unref(segment->file);
    memset(segment, 0, sizeof(*segment));
}

void sysvshm_reap(void) {
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (segments[i].used && segments[i].destroyed && attach_count(&segments[i]) == 0)
            release(&segments[i]);
}

int sysvshm_get(int32_t key, uint64_t size, int flags, uint32_t pid) {
    sysvshm_reap();
    struct shm_segment *existing = find_by_key(key);
    if (existing) {
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -EEXIST;
        /* A size of 0 means "whatever it already is"; anything larger than the
           segment is a request this one cannot satisfy. */
        if (size && size > existing->size) return -EINVAL;
        return existing->id;
    }
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) return -ENOENT;
    if (!size || size > SHM_MAX_BYTES) return -EINVAL;

    struct shm_segment *slot = NULL;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++)
        if (!segments[i].used) { slot = &segments[i]; break; }
    if (!slot) return -ENOSPC;

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
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return NULL;
    if (size_out) *size_out = segment->size;
    file_ref(segment->file);
    return segment->file;
}

void sysvshm_touch(int id, uint32_t pid, int attaching) {
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return;
    segment->lpid = pid;
    if (attaching) segment->atime = now_seconds();
    else segment->dtime = now_seconds();
}

int sysvshm_stat(int id, struct shm_id_ds *out) {
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
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return -EINVAL;
    segment->mode = mode & 0777U;
    segment->uid = uid;
    segment->gid = gid;
    segment->ctime = now_seconds();
    return 0;
}

int sysvshm_remove(int id) {
    struct shm_segment *segment = find_by_id(id);
    if (!segment) return -EINVAL;
    /* While anything is attached the id has to keep resolving, because a caller
       is allowed to remove a segment before handing the id to whoever attaches
       next -- see the note in sysvshm.h. */
    if (attach_count(segment) > 0) {
        segment->destroyed = 1;
        segment->ctime = now_seconds();
        return 0;
    }
    release(segment);
    return 0;
}
