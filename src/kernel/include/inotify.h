#ifndef TUNIX_INOTIFY_H
#define TUNIX_INOTIFY_H

#include <stddef.h>
#include <stdint.h>

#define TUNIX_IN_MODIFY      0x00000002U
#define TUNIX_IN_ATTRIB      0x00000004U
#define TUNIX_IN_MOVED_FROM  0x00000040U
#define TUNIX_IN_MOVED_TO    0x00000080U
#define TUNIX_IN_CREATE      0x00000100U
#define TUNIX_IN_DELETE      0x00000200U
#define TUNIX_IN_DELETE_SELF 0x00000400U
#define TUNIX_IN_MOVE_SELF   0x00000800U

struct inotify_context;
struct vfs_node;

struct inotify_context *inotify_create(void);
void inotify_destroy(struct inotify_context *context);
int inotify_add_watch(struct inotify_context *context, struct vfs_node *node,
                      uint32_t mask);
int inotify_remove_watch(struct inotify_context *context, int descriptor);
int64_t inotify_read(struct inotify_context *context, size_t size, void *buffer);
int inotify_read_ready(struct inotify_context *context);
void inotify_notify(struct vfs_node *node, uint32_t mask, const char *name,
                    uint32_t cookie);
void inotify_invalidate(struct vfs_node *node);
uint32_t inotify_next_cookie(void);

#endif
