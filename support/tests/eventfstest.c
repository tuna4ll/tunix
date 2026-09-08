#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/fs/eventfs.c"

static struct credentials test_cred;
static struct vfs_node test_root;
static unsigned attached_nodes;
static unsigned allocated_nodes;
static struct vfs_node *test_nodes[4];
static unsigned wake_count;
static int allocation_fails;

void *kmalloc(size_t size) {
    return allocation_fails ? NULL : malloc(size);
}

void kfree(void *pointer) {
    free(pointer);
}

struct credentials *cred_current(void) {
    return &test_cred;
}

int kernel_lock_shared_here(void) {
    return 0;
}

int process_wake_all(const void *channel) {
    assert(channel);
    wake_count++;
    return 0;
}

struct vfs_node *vfs_mkdir_p(const char *path) {
    assert(strcmp(path, "/events") == 0);
    memset(&test_root, 0, sizeof(test_root));
    return &test_root;
}

struct vfs_node *vfs_alloc_node(const char *name, uint32_t flags) {
    struct vfs_node *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->flags = flags;
    assert(allocated_nodes < 4U);
    test_nodes[allocated_nodes++] = node;
    return node;
}

int vfs_attach(struct vfs_node *parent, struct vfs_node *child) {
    assert(parent == &test_root);
    assert(child);
    child->parent = parent;
    attached_nodes++;
    return 0;
}

void vfs_mount_builtin(const char *source, const char *target, const char *type,
                       struct vfs_node *root) {
    assert(strcmp(source, "eventfs") == 0);
    assert(strcmp(target, "/events") == 0);
    assert(strcmp(type, "eventfs") == 0);
    assert(root == &test_root);
}

static void use_uid(uint32_t uid) {
    memset(&test_cred, 0, sizeof(test_cred));
    test_cred.euid = uid;
}

static void expect_read(struct eventfs_subscriber *subscriber,
                        const char *expected) {
    char output[EVENTFS_MAX_EVENT + 64U];
    int64_t length = eventfs_read(subscriber, sizeof(output), output);
    assert(length == (int64_t)strlen(expected));
    assert(memcmp(output, expected, (size_t)length) == 0);
}

static void test_open_and_independent_readers(void) {
    use_uid(1000);
    eventfs_emit_process_exec(1000, 1, "old");
    struct eventfs_subscriber *first = eventfs_subscribe(EVENTFS_PROCESS);
    struct eventfs_subscriber *second = eventfs_subscribe(EVENTFS_PROCESS);
    assert(first && second);
    char output[64];
    assert(eventfs_read(first, sizeof(output), output) == -EAGAIN);
    eventfs_emit_process_exec(1000, 42, "bash");
    assert(eventfs_read_ready(first));
    assert(eventfs_wait_channel(first) != eventfs_wait_channel(second));
    expect_read(first, "exec 42 bash\n");
    expect_read(second, "exec 42 bash\n");
    eventfs_unsubscribe(first);
    eventfs_unsubscribe(second);
}

static void test_atomicity_and_escaping(void) {
    use_uid(1000);
    struct eventfs_subscriber *subscriber = eventfs_subscribe(EVENTFS_FILES);
    assert(subscriber);
    eventfs_emit_file_write(1000, 42, "/tmp/a b\tc\nd\\e\"q");
    char small[8];
    assert(eventfs_read(subscriber, sizeof(small), small) == -EMSGSIZE);
    assert(eventfs_read_ready(subscriber));
    expect_read(subscriber, "write 42 /tmp/a\\ b\\tc\\nd\\\\e\"q\n");
    assert(!eventfs_read_ready(subscriber));
    eventfs_unsubscribe(subscriber);
}

static void test_permissions(void) {
    use_uid(1001);
    struct eventfs_subscriber *other = eventfs_subscribe(EVENTFS_PROCESS);
    use_uid(0);
    struct eventfs_subscriber *root = eventfs_subscribe(EVENTFS_PROCESS);
    struct eventfs_subscriber *devices = eventfs_subscribe(EVENTFS_DEVICES);
    use_uid(1001);
    struct eventfs_subscriber *user_devices = eventfs_subscribe(EVENTFS_DEVICES);
    assert(other && root && devices && user_devices);
    eventfs_emit_process_exit(1000, 77, 0);
    char output[64];
    assert(eventfs_read(other, sizeof(output), output) == -EAGAIN);
    expect_read(root, "exit 77 0\n");
    eventfs_emit_device_attach("block", "sdb");
    expect_read(devices, "attach block sdb\n");
    assert(eventfs_read(user_devices, sizeof(output), output) == -EAGAIN);
    eventfs_unsubscribe(other);
    eventfs_unsubscribe(root);
    eventfs_unsubscribe(devices);
    eventfs_unsubscribe(user_devices);
}

static void test_overflow_and_wraparound(void) {
    use_uid(1000);
    struct eventfs_subscriber *subscriber = eventfs_subscribe(EVENTFS_PROCESS);
    assert(subscriber);
    for (unsigned index = 0; index < 2000U; index++)
        eventfs_emit_process_fork(1000, 1, index + 2U);
    char output[EVENTFS_QUEUE_BYTES + 64U];
    int64_t length = eventfs_read(subscriber, sizeof(output) - 1U, output);
    assert(length > 0);
    output[length] = '\0';
    char *lost = strstr(output, "lost ");
    assert(lost && lost > output && lost[strlen(lost) - 1U] == '\n');
    eventfs_emit_process_exec(1000, 90, "gcc");
    expect_read(subscriber, "exec 90 gcc\n");
    for (unsigned round = 0; round < 100U; round++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "exit %u 0\n", round);
        eventfs_emit_process_exit(1000, round, 0);
        expect_read(subscriber, expected);
    }
    eventfs_unsubscribe(subscriber);
}

static void test_oversize_and_allocation_failure(void) {
    use_uid(1000);
    struct eventfs_subscriber *subscriber = eventfs_subscribe(EVENTFS_PROCESS);
    assert(subscriber);
    char name[EVENTFS_MAX_EVENT];
    memset(name, 'x', sizeof(name) - 1U);
    name[sizeof(name) - 1U] = '\0';
    eventfs_emit_process_exec(1000, 1, name);
    expect_read(subscriber, "lost 1\n");
    eventfs_unsubscribe(subscriber);
    allocation_fails = 1;
    assert(eventfs_subscribe(EVENTFS_PROCESS) == NULL);
    allocation_fails = 0;
}

int main(void) {
    eventfs_init();
    assert(initialized);
    assert(attached_nodes == 4U);
    test_open_and_independent_readers();
    test_atomicity_and_escaping();
    test_permissions();
    test_overflow_and_wraparound();
    test_oversize_and_allocation_failure();
    assert(subscriber_count == 0U);
    assert(wake_count > 0U);
    for (unsigned index = 0; index < allocated_nodes; index++) free(test_nodes[index]);
    puts("eventfstest ok");
    return 0;
}
