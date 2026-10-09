#ifndef TUNIX_IO_URING_H
#define TUNIX_IO_URING_H

#include <stddef.h>
#include <stdint.h>

struct io_uring_context;
struct memfd_object;

struct tunix_io_sqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t array;
    uint32_t resv1;
    uint64_t user_addr;
};

struct tunix_io_cqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t overflow;
    uint32_t cqes;
    uint32_t flags;
    uint32_t resv1;
    uint64_t user_addr;
};

struct tunix_io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct tunix_io_sqring_offsets sq_off;
    struct tunix_io_cqring_offsets cq_off;
};

#define IORING_OP_NOP             0
#define IORING_OP_READV           1
#define IORING_OP_WRITEV          2
#define IORING_OP_FSYNC           3
#define IORING_OP_POLL_ADD        6
#define IORING_OP_POLL_REMOVE     7
#define IORING_OP_SYNC_FILE_RANGE 8
#define IORING_OP_SENDMSG         9
#define IORING_OP_RECVMSG         10
#define IORING_OP_TIMEOUT         11
#define IORING_OP_TIMEOUT_REMOVE  12
#define IORING_OP_ACCEPT          13
#define IORING_OP_ASYNC_CANCEL    14
#define IORING_OP_LINK_TIMEOUT    15
#define IORING_OP_CONNECT         16
#define IORING_OP_FALLOCATE       17
#define IORING_OP_OPENAT          18
#define IORING_OP_CLOSE           19
#define IORING_OP_STATX           21
#define IORING_OP_READ            22
#define IORING_OP_WRITE           23
#define IORING_OP_FADVISE         24
#define IORING_OP_MADVISE         25
#define IORING_OP_SEND            26
#define IORING_OP_RECV            27
#define IORING_OP_EPOLL_CTL       29
#define IORING_OP_SHUTDOWN        34
#define IORING_OP_RENAMEAT        35
#define IORING_OP_UNLINKAT        36
#define IORING_OP_MKDIRAT         37
#define IORING_OP_SYMLINKAT       38
#define IORING_OP_LINKAT          39
#define IORING_OP_LAST            58

#define IO_URING_PENDING INT64_MIN

struct io_uring_op {
    struct io_uring_op *next;
    struct io_uring_op *after;
    struct io_uring_op *guard;
    uint64_t user_data;
    uint64_t off;
    uint64_t addr;
    uint64_t addr3;
    uint64_t space;
    uint64_t deadline_ns;
    uint64_t target;
    int32_t fd;
    uint32_t len;
    uint32_t op_flags;
    uint32_t watch;
    uint32_t attempts;
    uint8_t opcode;
    uint8_t sqe_flags;
    uint8_t state;
    uint8_t cancelled;
    uint8_t settled;
    int32_t result;
};

typedef int64_t (*io_uring_executor)(struct io_uring_op *op);
typedef void (*io_uring_watcher)(int fd, uint32_t events);

int io_uring_create(uint32_t entries, struct tunix_io_uring_params *params,
                    struct io_uring_context **out);
void io_uring_destroy(struct io_uring_context *context);
struct memfd_object *io_uring_memory(struct io_uring_context *context);
int io_uring_map_offset(struct io_uring_context *context, uint64_t offset, uint64_t length,
                        uint64_t *object_offset);

void io_uring_lock(struct io_uring_context *context);
void io_uring_unlock(struct io_uring_context *context);
int64_t io_uring_submit(struct io_uring_context *context, uint32_t to_submit, uint64_t space);
void io_uring_run(struct io_uring_context *context, io_uring_executor execute, uint64_t space);
uint32_t io_uring_completions_ready(struct io_uring_context *context);
uint64_t io_uring_watch(struct io_uring_context *context, io_uring_watcher watch, uint64_t space);
void io_uring_note_restart(struct io_uring_context *context, uint64_t tid, uint32_t submitted,
                           uint64_t wait_deadline);
uint32_t io_uring_take_restart(struct io_uring_context *context, uint64_t tid,
                               uint64_t *wait_deadline);
uint32_t io_uring_cq_entries(struct io_uring_context *context);
uint32_t io_uring_poll(struct io_uring_context *context, uint32_t requested);
int io_uring_op_supported(uint32_t opcode);

#endif
