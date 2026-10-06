typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short u16;

#include "tunix_syscall.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_FORK 57
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_MKDIR 83
#define SYS_CHMOD 90
#define SYS_SETRESUID 117
#define SYS_MOUNT 165
#define SYS_SETXATTR 188
#define SYS_GETXATTR 191
#define SYS_LISTXATTR 194
#define SYS_REMOVEXATTR 197
#define SYS_EXIT_GROUP 231
#define SYS_NEWFSTATAT 262
#define O_RDWR 2
#define O_CREAT 0100
#define AT_FDCWD_TEST -100
#define EPERM 1
#define EACCES 13
#define EINVAL 22
#define ENODATA 61
#define EOPNOTSUPP 95

#define ACL_NAME "system.posix_acl_access"
#define FILE_PATH "/tmp/acl/device"

struct acl_entry {
    u16 tag;
    u16 perm;
    u32 id;
};

struct acl_blob {
    u32 version;
    struct acl_entry entries[6];
};

static unsigned failures;

static void put(const char *value) {
    u64 length = 0;
    while (value[length]) length++;
    (void)syscall3(SYS_WRITE, 1, (s64)value, (s64)length);
}

static void put_number(s64 value) {
    char digits[24];
    int count = 0;
    u64 magnitude = value < 0 ? (u64)-value : (u64)value;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    if (value < 0) put("-");
    char out[24];
    for (int index = 0; index < count; index++) out[index] = digits[count - 1 - index];
    out[count] = 0;
    put(out);
}

static void check(const char *name, int ok, s64 detail) {
    put("ACLTEST ");
    put(name);
    put(ok ? " PASS" : " FAIL ");
    if (!ok) put_number(detail);
    put("\n");
    if (!ok) failures++;
}

static s64 open_as(u64 uid) {
    s64 child = syscall0(SYS_FORK);
    if (child == 0) {
        (void)syscall3(SYS_SETRESUID, (s64)uid, (s64)uid, (s64)uid);
        s64 fd = syscall3(SYS_OPEN, (s64)FILE_PATH, O_RDWR, 0);
        syscall1(SYS_EXIT, fd >= 0 ? 0 : (s64)-fd);
    }
    int status = 0;
    (void)syscall4(SYS_WAIT4, child, (s64)&status, 0, 0);
    return (status >> 8) & 0xFF;
}

static s64 set_as(u64 uid, const struct acl_blob *blob) {
    s64 child = syscall0(SYS_FORK);
    if (child == 0) {
        (void)syscall3(SYS_SETRESUID, (s64)uid, (s64)uid, (s64)uid);
        s64 result = syscall6(SYS_SETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)blob,
                              (s64)sizeof(*blob), 0, 0);
        syscall1(SYS_EXIT, result == 0 ? 0 : (s64)-result);
    }
    int status = 0;
    (void)syscall4(SYS_WAIT4, child, (s64)&status, 0, 0);
    return (status >> 8) & 0xFF;
}

static unsigned file_mode(void) {
    unsigned stat_buffer[36];
    (void)syscall4(SYS_NEWFSTATAT, AT_FDCWD_TEST, (s64)FILE_PATH, (s64)stat_buffer, 0);
#if defined(__x86_64__)
    return stat_buffer[6] & 07777U;
#else
    return stat_buffer[4] & 07777U;
#endif
}

static int same(const void *a, const void *b, u64 length) {
    const unsigned char *x = a, *y = b;
    for (u64 index = 0; index < length; index++)
        if (x[index] != y[index]) return 0;
    return 1;
}

void run(void) {
    (void)syscall2(SYS_MKDIR, (s64)"/tmp", 01777);
    (void)syscall2(SYS_MKDIR, (s64)"/tmp/acl", 0755);
    s64 status = syscall6(SYS_MOUNT, (s64)"none", (s64)"/tmp/acl", (s64)"tmpfs", 0,
                          (s64)"mode=0755", 0);
    check("mount-tmpfs", status == 0, status);
    s64 fd = syscall3(SYS_OPEN, (s64)FILE_PATH, O_CREAT | O_RDWR, 0600);
    (void)syscall1(SYS_CLOSE, fd);
    check("create", fd >= 0, fd);

    char buffer[256];
    s64 got = syscall4(SYS_GETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)buffer, sizeof(buffer));
    check("no-acl-yet", got == -ENODATA, got);
    check("stranger-refused-before", open_as(1000) == EACCES, open_as(1000));

    struct acl_blob blob = { 2, {
        { 0x01, 6, 0xFFFFFFFFU },
        { 0x02, 6, 1000 },
        { 0x04, 0, 0xFFFFFFFFU },
        { 0x10, 6, 0xFFFFFFFFU },
        { 0x20, 0, 0xFFFFFFFFU },
        { 0x20, 0, 0xFFFFFFFFU },
    } };
    status = syscall6(SYS_SETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)&blob, (s64)sizeof(blob), 0, 0);
    check("duplicate-other-refused", status == -EINVAL, status);
    u64 length = 4 + 5 * sizeof(struct acl_entry);
    status = syscall6(SYS_SETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)&blob, (s64)length, 0, 0);
    check("set-acl", status == 0, status);

    got = syscall4(SYS_GETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, 0, 0);
    check("size-query", got == (s64)length, got);
    got = syscall4(SYS_GETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)buffer, sizeof(buffer));
    check("round-trip", got == (s64)length && same(buffer, &blob, length), got);
    got = syscall3(SYS_LISTXATTR, (s64)FILE_PATH, (s64)buffer, sizeof(buffer));
    check("listed", got == (s64)sizeof(ACL_NAME) && same(buffer, ACL_NAME, sizeof(ACL_NAME)), got);
    check("mode-shows-mask", file_mode() == 0660, file_mode());

    check("named-user-opens", open_as(1000) == 0, open_as(1000));
    check("other-user-refused", open_as(1001) == EACCES, open_as(1001));
    check("non-owner-cannot-set", set_as(1000, &blob) == EPERM, set_as(1000, &blob));

    status = syscall2(SYS_CHMOD, (s64)FILE_PATH, 0600);
    check("chmod-clears-mask", status == 0 && open_as(1000) == EACCES, open_as(1000));
    (void)syscall2(SYS_CHMOD, (s64)FILE_PATH, 0660);
    check("chmod-restores-mask", open_as(1000) == 0, open_as(1000));

    struct acl_blob unmasked = blob;
    unmasked.entries[3] = unmasked.entries[4];
    status = syscall6(SYS_SETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)&unmasked,
                      (s64)(4 + 4 * sizeof(struct acl_entry)), 0, 0);
    check("named-without-mask-refused", status == -EINVAL, status);

    status = syscall2(SYS_REMOVEXATTR, (s64)FILE_PATH, (s64)ACL_NAME);
    got = syscall4(SYS_GETXATTR, (s64)FILE_PATH, (s64)ACL_NAME, (s64)buffer, sizeof(buffer));
    check("removed", status == 0 && got == -ENODATA, got);
    (void)syscall2(SYS_CHMOD, (s64)FILE_PATH, 0600);
    check("removed-user-refused", open_as(1000) == EACCES, open_as(1000));

    status = syscall6(SYS_SETXATTR, (s64)"/sbin/init", (s64)ACL_NAME, (s64)&blob, (s64)length, 0, 0);
    check("disk-file-unsupported", status == -EOPNOTSUPP, status);

    put(failures ? "ACLTEST FAIL\n" : "ACLTEST PASS\n");
    syscall1(SYS_EXIT_GROUP, 0);
    for (;;) { }
}

TUNIX_START(run)
