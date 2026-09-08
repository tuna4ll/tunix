#include <stddef.h>
#include <stdint.h>
#include "../include/cred.h"
#include "../include/eventfs.h"
#include "../include/heap.h"
#include "../include/klock.h"
#include "../include/kstring.h"
#include "../include/process.h"
#include "../include/vfs.h"

#define EAGAIN 11
#define EMSGSIZE 90
#define EVENTFS_MAX_SUBSCRIBERS 64U

struct eventfs_subscriber {
    struct eventfs_subscriber *next;
    enum eventfs_channel channel;
    uint32_t uid;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint64_t lost;
    char wait_token;
    uint8_t queue[EVENTFS_QUEUE_BYTES];
};

struct event_builder {
    char *data;
    size_t length;
    size_t capacity;
    int overflow;
};

static struct eventfs_subscriber *subscribers[EVENTFS_CHANNEL_COUNT];
static unsigned subscriber_count;
static int initialized;
static char format_buffer[EVENTFS_MAX_EVENT];

/* Kernel lock orders EventFS before process_wake_all's oplock. */
static void builder_char(struct event_builder *builder, char value) {
    if (builder->length >= builder->capacity) {
        builder->overflow = 1;
        return;
    }
    builder->data[builder->length++] = value;
}

static void builder_text(struct event_builder *builder, const char *text) {
    if (!text) text = "-";
    while (*text) builder_char(builder, *text++);
}

static void builder_uint(struct event_builder *builder, uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    while (count) builder_char(builder, digits[--count]);
}

static void builder_int(struct event_builder *builder, int value) {
    int64_t wide = value;
    if (wide < 0) {
        builder_char(builder, '-');
        wide = -wide;
    }
    builder_uint(builder, (uint64_t)wide);
}

static void builder_field(struct event_builder *builder, const char *value) {
    static const char hex[] = "0123456789abcdef";
    builder_char(builder, ' ');
    if (!value || !value[0]) {
        builder_text(builder, "\\0");
        return;
    }
    for (const unsigned char *at = (const unsigned char *)value; *at; at++) {
        unsigned char byte = *at;
        if (byte == ' ') builder_text(builder, "\\ ");
        else if (byte == '\\') builder_text(builder, "\\\\");
        else if (byte == '\n') builder_text(builder, "\\n");
        else if (byte == '\r') builder_text(builder, "\\r");
        else if (byte == '\t') builder_text(builder, "\\t");
        else if (byte < 0x20U || byte == 0x7FU) {
            builder_text(builder, "\\x");
            builder_char(builder, hex[byte >> 4]);
            builder_char(builder, hex[byte & 0xFU]);
        } else builder_char(builder, (char)byte);
    }
}

static uint8_t ring_get(const struct eventfs_subscriber *subscriber,
                        uint32_t offset) {
    return subscriber->queue[(subscriber->head + offset) % EVENTFS_QUEUE_BYTES];
}

static void ring_put(struct eventfs_subscriber *subscriber, uint8_t value) {
    subscriber->queue[subscriber->tail] = value;
    subscriber->tail = (subscriber->tail + 1U) % EVENTFS_QUEUE_BYTES;
    subscriber->used++;
}

static void ring_drop(struct eventfs_subscriber *subscriber, uint32_t count) {
    subscriber->head = (subscriber->head + count) % EVENTFS_QUEUE_BYTES;
    subscriber->used -= count;
}

static uint16_t next_length(const struct eventfs_subscriber *subscriber) {
    return (uint16_t)ring_get(subscriber, 0) |
           (uint16_t)((uint16_t)ring_get(subscriber, 1) << 8);
}

static void lose_event(struct eventfs_subscriber *subscriber) {
    if (subscriber->lost != UINT64_MAX) subscriber->lost++;
}

static int queue_event(struct eventfs_subscriber *subscriber, const char *event,
                       size_t length) {
    if (subscriber->lost || length > UINT16_MAX ||
        length + 2U > EVENTFS_QUEUE_BYTES - subscriber->used) {
        lose_event(subscriber);
        return -1;
    }
    ring_put(subscriber, (uint8_t)length);
    ring_put(subscriber, (uint8_t)(length >> 8));
    for (size_t index = 0; index < length; index++)
        ring_put(subscriber, (uint8_t)event[index]);
    return 0;
}

static int may_receive(const struct eventfs_subscriber *subscriber,
                       uint32_t uid, int system_event) {
    if (subscriber->uid == 0) return 1;
    return !system_event && subscriber->uid == uid;
}

static void publish(enum eventfs_channel channel, uint32_t uid,
                    int system_event, size_t length) {
    if (!initialized || channel <= 0 || channel >= EVENTFS_CHANNEL_COUNT) return;
    for (struct eventfs_subscriber *item = subscribers[channel]; item;
         item = item->next) {
        if (!may_receive(item, uid, system_event)) continue;
        int was_ready = item->used || item->lost;
        if (!length || length > EVENTFS_MAX_EVENT) lose_event(item);
        else (void)queue_event(item, format_buffer, length);
        if (!was_ready) process_wake_all(&item->wait_token);
    }
}

static size_t finish(struct event_builder *builder) {
    builder_char(builder, '\n');
    return builder->overflow ? 0 : builder->length;
}

static struct event_builder begin(const char *action) {
    struct event_builder builder = { format_buffer, 0, sizeof(format_buffer), 0 };
    builder_text(&builder, action);
    return builder;
}

struct eventfs_subscriber *eventfs_subscribe(enum eventfs_channel channel) {
    if (!initialized || channel <= 0 || channel >= EVENTFS_CHANNEL_COUNT ||
        subscriber_count >= EVENTFS_MAX_SUBSCRIBERS || kernel_lock_shared_here())
        return NULL;
    struct eventfs_subscriber *subscriber = kmalloc(sizeof(*subscriber));
    if (!subscriber) return NULL;
    memset(subscriber, 0, sizeof(*subscriber));
    const struct credentials *cred = cred_current();
    subscriber->uid = cred ? cred->euid : 0;
    subscriber->channel = channel;
    subscriber->next = subscribers[channel];
    subscribers[channel] = subscriber;
    subscriber_count++;
    return subscriber;
}

void eventfs_unsubscribe(struct eventfs_subscriber *subscriber) {
    if (!subscriber || subscriber->channel <= 0 ||
        subscriber->channel >= EVENTFS_CHANNEL_COUNT) return;
    struct eventfs_subscriber **at = &subscribers[subscriber->channel];
    while (*at && *at != subscriber) at = &(*at)->next;
    if (*at != subscriber) return;
    *at = subscriber->next;
    if (subscriber_count) subscriber_count--;
    process_wake_all(&subscriber->wait_token);
    kfree(subscriber);
}

static size_t lost_record(char output[32], uint64_t lost) {
    struct event_builder builder = { output, 0, 32, 0 };
    builder_text(&builder, "lost ");
    builder_uint(&builder, lost);
    builder_char(&builder, '\n');
    return builder.length;
}

int64_t eventfs_read(struct eventfs_subscriber *subscriber, size_t size,
                     void *buffer) {
    if (!subscriber || !buffer) return -EAGAIN;
    if (!size) return 0;
    uint8_t *out = buffer;
    size_t moved = 0;
    while (subscriber->used >= 2U) {
        uint16_t length = next_length(subscriber);
        if (length > EVENTFS_MAX_EVENT || (uint32_t)length + 2U > subscriber->used) {
            subscriber->head = subscriber->tail = subscriber->used = 0;
            lose_event(subscriber);
            break;
        }
        if (length > size - moved) {
            if (!moved) return -EMSGSIZE;
            break;
        }
        ring_drop(subscriber, 2U);
        for (uint16_t index = 0; index < length; index++) {
            out[moved++] = ring_get(subscriber, 0);
            ring_drop(subscriber, 1U);
        }
    }
    if (!subscriber->used && subscriber->lost) {
        char record[32];
        size_t length = lost_record(record, subscriber->lost);
        if (length > size - moved) {
            if (!moved) return -EMSGSIZE;
        } else {
            memcpy(out + moved, record, length);
            moved += length;
            subscriber->lost = 0;
        }
    }
    return moved ? (int64_t)moved : -EAGAIN;
}

int eventfs_read_ready(const struct eventfs_subscriber *subscriber) {
    return subscriber && (subscriber->used || subscriber->lost);
}

const void *eventfs_wait_channel(const struct eventfs_subscriber *subscriber) {
    return subscriber ? &subscriber->wait_token : NULL;
}

int eventfs_interested(enum eventfs_channel channel, uint32_t uid,
                       int system_event) {
    if (!initialized || channel <= 0 || channel >= EVENTFS_CHANNEL_COUNT)
        return 0;
    for (struct eventfs_subscriber *item = subscribers[channel]; item;
         item = item->next)
        if (may_receive(item, uid, system_event)) return 1;
    return 0;
}

void eventfs_emit_process_exec(uint32_t uid, uint64_t pid, const char *name) {
    if (!eventfs_interested(EVENTFS_PROCESS, uid, 0)) return;
    struct event_builder builder = begin("exec ");
    builder_uint(&builder, pid);
    builder_field(&builder, name);
    publish(EVENTFS_PROCESS, uid, 0, finish(&builder));
}

void eventfs_emit_process_fork(uint32_t uid, uint64_t parent, uint64_t child) {
    if (!eventfs_interested(EVENTFS_PROCESS, uid, 0)) return;
    struct event_builder builder = begin("fork ");
    builder_uint(&builder, parent);
    builder_char(&builder, ' ');
    builder_uint(&builder, child);
    publish(EVENTFS_PROCESS, uid, 0, finish(&builder));
}

void eventfs_emit_process_exit(uint32_t uid, uint64_t pid, int status) {
    if (!eventfs_interested(EVENTFS_PROCESS, uid, 0)) return;
    struct event_builder builder = begin("exit ");
    builder_uint(&builder, pid);
    builder_char(&builder, ' ');
    builder_int(&builder, status);
    publish(EVENTFS_PROCESS, uid, 0, finish(&builder));
}

void eventfs_emit_process_signal(uint32_t uid, uint64_t pid, int signal_number) {
    if (!eventfs_interested(EVENTFS_PROCESS, uid, 0)) return;
    struct event_builder builder = begin("signal ");
    builder_uint(&builder, pid);
    builder_char(&builder, ' ');
    builder_int(&builder, signal_number);
    publish(EVENTFS_PROCESS, uid, 0, finish(&builder));
}

void eventfs_emit_process_fault(uint32_t uid, uint64_t pid, const char *type,
                                const char *name) {
    if (!eventfs_interested(EVENTFS_PROCESS, uid, 0)) return;
    struct event_builder builder = begin("fault ");
    builder_uint(&builder, pid);
    builder_field(&builder, type);
    builder_field(&builder, name);
    publish(EVENTFS_PROCESS, uid, 0, finish(&builder));
}

static void emit_file_one(const char *action, uint32_t uid, uint64_t pid,
                          const char *path) {
    if (!eventfs_interested(EVENTFS_FILES, uid, 0)) return;
    struct event_builder builder = begin(action);
    builder_char(&builder, ' ');
    builder_uint(&builder, pid);
    builder_field(&builder, path);
    publish(EVENTFS_FILES, uid, 0, finish(&builder));
}

void eventfs_emit_file_create(uint32_t uid, uint64_t pid, const char *path) {
    emit_file_one("create", uid, pid, path);
}

void eventfs_emit_file_write(uint32_t uid, uint64_t pid, const char *path) {
    emit_file_one("write", uid, pid, path);
}

void eventfs_emit_file_rename(uint32_t uid, uint64_t pid, const char *old_path,
                              const char *new_path) {
    if (!eventfs_interested(EVENTFS_FILES, uid, 0)) return;
    struct event_builder builder = begin("rename ");
    builder_uint(&builder, pid);
    builder_field(&builder, old_path);
    builder_field(&builder, new_path);
    publish(EVENTFS_FILES, uid, 0, finish(&builder));
}

void eventfs_emit_file_remove(uint32_t uid, uint64_t pid, const char *path) {
    emit_file_one("remove", uid, pid, path);
}

static void emit_device(const char *action, const char *type, const char *name) {
    if (!eventfs_interested(EVENTFS_DEVICES, 0, 1)) return;
    struct event_builder builder = begin(action);
    builder_field(&builder, type);
    builder_field(&builder, name);
    publish(EVENTFS_DEVICES, 0, 1, finish(&builder));
}

void eventfs_emit_device_attach(const char *type, const char *name) {
    emit_device("attach", type, name);
}

void eventfs_emit_device_remove(const char *type, const char *name) {
    emit_device("remove", type, name);
}

static void emit_network(const char *action, uint32_t uid, uint64_t pid,
                         const char *proto, uint32_t local_address,
                         uint16_t local_port, uint32_t remote_address,
                         uint16_t remote_port) {
    if (!eventfs_interested(EVENTFS_NETWORK, uid, 0)) return;
    struct event_builder builder = begin(action);
    builder_char(&builder, ' ');
    builder_uint(&builder, pid);
    builder_field(&builder, proto);
    builder_char(&builder, ' ');
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        if (shift) builder_char(&builder, '.');
        builder_uint(&builder, (local_address >> shift) & 0xFFU);
    }
    builder_char(&builder, ':');
    builder_uint(&builder, local_port);
    builder_char(&builder, ' ');
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        if (shift) builder_char(&builder, '.');
        builder_uint(&builder, (remote_address >> shift) & 0xFFU);
    }
    builder_char(&builder, ':');
    builder_uint(&builder, remote_port);
    publish(EVENTFS_NETWORK, uid, 0, finish(&builder));
}

void eventfs_emit_network_connect(uint32_t uid, uint64_t pid, const char *proto,
                                  uint32_t local_address, uint16_t local_port,
                                  uint32_t remote_address, uint16_t remote_port) {
    emit_network("connect", uid, pid, proto, local_address, local_port,
                 remote_address, remote_port);
}

void eventfs_emit_network_accept(uint32_t uid, uint64_t pid, const char *proto,
                                 uint32_t local_address, uint16_t local_port,
                                 uint32_t remote_address, uint16_t remote_port) {
    emit_network("accept", uid, pid, proto, local_address, local_port,
                 remote_address, remote_port);
}

void eventfs_emit_network_close(uint32_t uid, uint64_t pid, const char *proto,
                                uint32_t local_address, uint16_t local_port,
                                uint32_t remote_address, uint16_t remote_port) {
    emit_network("close", uid, pid, proto, local_address, local_port,
                 remote_address, remote_port);
}

static int attach_stream(struct vfs_node *root, const char *name,
                         enum eventfs_channel channel) {
    struct vfs_node *node = vfs_alloc_node(name, VFS_FILE | VFS_READONLY |
                                                  VFS_VOLATILE | VFS_EVENTSTREAM);
    if (!node) return -1;
    node->mode = 0444;
    node->data = (void *)(uintptr_t)channel;
    if (vfs_attach(root, node) == 0) return 0;
    kfree(node);
    return -1;
}

void eventfs_init(void) {
    struct vfs_node *root = vfs_mkdir_p("/events");
    if (!root) return;
    root->mode = 0555;
    root->flags |= VFS_READONLY | VFS_VOLATILE;
    if (attach_stream(root, "process", EVENTFS_PROCESS) != 0 ||
        attach_stream(root, "files", EVENTFS_FILES) != 0 ||
        attach_stream(root, "devices", EVENTFS_DEVICES) != 0 ||
        attach_stream(root, "network", EVENTFS_NETWORK) != 0) return;
    vfs_mount_builtin("eventfs", "/events", "eventfs", root);
    initialized = 1;
}
