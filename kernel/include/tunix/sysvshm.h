#ifndef TUNIX_SYSVSHM_H
#define TUNIX_SYSVSHM_H

#include <stdint.h>

struct file;

#define IPC_PRIVATE 0

#define IPC_CREAT 01000
#define IPC_EXCL  02000

#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
#define IPC_64   0x100

#define SHM_RDONLY 010000
#define SHM_RND    020000

#define SHM_DEST 01000

struct shm_id_ds {
    int32_t key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    uint32_t seq;
    uint64_t pad1;
    uint64_t pad2;
    uint64_t segsz;
    int64_t atime;
    int64_t dtime;
    int64_t ctime;
    int32_t cpid;
    int32_t lpid;
    uint64_t nattch;
    uint64_t unused4;
    uint64_t unused5;
};

int sysvshm_get(int32_t key, uint64_t size, int flags, uint32_t pid);
void sysvshm_reap(void);
struct file *sysvshm_acquire(int id, uint64_t *size_out);
int sysvshm_stat(int id, struct shm_id_ds *out);
int sysvshm_set(int id, uint32_t mode, uint32_t uid, uint32_t gid);
int sysvshm_remove(int id);
void sysvshm_touch(int id, uint32_t pid, int attaching);

#endif
