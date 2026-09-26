#include <stddef.h>
#include <stdint.h>
#include "../include/file.h"
#include "../include/heap.h"
#include "../include/kstring.h"
#include "../include/klock.h"

static int64_t pipe_read_locked(struct pipe_buffer *pipe, size_t size, void *buffer);
static int64_t pipe_write_locked(struct pipe_buffer *pipe, size_t size, const void *buffer);
#include "../include/pipe.h"
#include "../include/process.h"

#define EAGAIN 11

int pipe_buffer_init(struct pipe_buffer *pipe, size_t capacity) {
    memset(pipe, 0, sizeof(*pipe));
    pipe->data = (uint8_t *)kmalloc(capacity);
    if (!pipe->data) return -1;
    pipe->capacity = capacity;
    return 0;
}

void pipe_buffer_fini(struct pipe_buffer *pipe) {
    if (!pipe) return;
    kfree(pipe->data);
    pipe->data = NULL;
    pipe->capacity = 0;
}

int pipe_resize(struct pipe_buffer *pipe, size_t capacity) {
    if (!pipe || capacity < pipe->count) return -1;
    if (capacity == pipe->capacity) return 0;
    uint8_t *data = (uint8_t *)kmalloc(capacity);
    if (!data) return -1;
    size_t first = pipe->capacity - pipe->read_pos;
    if (first > pipe->count) first = pipe->count;
    memcpy(data, pipe->data + pipe->read_pos, first);
    if (pipe->count > first) memcpy(data + first, pipe->data, pipe->count - first);
    kfree(pipe->data);
    pipe->data = data;
    pipe->capacity = capacity;
    pipe->read_pos = 0;
    pipe->write_pos = pipe->count & (capacity - 1U);
    return 0;
}

int pipe_create(struct file **read_end, struct file **write_end) {
    if (!read_end || !write_end) return -1;
    struct pipe_buffer *pipe = (struct pipe_buffer *)kmalloc(sizeof(*pipe));
    if (!pipe) return -1;
    if (pipe_buffer_init(pipe, PIPE_CAPACITY) != 0) {
        kfree(pipe);
        return -1;
    }
    *read_end = file_create_pipe_end(pipe, 0);
    *write_end = file_create_pipe_end(pipe, 1);
    if (!*read_end || !*write_end) return -1;
    return 0;
}

struct pipe_buffer *pipe_buffer_create_named(void) {
    struct pipe_buffer *pipe = (struct pipe_buffer *)kmalloc(sizeof(*pipe));
    if (!pipe) return NULL;
    if (pipe_buffer_init(pipe, PIPE_CAPACITY) != 0) {
        kfree(pipe);
        return NULL;
    }
    pipe->named = 1;
    return pipe;
}

void pipe_buffer_destroy(struct pipe_buffer *pipe) {
    if (!pipe) return;
    pipe_buffer_fini(pipe);
    kfree(pipe);
}

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
    size_t first = pipe->capacity - pipe->read_pos;
    if (first > amount) first = amount;
    memcpy(out, pipe->data + pipe->read_pos, first);
    if (amount > first) memcpy(out + first, pipe->data, amount - first);
    pipe->read_pos = (pipe->read_pos + amount) & (pipe->capacity - 1U);
    pipe->count -= amount;

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
    size_t available = pipe->capacity - pipe->count;
    if (available == 0) return -EAGAIN;
    const uint8_t *in = (const uint8_t *)buffer;
    size_t amount = size < available ? size : available;
    size_t first = pipe->capacity - pipe->write_pos;
    if (first > amount) first = amount;
    memcpy(pipe->data + pipe->write_pos, in, first);
    if (amount > first) memcpy(pipe->data, in + first, amount - first);
    pipe->write_pos = (pipe->write_pos + amount) & (pipe->capacity - 1U);
    pipe->count += amount;

    process_wake_all(&pipe->data_wait);
    return (int64_t)amount;
}

void pipe_release(struct pipe_buffer *pipe, int write_end) {
    if (!pipe) return;
    if (write_end) {
        if (pipe->writers > 0) pipe->writers--;

        if (pipe->writers == 0) process_wake_all(&pipe->data_wait);
    } else {
        if (pipe->readers > 0) pipe->readers--;
        if (pipe->readers == 0) process_wake_all(&pipe->space_wait);
    }
    if (!pipe->named && pipe->readers == 0 && pipe->writers == 0) pipe_buffer_destroy(pipe);
}
