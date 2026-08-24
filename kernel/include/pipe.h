#ifndef TUNIX_PIPE_H
#define TUNIX_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include "spinlock.h"

#define PIPE_CAPACITY 4096

struct file;

struct pipe_buffer {
    uint8_t data[PIPE_CAPACITY];
    size_t read_pos;
    size_t write_pos;
    size_t count;
    int readers;
    int writers;
    /* Sleep channels. Only the addresses matter; the values are never read.
       Readers wait for data, writers wait for space. */
    char data_wait;
    char space_wait;
    /* Taken by shared-mode readers and writers, which are the only things that
       can be inside this pipe at the same time. Two processes on different
       pipes take different locks and never meet: that is where the parallelism
       comes from. */
    spinlock_t lock;
};

int pipe_create(struct file **read_end, struct file **write_end);
int64_t pipe_read(struct pipe_buffer *pipe, size_t size, void *buffer);
int64_t pipe_write(struct pipe_buffer *pipe, size_t size, const void *buffer);
/* Drop a reader or writer, freeing the buffer once both sides are gone. */
void pipe_release(struct pipe_buffer *pipe, int write_end);

#endif
