#ifndef TUNIX_PIPE_H
#define TUNIX_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include "spinlock.h"

#define PIPE_CAPACITY 65536
_Static_assert((PIPE_CAPACITY & (PIPE_CAPACITY - 1)) == 0, "pipe capacity must be a power of two");

struct file;

struct pipe_buffer {
    uint8_t data[PIPE_CAPACITY];
    size_t read_pos;
    size_t write_pos;
    size_t count;
    int readers;
    int writers;
    int named;
    char data_wait;
    char space_wait;
    spinlock_t lock;
};

int pipe_create(struct file **read_end, struct file **write_end);
struct pipe_buffer *pipe_buffer_create_named(void);
void pipe_buffer_destroy(struct pipe_buffer *pipe);
int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer);
int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer);
void pipe_release(struct pipe_buffer *pipe, int write_end);

#endif
