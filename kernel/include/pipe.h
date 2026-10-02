#ifndef TUNIX_PIPE_H
#define TUNIX_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include "spinlock.h"

#define PIPE_CAPACITY 65536
#define PIPE_MAX_CAPACITY (1024U * 1024U)
#define PIPE_ROOT_MAX_CAPACITY (64U * 1024U * 1024U)

struct file;

struct pipe_buffer {
    uint8_t *data;
    size_t capacity;
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

int pipe_buffer_init(struct pipe_buffer *pipe, size_t capacity);
void pipe_buffer_fini(struct pipe_buffer *pipe);
int pipe_resize(struct pipe_buffer *pipe, size_t capacity);
int pipe_create(struct file **read_end, struct file **write_end);
struct pipe_buffer *pipe_buffer_create_named(void);
void pipe_buffer_destroy(struct pipe_buffer *pipe);
int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer);
int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer);
void pipe_release(struct pipe_buffer *pipe, int write_end);
void pipe_attach_end(struct pipe_buffer *pipe, int write_end);

#endif
