#include <stddef.h>
#include <stdint.h>
#include <tunix/cred.h>
#include <tunix/errno.h>
#include <tunix/heap.h>
#include <tunix/kstring.h>
#include <tunix/lock.h>
#include <tunix/process.h>
#include <tunix/sysvsem.h>
#include <tunix/sysvshm.h>
#include <tunix/time.h>
#include <tunix/usercopy.h>

#define SEM_SETS_MAX  128
#define SEM_COUNT_MAX 250
#define SEM_VALUE_MAX 32767
#define SEM_OPS_MAX   500
#define SEM_ADJ_MAX   32767

#define GETPID       11
#define GETVAL       12
#define GETALL       13
#define GETNCNT      14
#define GETZCNT      15
#define SETVAL       16
#define SETALL       17
#define SEM_STAT     18
#define SEM_INFO     19
#define SEM_STAT_ANY 20
#define IPC_INFO     3

#define SEM_UNDO   0x1000
#define IPC_NOWAIT 04000

struct sem {
    int32_t value;
    int32_t pid;
    uint32_t waiting_less;
    uint32_t waiting_zero;
};

struct sem_set {
    int id;
    int removed;
    int32_t key;
    uint32_t mode;
    uint32_t uid, gid, cuid, cgid;
    uint32_t seq;
    int64_t otime, ctime;
    uint32_t count;
    uint32_t generation;
    uint32_t sleepers;
    struct sem *sems;
};

struct sem_undo {
    struct sem_undo *next;
    int64_t tgid;
    int id;
    uint32_t count;
    int16_t *adjust;
};

struct sembuf_user {
    uint16_t num;
    int16_t op;
    int16_t flags;
};

static struct lock sem_lock = LOCK_INITIALIZER("sysv sem", LOCK_RANK_OBJECT);
static struct sem_set *sets[SEM_SETS_MAX];
static struct sem_undo *undos;
static uint32_t next_seq;

static int64_t now_seconds(void) { return (int64_t)(time_realtime_ns() / 1000000000ULL); }

static int64_t current_tgid(void) {
    struct process *self = process_current();
    return self ? (int64_t)self->tgid : 0;
}

static int make_id(int slot, uint32_t seq) { return (int)(seq % 32768U) * SEM_SETS_MAX + slot; }

static struct sem_set *find_by_id(int id) {
    if (id < 0) return NULL;
    int slot = id % SEM_SETS_MAX;
    struct sem_set *set = sets[slot];
    if (!set || set->removed || set->id != id) return NULL;
    return set;
}

static struct sem_set *find_by_key(int32_t key) {
    if (key == IPC_PRIVATE) return NULL;
    for (int slot = 0; slot < SEM_SETS_MAX; slot++)
        if (sets[slot] && !sets[slot]->removed && sets[slot]->key == key) return sets[slot];
    return NULL;
}

static int permitted(const struct sem_set *set, int write) {
    const struct credentials *cred = cred_current();
    if (!cred || cred->euid == 0) return 1;
    uint32_t mode = set->mode;
    uint32_t bits;
    if (cred->euid == set->uid || cred->euid == set->cuid) bits = mode >> 6;
    else if (cred_has_group(set->gid) || cred_has_group(set->cgid)) bits = mode >> 3;
    else bits = mode;
    return (bits & (write ? 2U : 4U)) != 0;
}

static int owner_or_root(const struct sem_set *set) {
    const struct credentials *cred = cred_current();
    return !cred || cred->euid == 0 || cred->euid == set->uid || cred->euid == set->cuid;
}

static void drop_undos_for(int id) {
    struct sem_undo **link = &undos;
    while (*link) {
        struct sem_undo *undo = *link;
        if (undo->id == id) {
            *link = undo->next;
            kfree(undo->adjust);
            kfree(undo);
        } else {
            link = &undo->next;
        }
    }
}

static void clear_undo_slot(int id, unsigned num) {
    for (struct sem_undo *undo = undos; undo; undo = undo->next)
        if (undo->id == id && num < undo->count) undo->adjust[num] = 0;
}

static void destroy(struct sem_set *set) {
    set->removed = 1;
    set->generation++;
    drop_undos_for(set->id);
    process_wake_all(set);
    if (!set->sleepers) {
        for (int slot = 0; slot < SEM_SETS_MAX; slot++)
            if (sets[slot] == set) sets[slot] = NULL;
        kfree(set->sems);
        kfree(set);
    }
}

int sysvsem_get(int32_t key, int count, int flags) {
    lock_acquire(&sem_lock);
    struct sem_set *existing = find_by_key(key);
    if (existing) {
        int result;
        if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) result = -EEXIST;
        else if (count > (int)existing->count) result = -EINVAL;
        else if (!permitted(existing, (flags & 0222) != 0) || !permitted(existing, 0))
            result = -EACCES;
        else result = existing->id;
        lock_release(&sem_lock);
        return result;
    }
    if (key != IPC_PRIVATE && !(flags & IPC_CREAT)) {
        lock_release(&sem_lock);
        return -ENOENT;
    }
    if (count <= 0 || count > SEM_COUNT_MAX) {
        lock_release(&sem_lock);
        return -EINVAL;
    }
    int slot = -1;
    for (int index = 0; index < SEM_SETS_MAX; index++)
        if (!sets[index]) {
            slot = index;
            break;
        }
    if (slot < 0) {
        lock_release(&sem_lock);
        return -ENOSPC;
    }
    struct sem_set *set = kmalloc(sizeof(*set));
    struct sem *sems = kmalloc(sizeof(*sems) * (size_t)count);
    if (!set || !sems) {
        kfree(set);
        kfree(sems);
        lock_release(&sem_lock);
        return -ENOMEM;
    }
    memset(set, 0, sizeof(*set));
    memset(sems, 0, sizeof(*sems) * (size_t)count);
    const struct credentials *cred = cred_current();
    set->seq = next_seq++;
    set->id = make_id(slot, set->seq);
    set->key = key;
    set->mode = (uint32_t)flags & 0777U;
    set->uid = set->cuid = cred ? cred->euid : 0;
    set->gid = set->cgid = cred ? cred->egid : 0;
    set->count = (uint32_t)count;
    set->ctime = now_seconds();
    set->sems = sems;
    sets[slot] = set;
    int id = set->id;
    lock_release(&sem_lock);
    return id;
}

static struct sem_undo *undo_for(int64_t tgid, struct sem_set *set) {
    for (struct sem_undo *undo = undos; undo; undo = undo->next)
        if (undo->tgid == tgid && undo->id == set->id) return undo;
    struct sem_undo *undo = kmalloc(sizeof(*undo));
    int16_t *adjust = kmalloc(sizeof(int16_t) * set->count);
    if (!undo || !adjust) {
        kfree(undo);
        kfree(adjust);
        return NULL;
    }
    memset(adjust, 0, sizeof(int16_t) * set->count);
    undo->tgid = tgid;
    undo->id = set->id;
    undo->count = set->count;
    undo->adjust = adjust;
    undo->next = undos;
    undos = undo;
    return undo;
}

static int try_operations(struct sem_set *set, const struct sembuf_user *ops, unsigned count,
                          int pid, unsigned *blocked_at) {
    int32_t staged[SEM_COUNT_MAX];
    for (unsigned index = 0; index < set->count; index++) staged[index] = set->sems[index].value;
    for (unsigned index = 0; index < count; index++) {
        const struct sembuf_user *op = &ops[index];
        int32_t value = staged[op->num];
        if (op->op < 0) {
            if (value + op->op < 0) {
                *blocked_at = index;
                return 1;
            }
            staged[op->num] = value + op->op;
        } else if (op->op == 0) {
            if (value != 0) {
                *blocked_at = index;
                return 1;
            }
        } else {
            if (value + op->op > SEM_VALUE_MAX) return -ERANGE;
            staged[op->num] = value + op->op;
        }
    }
    for (unsigned index = 0; index < count; index++)
        if ((ops[index].flags & SEM_UNDO) && ops[index].op) {
            struct sem_undo *undo = undo_for(current_tgid(), set);
            if (!undo) return -ENOMEM;
            int32_t adjusted = undo->adjust[ops[index].num] - ops[index].op;
            if (adjusted > SEM_ADJ_MAX || adjusted < -SEM_ADJ_MAX) return -ERANGE;
        }
    for (unsigned index = 0; index < count; index++) {
        const struct sembuf_user *op = &ops[index];
        if ((op->flags & SEM_UNDO) && op->op) {
            struct sem_undo *undo = undo_for(current_tgid(), set);
            undo->adjust[op->num] = (int16_t)(undo->adjust[op->num] - op->op);
        }
    }
    int changed = 0;
    for (unsigned index = 0; index < set->count; index++) {
        if (staged[index] != set->sems[index].value) changed = 1;
        set->sems[index].value = staged[index];
    }
    for (unsigned index = 0; index < count; index++) set->sems[ops[index].num].pid = pid;
    set->otime = now_seconds();
    if (changed) {
        set->generation++;
        process_wake_all(set);
    }
    return 0;
}

int64_t sysvsem_op(int id, uint64_t user_ops, unsigned count, int64_t timeout_ns) {
    if (!count) return -EINVAL;
    if (count > SEM_OPS_MAX) return -E2BIG;
    struct sembuf_user *ops = kmalloc(sizeof(*ops) * count);
    if (!ops) return -ENOMEM;
    if (copy_from_user(ops, user_ops, sizeof(*ops) * count) != 0) {
        kfree(ops);
        return -EFAULT;
    }
    uint64_t deadline = timeout_ns >= 0 ? time_uptime_ns() + (uint64_t)timeout_ns : 0;
    int pid = (int)current_tgid();
    int writes = 0;
    for (unsigned index = 0; index < count; index++)
        if (ops[index].op) writes = 1;
    int64_t result;
    lock_acquire(&sem_lock);
    struct sem_set *set = find_by_id(id);
    if (!set) {
        result = -EINVAL;
        goto out;
    }
    for (unsigned index = 0; index < count; index++)
        if (ops[index].num >= set->count) {
            result = -EFBIG;
            goto out;
        }
    if (!permitted(set, writes)) {
        result = -EACCES;
        goto out;
    }
    for (;;) {
        unsigned blocked_at = 0;
        int status = try_operations(set, ops, count, pid, &blocked_at);
        if (status <= 0) {
            result = status;
            break;
        }
        if (ops[blocked_at].flags & IPC_NOWAIT) {
            result = -EAGAIN;
            break;
        }
        if (timeout_ns >= 0 && time_uptime_ns() >= deadline) {
            result = -EAGAIN;
            break;
        }
        struct sem *waited = &set->sems[ops[blocked_at].num];
        int zero = ops[blocked_at].op == 0;
        if (zero) waited->waiting_zero++;
        else waited->waiting_less++;
        set->sleepers++;
        uint32_t generation = set->generation;
        lock_release(&sem_lock);
        int prepared = process_prepare_wait_interruptible(set, timeout_ns >= 0 ? deadline : 0);
        lock_acquire(&sem_lock);
        int ready = prepared != 0 || set->generation != generation || set->removed;
        lock_release(&sem_lock);
        if (prepared == 0) {
            if (!ready) process_wait();
            process_finish_wait();
        }
        lock_acquire(&sem_lock);
        set->sleepers--;
        if (zero) waited->waiting_zero--;
        else waited->waiting_less--;
        if (set->removed) {
            if (!set->sleepers) destroy(set);
            result = -EIDRM;
            break;
        }
        if (prepared != 0 || process_wait_interrupted()) {
            result = -EINTR;
            break;
        }
    }
out:
    lock_release(&sem_lock);
    kfree(ops);
    return result;
}

struct semid_ds_user {
    int32_t key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    uint16_t seq;
    uint16_t pad;
    uint32_t pad2;
    uint64_t unused1;
    uint64_t unused2;
    int64_t otime;
#if defined(__x86_64__)
    uint64_t unused3;
#endif
    int64_t ctime;
#if defined(__x86_64__)
    uint64_t unused4;
#endif
    uint64_t nsems;
    uint64_t unused5;
    uint64_t unused6;
};

struct seminfo_user {
    int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem;
};

static void fill_stat(const struct sem_set *set, struct semid_ds_user *out) {
    memset(out, 0, sizeof(*out));
    out->key = set->key;
    out->uid = set->uid;
    out->gid = set->gid;
    out->cuid = set->cuid;
    out->cgid = set->cgid;
    out->mode = set->mode;
    out->seq = (uint16_t)set->seq;
    out->otime = set->otime;
    out->ctime = set->ctime;
    out->nsems = set->count;
}

int64_t sysvsem_ctl(int id, int num, int command, uint64_t argument) {
    command &= ~IPC_64;
    if (command == IPC_INFO || command == SEM_INFO) {
        struct seminfo_user info = {0};
        info.semmni = SEM_SETS_MAX;
        info.semmsl = SEM_COUNT_MAX;
        info.semmns = SEM_SETS_MAX * SEM_COUNT_MAX;
        info.semopm = SEM_OPS_MAX;
        info.semvmx = SEM_VALUE_MAX;
        info.semaem = SEM_ADJ_MAX;
        info.semmnu = info.semmns;
        info.semmap = info.semmns;
        info.semume = SEM_OPS_MAX;
        info.semusz = (int32_t)sizeof(struct sem_undo);
        int highest = 0;
        lock_acquire(&sem_lock);
        for (int slot = 0; slot < SEM_SETS_MAX; slot++)
            if (sets[slot] && !sets[slot]->removed) {
                highest = slot;
                if (command == SEM_INFO) {
                    info.semusz++;
                    info.semaem += (int32_t)sets[slot]->count;
                }
            }
        lock_release(&sem_lock);
        if (copy_to_user(argument, &info, sizeof(info)) != 0) return -EFAULT;
        return highest;
    }
    lock_acquire(&sem_lock);
    struct sem_set *set;
    if (command == SEM_STAT || command == SEM_STAT_ANY)
        set = id >= 0 && id < SEM_SETS_MAX && sets[id] && !sets[id]->removed ? sets[id] : NULL;
    else set = find_by_id(id);
    if (!set) {
        lock_release(&sem_lock);
        return -EINVAL;
    }
    int64_t result = 0;
    switch (command) {
    case IPC_STAT:
    case SEM_STAT:
    case SEM_STAT_ANY: {
        if (command != SEM_STAT_ANY && !permitted(set, 0)) {
            result = -EACCES;
            break;
        }
        struct semid_ds_user value;
        fill_stat(set, &value);
        int reply = command == IPC_STAT ? 0 : set->id;
        lock_release(&sem_lock);
        if (copy_to_user(argument, &value, sizeof(value)) != 0) return -EFAULT;
        return reply;
    }
    case IPC_SET: {
        struct semid_ds_user value;
        lock_release(&sem_lock);
        if (copy_from_user(&value, argument, sizeof(value)) != 0) return -EFAULT;
        lock_acquire(&sem_lock);
        set = find_by_id(id);
        if (!set) result = -EINVAL;
        else if (!owner_or_root(set)) result = -EPERM;
        else {
            set->uid = value.uid;
            set->gid = value.gid;
            set->mode = (set->mode & ~0777U) | (value.mode & 0777U);
            set->ctime = now_seconds();
        }
        break;
    }
    case IPC_RMID:
        if (!owner_or_root(set)) result = -EPERM;
        else destroy(set);
        break;
    case GETVAL:
    case GETPID:
    case GETNCNT:
    case GETZCNT:
        if (num < 0 || (uint32_t)num >= set->count) result = -EINVAL;
        else if (!permitted(set, 0)) result = -EACCES;
        else if (command == GETVAL) result = set->sems[num].value;
        else if (command == GETPID) result = set->sems[num].pid;
        else if (command == GETNCNT) result = set->sems[num].waiting_less;
        else result = set->sems[num].waiting_zero;
        break;
    case SETVAL: {
        int32_t value = (int32_t)(uint32_t)argument;
        if (num < 0 || (uint32_t)num >= set->count) result = -EINVAL;
        else if (value < 0 || value > SEM_VALUE_MAX) result = -ERANGE;
        else if (!permitted(set, 1)) result = -EACCES;
        else {
            set->sems[num].value = value;
            set->sems[num].pid = (int32_t)current_tgid();
            set->ctime = now_seconds();
            clear_undo_slot(set->id, (unsigned)num);
            set->generation++;
            process_wake_all(set);
        }
        break;
    }
    case GETALL: {
        if (!permitted(set, 0)) {
            result = -EACCES;
            break;
        }
        uint16_t values[SEM_COUNT_MAX];
        uint32_t count = set->count;
        for (uint32_t index = 0; index < count; index++)
            values[index] = (uint16_t)set->sems[index].value;
        lock_release(&sem_lock);
        return copy_to_user(argument, values, sizeof(uint16_t) * count) == 0 ? 0 : -EFAULT;
    }
    case SETALL: {
        uint32_t count = set->count;
        uint16_t values[SEM_COUNT_MAX];
        lock_release(&sem_lock);
        if (copy_from_user(values, argument, sizeof(uint16_t) * count) != 0) return -EFAULT;
        lock_acquire(&sem_lock);
        set = find_by_id(id);
        if (!set || set->count != count) {
            result = -EINVAL;
            break;
        }
        if (!permitted(set, 1)) {
            result = -EACCES;
            break;
        }
        for (uint32_t index = 0; index < count; index++)
            if (values[index] > SEM_VALUE_MAX) result = -ERANGE;
        if (result) break;
        for (uint32_t index = 0; index < count; index++) {
            set->sems[index].value = values[index];
            set->sems[index].pid = (int32_t)current_tgid();
            clear_undo_slot(set->id, index);
        }
        set->ctime = now_seconds();
        set->generation++;
        process_wake_all(set);
        break;
    }
    default: result = -EINVAL; break;
    }
    lock_release(&sem_lock);
    return result;
}

void sysvsem_exit(int64_t tgid) {
    lock_acquire(&sem_lock);
    struct sem_undo **link = &undos;
    while (*link) {
        struct sem_undo *undo = *link;
        if (undo->tgid != tgid) {
            link = &undo->next;
            continue;
        }
        *link = undo->next;
        struct sem_set *set = find_by_id(undo->id);
        if (set) {
            int changed = 0;
            for (uint32_t index = 0; index < undo->count && index < set->count; index++) {
                if (!undo->adjust[index]) continue;
                int32_t value = set->sems[index].value + undo->adjust[index];
                if (value < 0) value = 0;
                if (value > SEM_VALUE_MAX) value = SEM_VALUE_MAX;
                set->sems[index].value = value;
                set->sems[index].pid = (int32_t)tgid;
                changed = 1;
            }
            if (changed) {
                set->generation++;
                process_wake_all(set);
            }
        }
        kfree(undo->adjust);
        kfree(undo);
    }
    lock_release(&sem_lock);
}
