#ifndef TUNIX_SYSVSHM_H
#define TUNIX_SYSVSHM_H

#include <stdint.h>

/*
 * System V shared memory, the object behind shmget(2).
 *
 * It exists for MIT-SHM. The X server probes shmget() at startup and disables
 * the extension when it fails, which sends every client frame down the display
 * socket as an XPutImage instead: measured here at 19.6 fps for a 640x400 blit
 * against 3668 fps for the same blit with no X at all.
 *
 * A segment is a memfd_object wrapped in a struct file, so the address-space
 * map's existing file reference counting is what keeps the pages alive: each
 * vm_area attached to a segment holds a reference, fork copies them, exit drops
 * them.
 *
 * IPC_RMID does not free the slot while anything is still attached. That is not
 * a nicety: cairo's shared-memory pool calls shmat(), then IPC_RMID, and only
 * then asks the X server to attach the same id -- so an id that stopped
 * resolving at IPC_RMID makes the server's shmat() fail and MIT-SHM comes apart
 * with a BadAccess on ShmAttach. A removed segment is instead marked destroyed:
 * its key stops matching, the id keeps resolving, and the slot is released once
 * the last attachment is gone.
 */

struct file;

#define IPC_PRIVATE 0

#define IPC_CREAT  01000
#define IPC_EXCL   02000

#define IPC_RMID 0
#define IPC_SET  1
#define IPC_STAT 2
/* glibc and musl or the caller add this to select the 64-bit structures; the
   only layout Tunix has is the 64-bit one, so it is masked off and ignored. */
#define IPC_64   0x100

#define SHM_RDONLY 010000
#define SHM_RND    020000

/* Reported in the mode of a segment that IPC_RMID has marked, as Linux does. */
#define SHM_DEST   01000

/* struct shmid64_ds, the layout shmctl(IPC_STAT) writes to user space. */
struct shm_id_ds {
    int32_t  key;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    uint32_t seq;
    uint64_t pad1;
    uint64_t pad2;
    uint64_t segsz;
    int64_t  atime;
    int64_t  dtime;
    int64_t  ctime;
    int32_t  cpid;
    int32_t  lpid;
    uint64_t nattch;
    uint64_t unused4;
    uint64_t unused5;
};

/* Returns a segment id, or a negative errno. */
int sysvshm_get(int32_t key, uint64_t size, int flags, uint32_t pid);
/* Release any destroyed segment whose last attachment has gone away. Cheap
   enough to call from every shm syscall, which is what keeps a process that
   exits while attached from holding its slot forever. */
void sysvshm_reap(void);
/* The file backing `id` with a reference taken, plus the segment size.
   The caller owns the reference and must file_unref() it. */
struct file *sysvshm_acquire(int id, uint64_t *size_out);
int sysvshm_stat(int id, struct shm_id_ds *out);
int sysvshm_set(int id, uint32_t mode, uint32_t uid, uint32_t gid);
int sysvshm_remove(int id);
/* Record which process last attached or detached, for IPC_STAT. */
void sysvshm_touch(int id, uint32_t pid, int attaching);

#endif
