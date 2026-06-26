#ifndef TUNIX_PTY_H
#define TUNIX_PTY_H

#include <stddef.h>
#include <stdint.h>

struct file;
struct pty_pair;
struct vfs_node;

#define PTY_MAX_PAIRS 8

void pty_init(void);
struct file *pty_open_master(struct vfs_node *node, uint32_t flags);
struct file *pty_open_slave(struct vfs_node *node, uint32_t flags);
struct file *pty_open_controlling(struct vfs_node *node, uint32_t flags);
void pty_ref_endpoint(struct pty_pair *pty, int master);
void pty_close_endpoint(struct pty_pair *pty, int master);
int64_t pty_read(struct pty_pair *pty, int master, size_t size, void *buffer);
int64_t pty_write(struct pty_pair *pty, int master, size_t size, const void *buffer);
int pty_read_ready(struct pty_pair *pty, int master);
int pty_write_ready(struct pty_pair *pty, int master);
int64_t pty_ioctl(struct pty_pair *pty, int master, unsigned long request,
                  uint64_t user_argument);

#endif
