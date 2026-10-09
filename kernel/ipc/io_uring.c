#include <stddef.h>
#include <stdint.h>
#include <tunix/heap.h>
#include <tunix/io_uring.h>
#include <tunix/kstring.h>
#include <tunix/memfd.h>
#include <tunix/mutex.h>
#include <tunix/time.h>
#include <tunix/usercopy.h>
#include <tunix/vmm.h>

#define ENOENT 2
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ETIME 62
#define ECANCELED 125

#define RING_PAGE 4096ULL
#define RING_MAX_ENTRIES 32768U
#define RING_MAX_CQ_ENTRIES (2U * RING_MAX_ENTRIES)

#define SETUP_IOPOLL (1U << 0)
#define SETUP_SQPOLL (1U << 1)
#define SETUP_SQ_AFF (1U << 2)
#define SETUP_CQSIZE (1U << 3)
#define SETUP_CLAMP (1U << 4)
#define SETUP_ATTACH_WQ (1U << 5)
#define SETUP_SUBMIT_ALL (1U << 7)
#define SETUP_COOP_TASKRUN (1U << 8)
#define SETUP_TASKRUN_FLAG (1U << 9)
#define SETUP_SINGLE_ISSUER (1U << 12)
#define SETUP_DEFER_TASKRUN (1U << 13)
#define SETUP_NO_SQARRAY (1U << 16)
#define SETUP_SUPPORTED (SETUP_CQSIZE | SETUP_CLAMP | SETUP_ATTACH_WQ | \
                         SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN | SETUP_TASKRUN_FLAG | \
                         SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN | SETUP_NO_SQARRAY)

#define FEAT_SINGLE_MMAP (1U << 0)
#define FEAT_NODROP (1U << 1)
#define FEAT_SUBMIT_STABLE (1U << 2)
#define FEAT_RW_CUR_POS (1U << 3)
#define FEAT_CUR_PERSONALITY (1U << 4)
#define FEAT_POLL_32BITS (1U << 6)
#define FEAT_EXT_ARG (1U << 8)

#define SQ_NEED_WAKEUP (1U << 0)
#define SQ_CQ_OVERFLOW (1U << 1)

#define SQE_FIXED_FILE (1U << 0)
#define SQE_IO_DRAIN (1U << 1)
#define SQE_IO_LINK (1U << 2)
#define SQE_IO_HARDLINK (1U << 3)
#define SQE_ASYNC (1U << 4)
#define SQE_BUFFER_SELECT (1U << 5)
#define SQE_CQE_SKIP_SUCCESS (1U << 6)
#define SQE_SUPPORTED (SQE_FIXED_FILE | SQE_IO_DRAIN | SQE_IO_LINK | SQE_IO_HARDLINK | \
                       SQE_ASYNC | SQE_CQE_SKIP_SUCCESS)

#define TIMEOUT_ABS (1U << 0)
#define TIMEOUT_UPDATE (1U << 1)
#define TIMEOUT_BOOTTIME (1U << 2)
#define TIMEOUT_REALTIME (1U << 3)
#define TIMEOUT_ETIME_SUCCESS (1U << 5)

#define CANCEL_ALL (1U << 0)
#define CANCEL_FD (1U << 1)
#define CANCEL_ANY (1U << 2)
#define CANCEL_USERDATA (1U << 4)
#define CANCEL_OP (1U << 5)

#define POLL_ADD_MULTI (1U << 0)
#define POLL_UPDATE_EVENTS (1U << 1)
#define POLL_UPDATE_USER_DATA (1U << 2)

#define OFF_SQ_RING 0ULL
#define OFF_CQ_RING 0x8000000ULL
#define OFF_SQES 0x10000000ULL

#define SQ_HEAD 0U
#define SQ_TAIL 4U
#define SQ_MASK 8U
#define SQ_ENTRIES 12U
#define SQ_FLAGS 16U
#define SQ_DROPPED 20U
#define CQ_HEAD 64U
#define CQ_TAIL 68U
#define CQ_MASK 72U
#define CQ_ENTRIES 76U
#define CQ_OVERFLOW 80U
#define CQ_FLAGS 84U
#define CQ_CQES 128U

#define OP_WAITING 0
#define OP_PENDING 1
#define OP_DONE 2

struct ring_sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t ioprio;
    int32_t fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t op_flags;
    uint64_t user_data;
    uint16_t buf_index;
    uint16_t personality;
    int32_t splice_fd_in;
    uint64_t addr3;
    uint64_t pad;
};

struct ring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

struct ring_timespec {
    int64_t sec;
    int64_t nsec;
};

_Static_assert(sizeof(struct ring_sqe) == 64, "io_uring_sqe is 64 bytes");
_Static_assert(sizeof(struct ring_cqe) == 16, "io_uring_cqe is 16 bytes");
_Static_assert(sizeof(struct tunix_io_uring_params) == 120, "io_uring_params is 120 bytes");

struct io_uring_context {
    struct mutex lock;
    struct memfd_object *memory;
    uint8_t **pages;
    uint64_t page_count;
    uint64_t ring_bytes;
    uint64_t sqes_offset;
    uint64_t sqes_bytes;
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t setup_flags;
    uint32_t op_count;
    uint32_t op_limit;
    uint64_t posted;
    struct io_uring_op *head;
    struct io_uring_op *tail;
    struct io_uring_op *link_last;
    uint64_t restart_tid;
    uint32_t restart_submitted;
    uint64_t restart_deadline;
};

static uint64_t round_pow2(uint64_t value) {
    uint64_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

static uint64_t align_to(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void *at(struct io_uring_context *context, uint64_t offset) {
    return context->pages[offset / RING_PAGE] + (offset % RING_PAGE);
}

static uint32_t *field(struct io_uring_context *context, uint32_t offset) {
    return (uint32_t *)at(context, offset);
}

static uint64_t array_offset(const struct io_uring_context *context) {
    return align_to(CQ_CQES + (uint64_t)context->cq_entries * sizeof(struct ring_cqe), 64);
}

static void release_context(struct io_uring_context *context) {
    if (!context) return;
    kfree(context->pages);
    if (context->memory) memfd_destroy(context->memory);
    kfree(context);
}

int io_uring_create(uint32_t entries, struct tunix_io_uring_params *params,
                    struct io_uring_context **out) {
    if (!params || !out) return -EFAULT;
    *out = NULL;
    uint32_t flags = params->flags;
    if (flags & ~SETUP_SUPPORTED) return -EINVAL;
    if ((flags & SETUP_DEFER_TASKRUN) && !(flags & SETUP_SINGLE_ISSUER)) return -EINVAL;
    for (unsigned index = 0; index < 3; index++)
        if (params->resv[index]) return -EINVAL;
    if (!entries) return -EINVAL;
    if (entries > RING_MAX_ENTRIES) {
        if (!(flags & SETUP_CLAMP)) return -EINVAL;
        entries = RING_MAX_ENTRIES;
    }
    uint64_t sq_entries = round_pow2(entries);
    uint64_t cq_entries = sq_entries * 2ULL;
    if (flags & SETUP_CQSIZE) {
        if (!params->cq_entries) return -EINVAL;
        cq_entries = params->cq_entries;
        if (cq_entries > RING_MAX_CQ_ENTRIES) {
            if (!(flags & SETUP_CLAMP)) return -EINVAL;
            cq_entries = RING_MAX_CQ_ENTRIES;
        }
        cq_entries = round_pow2(cq_entries);
        if (cq_entries < sq_entries) return -EINVAL;
    }

    struct io_uring_context *context = kmalloc(sizeof(*context));
    if (!context) return -ENOMEM;
    memset(context, 0, sizeof(*context));
    mutex_init(&context->lock, "io_uring", LOCK_RANK_RING);
    context->sq_entries = (uint32_t)sq_entries;
    context->cq_entries = (uint32_t)cq_entries;
    context->setup_flags = flags;
    context->op_limit = context->cq_entries * 16U;
    uint64_t array = array_offset(context);
    context->ring_bytes = (flags & SETUP_NO_SQARRAY) ? array : array + sq_entries * 4ULL;
    context->sqes_offset = align_to(context->ring_bytes, RING_PAGE);
    context->sqes_bytes = sq_entries * sizeof(struct ring_sqe);
    uint64_t total = context->sqes_offset + align_to(context->sqes_bytes, RING_PAGE);
    context->page_count = total / RING_PAGE;

    context->memory = memfd_create_object();
    context->pages = kmalloc(context->page_count * sizeof(*context->pages));
    if (!context->memory || !context->pages || memfd_truncate(context->memory, total) != 0) {
        release_context(context);
        return -ENOMEM;
    }
    for (uint64_t index = 0; index < context->page_count; index++) {
        uint64_t physical = memfd_page_ensure(context->memory, index);
        if (!physical) {
            release_context(context);
            return -ENOMEM;
        }
        context->pages[index] = (uint8_t *)vmm_phys_to_virt(physical);
    }

    *field(context, SQ_MASK) = context->sq_entries - 1U;
    *field(context, SQ_ENTRIES) = context->sq_entries;
    *field(context, CQ_MASK) = context->cq_entries - 1U;
    *field(context, CQ_ENTRIES) = context->cq_entries;

    params->sq_entries = context->sq_entries;
    params->cq_entries = context->cq_entries;
    params->features = FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_SUBMIT_STABLE |
                       FEAT_RW_CUR_POS | FEAT_CUR_PERSONALITY | FEAT_POLL_32BITS |
                       FEAT_EXT_ARG;
    memset(&params->sq_off, 0, sizeof(params->sq_off));
    memset(&params->cq_off, 0, sizeof(params->cq_off));
    params->sq_off.head = SQ_HEAD;
    params->sq_off.tail = SQ_TAIL;
    params->sq_off.ring_mask = SQ_MASK;
    params->sq_off.ring_entries = SQ_ENTRIES;
    params->sq_off.flags = SQ_FLAGS;
    params->sq_off.dropped = SQ_DROPPED;
    params->sq_off.array = (flags & SETUP_NO_SQARRAY) ? 0U : (uint32_t)array;
    params->cq_off.head = CQ_HEAD;
    params->cq_off.tail = CQ_TAIL;
    params->cq_off.ring_mask = CQ_MASK;
    params->cq_off.ring_entries = CQ_ENTRIES;
    params->cq_off.overflow = CQ_OVERFLOW;
    params->cq_off.cqes = CQ_CQES;
    params->cq_off.flags = CQ_FLAGS;
    *out = context;
    return 0;
}

void io_uring_destroy(struct io_uring_context *context) {
    if (!context) return;
    struct io_uring_op *op = context->head;
    while (op) {
        struct io_uring_op *next = op->next;
        kfree(op);
        op = next;
    }
    release_context(context);
}

struct memfd_object *io_uring_memory(struct io_uring_context *context) {
    return context ? context->memory : NULL;
}

int io_uring_map_offset(struct io_uring_context *context, uint64_t offset,
                        uint64_t length, uint64_t *object_offset) {
    if (!context || !object_offset) return -EINVAL;
    if (offset == OFF_SQ_RING || offset == OFF_CQ_RING) {
        if (length > align_to(context->ring_bytes, RING_PAGE)) return -EINVAL;
        *object_offset = 0;
        return 0;
    }
    if (offset == OFF_SQES) {
        if (length > align_to(context->sqes_bytes, RING_PAGE)) return -EINVAL;
        *object_offset = context->sqes_offset;
        return 0;
    }
    return -EINVAL;
}

void io_uring_lock(struct io_uring_context *context) {
    mutex_lock(&context->lock);
}

void io_uring_unlock(struct io_uring_context *context) {
    mutex_unlock(&context->lock);
}

int io_uring_op_supported(uint32_t opcode) {
    switch (opcode) {
    case IORING_OP_NOP: case IORING_OP_READV: case IORING_OP_WRITEV:
    case IORING_OP_FSYNC: case IORING_OP_POLL_ADD: case IORING_OP_POLL_REMOVE:
    case IORING_OP_SYNC_FILE_RANGE: case IORING_OP_SENDMSG: case IORING_OP_RECVMSG:
    case IORING_OP_TIMEOUT: case IORING_OP_TIMEOUT_REMOVE: case IORING_OP_ACCEPT:
    case IORING_OP_ASYNC_CANCEL: case IORING_OP_LINK_TIMEOUT: case IORING_OP_CONNECT:
    case IORING_OP_FALLOCATE: case IORING_OP_OPENAT: case IORING_OP_CLOSE:
    case IORING_OP_STATX: case IORING_OP_READ: case IORING_OP_WRITE:
    case IORING_OP_FADVISE: case IORING_OP_MADVISE: case IORING_OP_SEND:
    case IORING_OP_RECV: case IORING_OP_EPOLL_CTL: case IORING_OP_SHUTDOWN:
    case IORING_OP_RENAMEAT: case IORING_OP_UNLINKAT: case IORING_OP_MKDIRAT:
    case IORING_OP_SYMLINKAT: case IORING_OP_LINKAT:
        return 1;
    default:
        return 0;
    }
}

static int read_deadline(uint64_t user_time, uint32_t flags, uint64_t *deadline) {
    struct ring_timespec value;
    if (!user_time || copy_from_user(&value, user_time, sizeof(value)) != 0) return -EFAULT;
    if (value.sec < 0 || value.nsec < 0) return -EINVAL;
    uint64_t span = (uint64_t)value.sec > UINT64_MAX / 1000000000ULL ? UINT64_MAX :
                    (uint64_t)value.sec * 1000000000ULL;
    span = UINT64_MAX - span < (uint64_t)value.nsec ? UINT64_MAX : span + (uint64_t)value.nsec;
    uint64_t now = time_uptime_ns();
    if (!(flags & TIMEOUT_ABS)) {
        *deadline = UINT64_MAX - now < span ? UINT64_MAX : now + span;
        return 0;
    }
    if (flags & TIMEOUT_REALTIME) {
        uint64_t real = time_realtime_ns();
        *deadline = span <= real ? now : (UINT64_MAX - now < span - real ? UINT64_MAX :
                                          now + (span - real));
        return 0;
    }
    *deadline = span;
    return 0;
}

static void append(struct io_uring_context *context, struct io_uring_op *op) {
    if (context->tail) context->tail->next = op;
    else context->head = op;
    context->tail = op;
    context->op_count++;
}

static void finish(struct io_uring_op *op, int32_t result) {
    op->state = OP_DONE;
    op->result = result;
}

static void prepare(struct io_uring_context *context, const struct ring_sqe *sqe,
                    struct io_uring_op *op, uint64_t space) {
    op->opcode = sqe->opcode;
    op->sqe_flags = sqe->flags;
    op->fd = sqe->fd;
    op->off = sqe->off;
    op->addr = sqe->addr;
    op->addr3 = sqe->addr3;
    op->len = sqe->len;
    op->op_flags = sqe->op_flags;
    op->user_data = sqe->user_data;
    op->space = space;
    op->state = OP_WAITING;

    if (context->link_last) {
        if (op->opcode == IORING_OP_LINK_TIMEOUT) op->guard = context->link_last;
        else op->after = context->link_last;
    }
    if (op->opcode != IORING_OP_LINK_TIMEOUT)
        context->link_last = (op->sqe_flags & (SQE_IO_LINK | SQE_IO_HARDLINK)) ? op : NULL;

    if (op->sqe_flags & ~SQE_SUPPORTED) {
        finish(op, -EINVAL);
        return;
    }
    if (op->sqe_flags & SQE_FIXED_FILE) {
        finish(op, -EBADF);
        return;
    }
    if (!io_uring_op_supported(op->opcode) || sqe->buf_index || sqe->personality) {
        finish(op, -EINVAL);
        return;
    }
    if (op->opcode == IORING_OP_LINK_TIMEOUT && !op->guard) {
        finish(op, -EINVAL);
        return;
    }
    if (op->opcode == IORING_OP_TIMEOUT || op->opcode == IORING_OP_LINK_TIMEOUT) {
        uint32_t allowed = TIMEOUT_ABS | TIMEOUT_BOOTTIME | TIMEOUT_REALTIME |
                           TIMEOUT_ETIME_SUCCESS;
        if (op->len != 1 || (op->op_flags & ~allowed) ||
            ((op->op_flags & TIMEOUT_BOOTTIME) && (op->op_flags & TIMEOUT_REALTIME))) {
            finish(op, -EINVAL);
            return;
        }
        int status = read_deadline(op->addr, op->op_flags, &op->deadline_ns);
        if (status != 0) {
            finish(op, status);
            return;
        }
        if (op->opcode == IORING_OP_TIMEOUT && op->off)
            op->target = context->posted + op->off;
    }
    if (op->opcode == IORING_OP_TIMEOUT_REMOVE) {
        if (op->op_flags & ~(TIMEOUT_UPDATE | TIMEOUT_ABS | TIMEOUT_BOOTTIME | TIMEOUT_REALTIME)) {
            finish(op, -EINVAL);
            return;
        }
        if (op->op_flags & TIMEOUT_UPDATE) {
            int status = read_deadline(op->off, op->op_flags, &op->deadline_ns);
            if (status != 0) {
                finish(op, status);
                return;
            }
        }
    }
    if (op->opcode == IORING_OP_POLL_ADD &&
        (op->len & (POLL_UPDATE_EVENTS | POLL_UPDATE_USER_DATA))) {
        finish(op, -EINVAL);
        return;
    }
}

int64_t io_uring_submit(struct io_uring_context *context, uint32_t to_submit,
                        uint64_t space) {
    uint32_t head = *field(context, SQ_HEAD);
    uint32_t tail = __atomic_load_n(field(context, SQ_TAIL), __ATOMIC_ACQUIRE);
    uint32_t available = tail - head;
    if (available > context->sq_entries) available = context->sq_entries;
    uint32_t wanted = to_submit < available ? to_submit : available;
    uint32_t mask = context->sq_entries - 1U;
    uint64_t array = array_offset(context);
    int64_t submitted = 0;
    int blocked = 0;
    context->link_last = NULL;

    for (uint32_t step = 0; step < wanted; step++) {
        if (context->op_count >= context->op_limit) {
            blocked = 1;
            break;
        }
        uint32_t index = (context->setup_flags & SETUP_NO_SQARRAY) ? (head & mask) :
            __atomic_load_n((uint32_t *)at(context, array + (uint64_t)(head & mask) * 4ULL),
                            __ATOMIC_RELAXED);
        head++;
        if (index >= context->sq_entries) {
            __atomic_store_n(field(context, SQ_DROPPED), *field(context, SQ_DROPPED) + 1U,
                             __ATOMIC_RELAXED);
            continue;
        }
        struct ring_sqe sqe;
        memcpy(&sqe, at(context, context->sqes_offset + (uint64_t)index * sizeof(sqe)),
               sizeof(sqe));
        struct io_uring_op *op = kmalloc(sizeof(*op));
        if (!op) {
            head--;
            blocked = 1;
            break;
        }
        memset(op, 0, sizeof(*op));
        prepare(context, &sqe, op, space);
        append(context, op);
        submitted++;
    }
    context->link_last = NULL;
    __atomic_store_n(field(context, SQ_HEAD), head, __ATOMIC_RELEASE);
    if (!submitted && blocked) return -EBUSY;
    return submitted;
}

static int older_unfinished(struct io_uring_context *context, struct io_uring_op *op) {
    for (struct io_uring_op *item = context->head; item && item != op; item = item->next)
        if (item->state != OP_DONE) return 1;
    return 0;
}

static void settle_links(struct io_uring_op *done) {
    int failed = done->result < 0 && !(done->sqe_flags & SQE_IO_HARDLINK);
    if (done->opcode == IORING_OP_LINK_TIMEOUT) return;
    for (struct io_uring_op *item = done->next; item; item = item->next) {
        if (item->after == done) {
            item->after = NULL;
            if (failed || done->cancelled) item->cancelled = 1;
        }
        if (item->guard == done) {
            item->guard = NULL;
            if (item->state != OP_DONE) finish(item, -ECANCELED);
        }
    }
}

static void cancel(struct io_uring_op *op) {
    if (op->state == OP_DONE) return;
    op->cancelled = 1;
    finish(op, -ECANCELED);
}

static int cancel_matches(const struct io_uring_op *request, const struct io_uring_op *item) {
    if (item == request || item->state == OP_DONE) return 0;
    if (request->op_flags & CANCEL_ANY) return 1;
    if (request->op_flags & (CANCEL_FD | CANCEL_OP)) {
        if ((request->op_flags & CANCEL_FD) && item->fd != request->fd) return 0;
        if ((request->op_flags & CANCEL_OP) && item->opcode != request->len) return 0;
        if ((request->op_flags & CANCEL_USERDATA) && item->user_data != request->addr) return 0;
        return 1;
    }
    return item->user_data == request->addr;
}

static int32_t run_cancel(struct io_uring_context *context, struct io_uring_op *request) {
    int32_t count = 0;
    for (struct io_uring_op *item = context->head; item; item = item->next) {
        if (!cancel_matches(request, item)) continue;
        cancel(item);
        count++;
        if (!(request->op_flags & (CANCEL_ALL | CANCEL_ANY))) break;
    }
    if (!count) return -ENOENT;
    return (request->op_flags & (CANCEL_ALL | CANCEL_ANY)) ? count : 0;
}

static int32_t run_timeout_remove(struct io_uring_context *context, struct io_uring_op *request) {
    for (struct io_uring_op *item = context->head; item; item = item->next) {
        if (item == request || item->state == OP_DONE) continue;
        if (item->opcode != IORING_OP_TIMEOUT || item->user_data != request->addr) continue;
        if (request->op_flags & TIMEOUT_UPDATE) {
            item->deadline_ns = request->deadline_ns;
            return 0;
        }
        cancel(item);
        return 0;
    }
    return -ENOENT;
}

static int32_t run_poll_remove(struct io_uring_context *context, struct io_uring_op *request) {
    for (struct io_uring_op *item = context->head; item; item = item->next) {
        if (item == request || item->state == OP_DONE) continue;
        if (item->opcode != IORING_OP_POLL_ADD || item->user_data != request->addr) continue;
        cancel(item);
        return 0;
    }
    return -ENOENT;
}

static int step(struct io_uring_context *context, struct io_uring_op *op,
                io_uring_executor execute, uint64_t space, uint64_t *now) {
    if (op->state == OP_DONE || op->after) return 0;
    if (op->cancelled) {
        finish(op, -ECANCELED);
        return 1;
    }
    if ((op->sqe_flags & SQE_IO_DRAIN) && older_unfinished(context, op)) return 0;
    switch (op->opcode) {
    case IORING_OP_NOP:
        finish(op, 0);
        return 1;
    case IORING_OP_TIMEOUT:
        if (op->target && context->posted >= op->target) {
            finish(op, 0);
            return 1;
        }
        if (!*now) *now = time_uptime_ns();
        if (*now >= op->deadline_ns) {
            finish(op, -ETIME);
            return 1;
        }
        op->state = OP_PENDING;
        return 0;
    case IORING_OP_LINK_TIMEOUT:
        if (op->guard && op->guard->after) return 0;
        if (!*now) *now = time_uptime_ns();
        if (*now >= op->deadline_ns) {
            if (op->guard) cancel(op->guard);
            finish(op, -ETIME);
            return 1;
        }
        op->state = OP_PENDING;
        return 0;
    case IORING_OP_TIMEOUT_REMOVE:
        finish(op, run_timeout_remove(context, op));
        return 1;
    case IORING_OP_ASYNC_CANCEL:
        finish(op, run_cancel(context, op));
        return 1;
    case IORING_OP_POLL_REMOVE:
        finish(op, run_poll_remove(context, op));
        return 1;
    default:
        break;
    }
    if (op->space != space) return 0;
    int64_t result = execute(op);
    if (result == IO_URING_PENDING) {
        op->state = OP_PENDING;
        return 0;
    }
    if (result > INT32_MAX) result = INT32_MAX;
    finish(op, (int32_t)result);
    return 1;
}

static void post(struct io_uring_context *context) {
    uint32_t head = __atomic_load_n(field(context, CQ_HEAD), __ATOMIC_ACQUIRE);
    uint32_t tail = *field(context, CQ_TAIL);
    uint32_t mask = context->cq_entries - 1U;
    int overflow = 0;
    struct io_uring_op **link = &context->head;
    struct io_uring_op *previous = NULL;
    while (*link) {
        struct io_uring_op *op = *link;
        if (op->state != OP_DONE || !op->settled) {
            previous = op;
            link = &op->next;
            continue;
        }
        int skip = (op->sqe_flags & SQE_CQE_SKIP_SUCCESS) && op->result >= 0;
        if (!skip) {
            if (tail - head >= context->cq_entries) {
                overflow = 1;
                break;
            }
            struct ring_cqe *cqe = (struct ring_cqe *)at(context,
                CQ_CQES + (uint64_t)(tail & mask) * sizeof(struct ring_cqe));
            cqe->user_data = op->user_data;
            cqe->res = op->result;
            cqe->flags = 0;
            tail++;
        }
        if (op->opcode != IORING_OP_TIMEOUT) context->posted++;
        *link = op->next;
        if (context->tail == op) context->tail = previous;
        context->op_count--;
        kfree(op);
    }
    __atomic_store_n(field(context, CQ_TAIL), tail, __ATOMIC_RELEASE);
    uint32_t flags = __atomic_load_n(field(context, SQ_FLAGS), __ATOMIC_RELAXED);
    flags = overflow ? (flags | SQ_CQ_OVERFLOW) : (flags & ~SQ_CQ_OVERFLOW);
    __atomic_store_n(field(context, SQ_FLAGS), flags, __ATOMIC_RELEASE);
}

void io_uring_run(struct io_uring_context *context, io_uring_executor execute,
                  uint64_t space) {
    for (unsigned round = 0; round < 8; round++) {
        uint64_t now = 0;
        int progressed = 0;
        for (struct io_uring_op *op = context->head; op; op = op->next)
            progressed |= step(context, op, execute, space, &now);
        for (struct io_uring_op *op = context->head; op; op = op->next) {
            if (op->state != OP_DONE || op->settled) continue;
            op->settled = 1;
            settle_links(op);
            progressed = 1;
        }
        uint64_t before = context->posted;
        post(context);
        if (!progressed && context->posted == before) break;
    }
}

uint32_t io_uring_completions_ready(struct io_uring_context *context) {
    if (!context) return 0;
    uint32_t tail = __atomic_load_n(field(context, CQ_TAIL), __ATOMIC_ACQUIRE);
    uint32_t head = __atomic_load_n(field(context, CQ_HEAD), __ATOMIC_ACQUIRE);
    return tail - head;
}

uint64_t io_uring_watch(struct io_uring_context *context, io_uring_watcher watch,
                        uint64_t space) {
    uint64_t deadline = UINT64_MAX;
    for (struct io_uring_op *op = context->head; op; op = op->next) {
        if (op->state != OP_PENDING) continue;
        if (op->opcode == IORING_OP_TIMEOUT || op->opcode == IORING_OP_LINK_TIMEOUT) {
            if (op->deadline_ns < deadline) deadline = op->deadline_ns;
            continue;
        }
        if (op->watch && op->space == space && watch) watch(op->fd, op->watch);
    }
    return deadline;
}

void io_uring_note_restart(struct io_uring_context *context, uint64_t tid,
                           uint32_t submitted, uint64_t wait_deadline) {
    context->restart_tid = tid;
    context->restart_submitted = submitted;
    context->restart_deadline = wait_deadline;
}

uint32_t io_uring_take_restart(struct io_uring_context *context, uint64_t tid,
                               uint64_t *wait_deadline) {
    if (context->restart_tid != tid) return 0;
    uint32_t submitted = context->restart_submitted;
    if (wait_deadline) *wait_deadline = context->restart_deadline;
    context->restart_tid = 0;
    context->restart_submitted = 0;
    context->restart_deadline = 0;
    return submitted;
}

uint32_t io_uring_cq_entries(struct io_uring_context *context) {
    return context ? context->cq_entries : 0;
}

uint32_t io_uring_poll(struct io_uring_context *context, uint32_t requested) {
    const uint32_t pollin = 0x001U;
    const uint32_t pollout = 0x004U;
    if (!context) return 0x008U;
    uint32_t events = 0;
    uint32_t flags = __atomic_load_n(field(context, SQ_FLAGS), __ATOMIC_ACQUIRE);
    if (io_uring_completions_ready(context) || (flags & SQ_CQ_OVERFLOW)) events |= pollin;
    uint32_t sq_tail = __atomic_load_n(field(context, SQ_TAIL), __ATOMIC_ACQUIRE);
    uint32_t sq_head = __atomic_load_n(field(context, SQ_HEAD), __ATOMIC_ACQUIRE);
    if (sq_tail - sq_head < context->sq_entries) events |= pollout;
    return events & requested;
}
