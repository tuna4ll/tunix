#ifndef TUNIX_NAMESPACE_H
#define TUNIX_NAMESPACE_H

#include <stdint.h>

#define CLONE_NEWCGROUP 0x02000000ULL
#define CLONE_NEWUTS    0x04000000ULL
#define CLONE_NEWIPC    0x08000000ULL
#define CLONE_NEWUSER   0x10000000ULL
#define CLONE_NEWPID    0x20000000ULL
#define CLONE_NEWNET    0x40000000ULL
#define CLONE_NEWNS     0x00020000ULL

#define CLONE_NAMESPACE_FLAGS \
    (CLONE_NEWUSER | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS | \
     CLONE_NEWCGROUP)

#define UID_MAP_MAX       5
#define USER_NS_MAX_DEPTH 32
#define UTS_FIELD_MAX     65

struct uid_extent {
    uint32_t first;
    uint32_t lower_first;
    uint32_t count;
};

struct uid_map {
    uint32_t extents;
    struct uid_extent extent[UID_MAP_MAX];
};

struct user_namespace {
    struct user_namespace *parent;
    uint32_t refs;
    uint32_t level;
    uint32_t owner_uid;
    uint32_t owner_gid;
    int uid_map_set;
    int gid_map_set;
    int setgroups_allowed;
    struct uid_map uid;
    struct uid_map gid;
};

struct uts_namespace {
    struct uts_namespace *parent;
    uint32_t refs;
    char nodename[UTS_FIELD_MAX];
    char domainname[UTS_FIELD_MAX];
};

struct pid_namespace {
    struct pid_namespace *parent;
    uint32_t refs;
    uint32_t level;
    uint64_t last_pid;
};

struct process;

int uts_ns_is_root(const struct uts_namespace *ns);
uint64_t pid_ns_alloc(struct pid_namespace *ns);
void namespace_assign_pid(struct process *child, const struct process *parent, int is_thread);
void namespace_inherit(struct process *child, const struct process *parent);
void namespace_release(struct process *process);
int64_t namespace_unshare(uint64_t flags);
int64_t namespace_apply_clone(struct process *child, uint64_t flags);

int user_ns_capable(void);
uint32_t user_ns_map_uid(const struct user_namespace *ns, uint32_t kuid);
uint32_t user_ns_map_gid(const struct user_namespace *ns, uint32_t kgid);

int64_t user_ns_write_uid_map(int is_gid, const char *text, uint64_t length);
int64_t user_ns_write_setgroups(const char *text, uint64_t length);
int64_t user_ns_read_uid_map(int is_gid, char *out, uint64_t capacity);

struct uts_namespace *uts_ns_current(void);

#endif
