#ifndef TUNIX_PIPE_H
#define TUNIX_PIPE_H

#include <stddef.h>
#include <stdint.h>

#define PIPE_CAPACITY 4096

struct file;

struct pipe_buffer {
    uint8_t data[PIPE_CAPACITY];
    size_t read_pos;
    size_t write_pos;
    size_t count;
    int readers;
    int writers;
};

int pipe_create(struct file **read_end, struct file **write_end);
int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer);
int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer);

#endif
