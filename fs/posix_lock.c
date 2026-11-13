#include <stddef.h>
#include <stdint.h>

#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/mutex.h>
#include <tunix/posix_lock.h>
#include <tunix/process.h>
#include <tunix/vfs.h>
#include <tunix/errno.h>

#define DEADLOCK_SEARCH_DEPTH 10

struct posix_lock {
    struct posix_lock *next;
    uint64_t owner;
    int32_t pid;
    int type;
    uint64_t start;
    uint64_t end;
};

struct lock_waiter {
    struct lock_waiter *next;
    uint64_t owner;
    uint64_t blocker;
};

static struct mutex posix_lock_mutex = MUTEX_INITIALIZER("posix locks", LOCK_RANK_OBJECT);
static struct lock_waiter *waiters;

static int overlaps(uint64_t start, uint64_t end, const struct posix_lock *lock) {
    return start <= lock->end && lock->start <= end;
}

static int touches(uint64_t start, uint64_t end, const struct posix_lock *lock) {
    int before = lock->end != POSIX_LOCK_END && lock->end + 1U < start;
    int after = end != POSIX_LOCK_END && end + 1U < lock->start;
    return !before && !after;
}

static struct posix_lock *find_conflict(struct vfs_node *node,
                                        const struct posix_lock_request *request) {
    for (struct posix_lock *lock = node->posix_locks; lock; lock = lock->next) {
        if (lock->owner == request->owner) continue;
        if (!overlaps(request->start, request->end, lock)) continue;
        if (lock->type == POSIX_LOCK_WRITE || request->type == POSIX_LOCK_WRITE) return lock;
    }
    return NULL;
}

static int would_deadlock(uint64_t owner, uint64_t blocker) {
    for (unsigned depth = 0; depth < DEADLOCK_SEARCH_DEPTH; depth++) {
        if (blocker == owner) return 1;
        struct lock_waiter *waiter = waiters;
        while (waiter && waiter->owner != blocker) waiter = waiter->next;
        if (!waiter) return 0;
        blocker = waiter->blocker;
    }
    return 0;
}

static void forget_waiter(struct lock_waiter *waiter) {
    struct lock_waiter **link = &waiters;
    while (*link && *link != waiter) link = &(*link)->next;
    if (*link) *link = waiter->next;
}

int posix_lock_test(struct vfs_node *node, struct posix_lock_request *request) {
    mutex_lock(&posix_lock_mutex);
    struct posix_lock *conflict = find_conflict(node, request);
    if (conflict) {
        request->type = conflict->type;
        request->start = conflict->start;
        request->end = conflict->end;
        request->pid = conflict->pid;
    } else {
        request->type = POSIX_LOCK_UNLOCK;
    }
    mutex_unlock(&posix_lock_mutex);
    return 0;
}

static struct posix_lock *take_spare(struct posix_lock **spare) {
    for (int index = 0; index < 2; index++) {
        if (!spare[index]) continue;
        struct posix_lock *lock = spare[index];
        spare[index] = NULL;
        return lock;
    }
    return NULL;
}

static void apply(struct vfs_node *node, const struct posix_lock_request *request,
                  struct posix_lock **spare) {
    uint64_t start = request->start;
    uint64_t end = request->end;
    struct posix_lock **link = &node->posix_locks;
    while (*link) {
        struct posix_lock *lock = *link;
        if (lock->owner != request->owner) {
            link = &lock->next;
            continue;
        }
        if (request->type != POSIX_LOCK_UNLOCK && lock->type == request->type &&
            touches(start, end, lock)) {
            if (lock->start < start) start = lock->start;
            if (lock->end > end) end = lock->end;
            *link = lock->next;
            kfree(lock);
            continue;
        }
        if (!overlaps(request->start, request->end, lock)) {
            link = &lock->next;
            continue;
        }
        if (lock->start < request->start && lock->end > request->end) {
            struct posix_lock *tail = take_spare(spare);
            *tail = *lock;
            tail->start = request->end + 1U;
            lock->end = request->start - 1U;
            tail->next = lock->next;
            lock->next = tail;
            link = &tail->next;
        } else if (lock->start < request->start) {
            lock->end = request->start - 1U;
            link = &lock->next;
        } else if (lock->end > request->end) {
            lock->start = request->end + 1U;
            link = &lock->next;
        } else {
            *link = lock->next;
            kfree(lock);
        }
    }
    if (request->type == POSIX_LOCK_UNLOCK) return;
    struct posix_lock *added = take_spare(spare);
    added->owner = request->owner;
    added->pid = request->pid;
    added->type = request->type;
    added->start = start;
    added->end = end;
    added->next = node->posix_locks;
    node->posix_locks = added;
}

int posix_lock_set(struct vfs_node *node, const struct posix_lock_request *request, int wait) {
    struct posix_lock *spare[2] = {(struct posix_lock *)kmalloc(sizeof(struct posix_lock)),
                                   (struct posix_lock *)kmalloc(sizeof(struct posix_lock))};
    if (!spare[0] || !spare[1]) {
        kfree(spare[0]);
        kfree(spare[1]);
        return -ENOLCK;
    }
    struct lock_waiter waiter = {NULL, request->owner, 0};
    int status = 0;
    mutex_lock(&posix_lock_mutex);
    while (request->type != POSIX_LOCK_UNLOCK) {
        struct posix_lock *blocker = find_conflict(node, request);
        if (!blocker) break;
        if (!wait) {
            status = -EAGAIN;
            break;
        }
        if ((request->owner & 1U) == 0 && would_deadlock(request->owner, blocker->owner)) {
            status = -EDEADLK;
            break;
        }
        waiter.blocker = blocker->owner;
        waiter.next = waiters;
        waiters = &waiter;
        status = process_prepare_wait_interruptible(&node->posix_locks, 0);
        if (status == 0) {
            mutex_unlock(&posix_lock_mutex);
            process_wait();
            process_finish_wait();
            mutex_lock(&posix_lock_mutex);
            if (process_wait_interrupted()) status = -EINTR;
        }
        forget_waiter(&waiter);
        if (status != 0) break;
    }
    if (status == 0) {
        apply(node, request, spare);
        process_wake_all(&node->posix_locks);
    }
    mutex_unlock(&posix_lock_mutex);
    kfree(spare[0]);
    kfree(spare[1]);
    return status;
}

void posix_lock_release_owner(struct vfs_node *node, uint64_t owner) {
    if (!node || !__atomic_load_n(&node->posix_locks, __ATOMIC_ACQUIRE)) return;
    mutex_lock(&posix_lock_mutex);
    int released = 0;
    struct posix_lock **link = &node->posix_locks;
    while (*link) {
        struct posix_lock *lock = *link;
        if (lock->owner != owner) {
            link = &lock->next;
            continue;
        }
        *link = lock->next;
        kfree(lock);
        released = 1;
    }
    if (released) process_wake_all(&node->posix_locks);
    mutex_unlock(&posix_lock_mutex);
}
