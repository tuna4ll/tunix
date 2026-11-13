#ifndef TUNIX_POSIX_LOCK_H
#define TUNIX_POSIX_LOCK_H

#include <stdint.h>

#define POSIX_LOCK_READ   0
#define POSIX_LOCK_WRITE  1
#define POSIX_LOCK_UNLOCK 2

#define POSIX_LOCK_END UINT64_MAX

struct vfs_node;

struct posix_lock_request {
    int type;
    uint64_t start;
    uint64_t end;
    uint64_t owner;
    int32_t pid;
};

int posix_lock_test(struct vfs_node *node, struct posix_lock_request *request);
int posix_lock_set(struct vfs_node *node, const struct posix_lock_request *request, int wait);
void posix_lock_release_owner(struct vfs_node *node, uint64_t owner);

#endif
