#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/heap.h"
#include "include/kstring.h"
#include "include/klock.h"

static int64_t pipe_read_locked(struct pipe_buffer *pipe, size_t size, void *buffer);
static int64_t pipe_write_locked(struct pipe_buffer *pipe, size_t size, const void *buffer);
#include "include/pipe.h"
#include "include/process.h"

#define EAGAIN 11

int pipe_create(struct file **read_end, struct file **write_end) {
    if (!read_end || !write_end) return -1;
    struct pipe_buffer *pipe = (struct pipe_buffer *)kmalloc(sizeof(*pipe));
    if (!pipe) return -1;
    memset(pipe, 0, sizeof(*pipe));
    *read_end = file_create_pipe_end(pipe, 0);
    *write_end = file_create_pipe_end(pipe, 1);
    if (!*read_end || !*write_end) return -1;
    return 0;
}

/*
 * Held only in shared mode, because that is the only mode in which a second
 * processor can be inside this pipe. An exclusive holder is alone and pays
 * nothing. See include/oplock.h.
 */
static void pipe_enter(struct pipe_buffer *pipe) {
    if (kernel_lock_shared_here()) spinlock_acquire(&pipe->lock);
}

static void pipe_leave(struct pipe_buffer *pipe) {
    if (kernel_lock_shared_here()) spinlock_release(&pipe->lock);
}

int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer) {
    if (!pipe || !buffer) return -1;
    pipe_enter(pipe);
    int64_t moved = pipe_read_locked(pipe, size, buffer);
    pipe_leave(pipe);
    return moved;
}

static int64_t pipe_read_locked(struct pipe_buffer *pipe, size_t size, void *buffer) {
    if (pipe->count == 0) return pipe->writers == 0 ? 0 : -EAGAIN;
    uint8_t *out = (uint8_t *)buffer;
    size_t amount = size < pipe->count ? size : pipe->count;
    for (size_t i = 0; i < amount; i++) {
        out[i] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count -= amount;
    /* Space freed up; anyone blocked in write can make progress. */
    process_wake_all(&pipe->space_wait);
    return (int64_t)amount;
}

int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer) {
    if (!pipe || !buffer) return -1;
    pipe_enter(pipe);
    int64_t moved = pipe_write_locked(pipe, size, buffer);
    pipe_leave(pipe);
    return moved;
}

static int64_t pipe_write_locked(struct pipe_buffer *pipe, size_t size, const void *buffer) {
    size_t available = PIPE_CAPACITY - pipe->count;
    if (available == 0) return -EAGAIN;
    const uint8_t *in = (const uint8_t *)buffer;
    size_t amount = size < available ? size : available;
    for (size_t i = 0; i < amount; i++) {
        pipe->data[pipe->write_pos] = in[i];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count += amount;
    /* Data arrived; anyone blocked in read can make progress. */
    process_wake_all(&pipe->data_wait);
    return (int64_t)amount;
}

void pipe_release(struct pipe_buffer *pipe, int write_end) {
    if (!pipe) return;
    if (write_end) {
        if (pipe->writers > 0) pipe->writers--;
        /* The last writer leaving turns a blocking read into EOF, and the last
           reader leaving turns a blocking write into EPIPE, so both closes have
           to wake the other side or it would sleep forever. */
        if (pipe->writers == 0) process_wake_all(&pipe->data_wait);
    } else {
        if (pipe->readers > 0) pipe->readers--;
        if (pipe->readers == 0) process_wake_all(&pipe->space_wait);
    }
    if (pipe->readers == 0 && pipe->writers == 0) kfree(pipe);
}
