#include <stddef.h>
#include <stdint.h>
#include "include/file.h"
#include "include/heap.h"
#include "include/kstring.h"
#include "include/pipe.h"

#define EAGAIN 11

int pipe_create(struct file **read_end, struct file **write_end) {
    if (!read_end || !write_end) return -1;
    struct pipe_buffer *pipe = (struct pipe_buffer *)kmalloc(sizeof(*pipe));
    memset(pipe, 0, sizeof(*pipe));
    *read_end = file_create_pipe_end(pipe, 0);
    *write_end = file_create_pipe_end(pipe, 1);
    if (!*read_end || !*write_end) return -1;
    return 0;
}

int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer) {
    if (!pipe || !buffer) return -1;
    if (pipe->count == 0) return pipe->writers == 0 ? 0 : -EAGAIN;
    uint8_t *out = (uint8_t *)buffer;
    size_t amount = size < pipe->count ? size : pipe->count;
    for (size_t i = 0; i < amount; i++) {
        out[i] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count -= amount;
    return (int64_t)amount;
}

int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer) {
    if (!pipe || !buffer) return -1;
    size_t available = PIPE_CAPACITY - pipe->count;
    if (available == 0) return -EAGAIN;
    const uint8_t *in = (const uint8_t *)buffer;
    size_t amount = size < available ? size : available;
    for (size_t i = 0; i < amount; i++) {
        pipe->data[pipe->write_pos] = in[i];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count += amount;
    return (int64_t)amount;
}
