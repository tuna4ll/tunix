#include <stddef.h>
#include <stdint.h>
#include "../include/heap.h"
#include "../include/file.h"
#include "../include/kstring.h"
#include "../include/cred.h"
#include "../include/pipe.h"
#include "../include/process.h"
#include "../include/klock.h"
#include "../include/oplock.h"
#include "../include/spinlock.h"
#include "../include/unix_socket.h"

#define EADDRINUSE 98
#define EAFNOSUPPORT 97
#define EAGAIN 11
#define EALREADY 114
#define ECONNREFUSED 111
#define EINVAL 22
#define ENAMETOOLONG 36
#define ENOTCONN 107
#define EPIPE 32

#define EMSGSIZE 90

#define UNIX_PENDING_MAX 4096
#define UNIX_RIGHTS_MAX UNIX_MAX_RIGHTS
#define UNIX_QUEUE_MAX 4096

struct unix_record_queue {
    uint32_t *lengths;
    struct unix_credentials *senders;
    int head;
    int tail;
    int count;
    int capacity;
};

struct unix_ancillary {
    size_t file_count;
    size_t offset;
    struct file *files[];
};

struct unix_ancillary_queue {
    struct unix_ancillary **entries;
    int head;
    int tail;
    int count;
    int capacity;
};

struct unix_channel {
    spinlock_t lock;
    struct pipe_buffer to_a;
    struct pipe_buffer to_b;
    struct unix_ancillary_queue ancillary_to_a;
    struct unix_ancillary_queue ancillary_to_b;
    struct unix_record_queue records_to_a;
    struct unix_record_queue records_to_b;
    struct unix_credentials a_credentials;
    struct unix_credentials b_credentials;
    char a_path[108];
    char b_path[108];
    int refs;
    int a_open;
    int b_open;
    int a_read_shutdown;
    int a_write_shutdown;
    int b_read_shutdown;
    int b_write_shutdown;
};

struct unix_socket {
    int refs;
    int listening;
    int connected;
    int seqpacket;
    int side;
    int backlog;
    int passcred;
    struct unix_credentials credentials;
    struct unix_credentials last_sender;
    char path[108];
    char *key;
    struct unix_channel *channel;
    struct unix_socket *pending_head;
    struct unix_socket *pending_tail;
    struct unix_socket *pending_next;
    int pending_count;
    struct unix_socket *next_listener;
};

static void channel_enter(struct unix_socket *socket) {
    if (kernel_lock_shared_here() && socket && socket->channel)
        spinlock_acquire(&socket->channel->lock);
}

static void channel_leave(struct unix_socket *socket) {
    if (kernel_lock_shared_here() && socket && socket->channel)
        spinlock_release(&socket->channel->lock);
}

static struct unix_socket *listener_list;

static struct pipe_buffer *incoming(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->to_a : &socket->channel->to_b;
}

static struct pipe_buffer *outgoing(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->to_b : &socket->channel->to_a;
}
static struct unix_record_queue *incoming_records(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->records_to_a :
                               &socket->channel->records_to_b;
}

static struct unix_record_queue *outgoing_records(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->records_to_b :
                               &socket->channel->records_to_a;
}

static struct unix_ancillary_queue *incoming_ancillary(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->ancillary_to_a :
                               &socket->channel->ancillary_to_b;
}

static struct unix_ancillary_queue *outgoing_ancillary(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->ancillary_to_b :
                               &socket->channel->ancillary_to_a;
}

static void ancillary_release(struct unix_ancillary *message) {
    if (!message) return;
    for (size_t index = 0; index < message->file_count; index++)
        if (message->files[index]) file_unref(message->files[index]);
    kfree(message);
}

static struct unix_ancillary *ancillary_at(const struct unix_ancillary_queue *queue, int step) {
    return queue->entries[(queue->head + step) % queue->capacity];
}

static void ancillary_queue_clear(struct unix_ancillary_queue *queue) {
    if (!queue) return;
    while (queue->count > 0) {
        ancillary_release(ancillary_at(queue, 0));
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
    }
    kfree(queue->entries);
    memset(queue, 0, sizeof(*queue));
}

static int ancillary_push(struct unix_ancillary_queue *queue, struct unix_ancillary *message) {
    if (queue->count == queue->capacity) {
        if (queue->capacity >= UNIX_QUEUE_MAX) return -EAGAIN;
        int capacity = queue->capacity ? queue->capacity * 2 : 8;
        struct unix_ancillary **entries = kmalloc((size_t)capacity * sizeof(*entries));
        if (!entries) return -EAGAIN;
        for (int step = 0; step < queue->count; step++) entries[step] = ancillary_at(queue, step);
        kfree(queue->entries);
        queue->entries = entries;
        queue->capacity = capacity;
        queue->head = 0;
        queue->tail = queue->count;
    }
    queue->entries[queue->tail] = message;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    return 0;
}

static void ancillary_consume(struct unix_ancillary_queue *queue,
                              size_t consumed, struct file **files,
                              size_t maximum_files, size_t *file_count) {
    if (file_count) *file_count = 0;
    if (!queue || !consumed) return;
    while (queue->count > 0) {
        struct unix_ancillary *message = ancillary_at(queue, 0);
        if (message->offset >= consumed) break;
        for (size_t index = 0; index < message->file_count; index++) {
            if (files && file_count && *file_count < maximum_files) {
                files[(*file_count)++] = message->files[index];
                message->files[index] = NULL;
            }
        }
        ancillary_release(message);
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
    }
    for (int step = 0; step < queue->count; step++) ancillary_at(queue, step)->offset -= consumed;
}

static size_t ancillary_read_limit(const struct unix_ancillary_queue *queue,
                                   size_t requested) {
    if (!queue || queue->count < 2) return requested;
    size_t boundary = ancillary_at(queue, 1)->offset;
    return boundary < requested ? boundary : requested;
}

static int record_reserve(struct unix_record_queue *records) {
    if (records->count < records->capacity) return 0;
    if (records->capacity >= UNIX_QUEUE_MAX) return -EAGAIN;
    int capacity = records->capacity ? records->capacity * 2 : 16;
    uint32_t *lengths = kmalloc((size_t)capacity * sizeof(*lengths));
    struct unix_credentials *senders = kmalloc((size_t)capacity * sizeof(*senders));
    if (!lengths || !senders) {
        kfree(lengths);
        kfree(senders);
        return -EAGAIN;
    }
    for (int step = 0; step < records->count; step++) {
        int from = (records->head + step) % records->capacity;
        lengths[step] = records->lengths[from];
        senders[step] = records->senders[from];
    }
    kfree(records->lengths);
    kfree(records->senders);
    records->lengths = lengths;
    records->senders = senders;
    records->capacity = capacity;
    records->head = 0;
    records->tail = records->count;
    return 0;
}

static void record_queue_free(struct unix_record_queue *records) {
    kfree(records->lengths);
    kfree(records->senders);
    memset(records, 0, sizeof(*records));
}

static int peer_open(struct unix_socket *socket) {
    if (!socket || !socket->channel) return 0;
    return socket->side == 0 ? socket->channel->b_open : socket->channel->a_open;
}

static int own_read_shutdown(struct unix_socket *socket) {
    if (!socket || !socket->channel) return 0;
    return socket->side == 0 ? socket->channel->a_read_shutdown : socket->channel->b_read_shutdown;
}

static int own_write_shutdown(struct unix_socket *socket) {
    if (!socket || !socket->channel) return 0;
    return socket->side == 0 ? socket->channel->a_write_shutdown : socket->channel->b_write_shutdown;
}

static int peer_read_shutdown(struct unix_socket *socket) {
    if (!socket || !socket->channel) return 0;
    return socket->side == 0 ? socket->channel->b_read_shutdown : socket->channel->a_read_shutdown;
}

static int peer_write_open(struct unix_socket *socket) {
    if (!peer_open(socket)) return 0;
    return socket->side == 0 ? !socket->channel->b_write_shutdown : !socket->channel->a_write_shutdown;
}

static void clear_pipe(struct pipe_buffer *pipe) {
    if (!pipe) return;
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
}

static void listener_unregister(struct unix_socket *socket) {
    struct unix_socket **link = &listener_list;
    while (*link) {
        if (*link == socket) {
            *link = socket->next_listener;
            socket->next_listener = NULL;
            return;
        }
        link = &(*link)->next_listener;
    }
}

struct unix_socket *unix_socket_create(int seqpacket) {
    struct unix_socket *socket = (struct unix_socket *)kmalloc(sizeof(*socket));
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->refs = 1;
    socket->seqpacket = seqpacket ? 1 : 0;
    socket->backlog = 128;
    return socket;
}

void unix_socket_set_credentials(struct unix_socket *socket, int32_t pid,
                                 uint32_t uid, uint32_t gid) {
    if (!socket) return;
    socket->credentials.pid = pid;
    socket->credentials.uid = uid;
    socket->credentials.gid = gid;
    if (socket->connected && socket->channel) {
        if (socket->side == 0) socket->channel->a_credentials = socket->credentials;
        else socket->channel->b_credentials = socket->credentials;
    }
}

int unix_socket_get_peer_credentials(struct unix_socket *socket,
                                     struct unix_credentials *credentials) {
    if (!socket || !credentials || !socket->connected || !socket->channel)
        return -ENOTCONN;
    *credentials = socket->side == 0 ? socket->channel->b_credentials :
                                       socket->channel->a_credentials;
    return 0;
}

int unix_socket_get_name(struct unix_socket *socket, int peer,
                         struct tunix_sockaddr_un *address, size_t *length) {
    if (!socket || !address || !length) return -EINVAL;
    const char *path = socket->path;
    if (peer) {
        if (!socket->connected || !socket->channel) return -ENOTCONN;
        path = socket->side == 0 ? socket->channel->b_path :
                                   socket->channel->a_path;
    }
    memset(address, 0, sizeof(*address));
    address->family = TUNIX_AF_UNIX;
    if (path && path[0] == '\x01') {
        size_t name_length = strlen(path + 1);
        if (name_length > sizeof(address->path) - 1) name_length = sizeof(address->path) - 1;
        address->path[0] = '\0';
        memcpy(address->path + 1, path + 1, name_length);
        *length = sizeof(address->family) + 1U + name_length;
        return 0;
    }
    size_t path_length = path && path[0] ? strlen(path) + 1U : 0U;
    if (path_length > sizeof(address->path)) path_length = sizeof(address->path);
    if (path_length) memcpy(address->path, path, path_length);
    *length = sizeof(address->family) + path_length;
    return 0;
}

void unix_socket_set_passcred(struct unix_socket *socket, int enabled) {
    if (socket) socket->passcred = enabled != 0;
}

int unix_socket_get_passcred(struct unix_socket *socket) {
    return socket && socket->passcred;
}

int unix_socket_pair(struct unix_socket **first, struct unix_socket **second,
                     int seqpacket) {
    if (!first || !second) return -EINVAL;
    *first = NULL;
    *second = NULL;
    struct unix_channel *channel = (struct unix_channel *)kmalloc(sizeof(*channel));
    struct unix_socket *a = unix_socket_create(seqpacket);
    struct unix_socket *b = unix_socket_create(seqpacket);
    if (!channel || !a || !b) {
        if (channel) kfree(channel);
        if (a) unix_socket_unref(a);
        if (b) unix_socket_unref(b);
        return -EAGAIN;
    }
    memset(channel, 0, sizeof(*channel));
    if (pipe_buffer_init(&channel->to_a, PIPE_CAPACITY) != 0 ||
        pipe_buffer_init(&channel->to_b, PIPE_CAPACITY) != 0) {
        pipe_buffer_fini(&channel->to_a);
        kfree(channel);
        unix_socket_unref(a);
        unix_socket_unref(b);
        return -EAGAIN;
    }
    spinlock_init(&channel->lock);
    channel->refs = 2;
    channel->a_open = 1;
    channel->b_open = 1;
    channel->a_credentials = a->credentials;
    channel->b_credentials = b->credentials;
    a->channel = channel;
    a->side = 0;
    a->connected = 1;
    b->channel = channel;
    b->side = 1;
    b->connected = 1;
    *first = a;
    *second = b;
    return 0;
}

void unix_socket_ref(struct unix_socket *socket) {
    if (socket) socket->refs++;
}

void unix_socket_unref(struct unix_socket *socket) {
    if (!socket || socket->refs <= 0) return;
    socket->refs--;
    if (socket->refs != 0) return;

    listener_unregister(socket);
    while (socket->pending_head) {
        struct unix_socket *pending = socket->pending_head;
        socket->pending_head = pending->pending_next;
        pending->pending_next = NULL;
        socket->pending_count--;
        unix_socket_unref(pending);
    }
    socket->pending_tail = NULL;

    if (socket->channel) {
        if (socket->side == 0) {
            socket->channel->a_open = 0;
            ancillary_queue_clear(&socket->channel->ancillary_to_a);
        } else {
            socket->channel->b_open = 0;
            ancillary_queue_clear(&socket->channel->ancillary_to_b);
        }
        socket->channel->refs--;
        if (socket->channel->refs == 0) {
            ancillary_queue_clear(&socket->channel->ancillary_to_a);
            ancillary_queue_clear(&socket->channel->ancillary_to_b);
            record_queue_free(&socket->channel->records_to_a);
            record_queue_free(&socket->channel->records_to_b);
            pipe_buffer_fini(&socket->channel->to_a);
            pipe_buffer_fini(&socket->channel->to_b);
            kfree(socket->channel);
        }
    }
    kfree(socket->key);
    kfree(socket);
}

static int copy_path(char destination[108], const struct tunix_sockaddr_un *address,
                     size_t length) {
    if (!address || length < sizeof(address->family) + 2 ||
        address->family != TUNIX_AF_UNIX) return -EAFNOSUPPORT;
    size_t maximum = length - sizeof(address->family);
    if (maximum > sizeof(address->path)) maximum = sizeof(address->path);
    int abstract = (address->path[0] == '\0');
    size_t start = abstract ? 1 : 0;
    size_t path_length = start;
    while (path_length < maximum && address->path[path_length]) path_length++;
    if (path_length == start) return -EINVAL;
    if (path_length >= sizeof(address->path)) return -ENAMETOOLONG;
    if (abstract) {
        destination[0] = '\x01';
        memcpy(destination + 1, address->path + 1, path_length - 1);
        destination[path_length] = '\0';
    } else {
        memcpy(destination, address->path, path_length);
        destination[path_length] = '\0';
    }
    return 0;
}

int unix_socket_bind(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                     size_t length, const char *resolved) {
    if (!socket || socket->connected || socket->listening || socket->path[0]) return -EINVAL;
    char path[108];
    int status = copy_path(path, address, length);
    if (status < 0) return status;
    const char *key = path[0] == '\x01' || !resolved ? path : resolved;
    for (struct unix_socket *bound = listener_list; bound; bound = bound->next_listener) {
        if (bound->key && strcmp(bound->key, key) == 0) return -EADDRINUSE;
    }
    size_t key_length = strlen(key);
    socket->key = (char *)kmalloc(key_length + 1);
    if (!socket->key) return -EAGAIN;
    memcpy(socket->key, key, key_length + 1);
    strncpy(socket->path, path, sizeof(socket->path) - 1);
    return 0;
}

int unix_socket_listen(struct unix_socket *socket, int backlog) {
    if (!socket || !socket->path[0] || socket->connected) return -EINVAL;
    for (struct unix_socket *bound = listener_list; bound; bound = bound->next_listener) {
        if (bound == socket) {
            socket->listening = 1;
            return 0;
        }
    }
    socket->next_listener = listener_list;
    listener_list = socket;
    socket->listening = 1;
    socket->backlog = backlog > 0 && backlog < UNIX_PENDING_MAX ? backlog : UNIX_PENDING_MAX;
    return 0;
}

static struct unix_socket *find_listener(const char *key) {
    for (struct unix_socket *bound = listener_list; bound; bound = bound->next_listener) {
        if (bound->listening && bound->key && strcmp(bound->key, key) == 0) return bound;
    }
    return NULL;
}

int unix_socket_connect(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                        size_t length, const char *resolved) {
    if (!socket) return -EINVAL;
    if (socket->connected) return -EALREADY;
    char path[108];
    int status = copy_path(path, address, length);
    if (status < 0) return status;
    struct unix_socket *listener = find_listener(path[0] == '\x01' || !resolved ? path : resolved);
    if (!listener || listener->pending_count >= listener->backlog) return -ECONNREFUSED;

    struct unix_channel *channel = (struct unix_channel *)kmalloc(sizeof(*channel));
    struct unix_socket *server = unix_socket_create(socket->seqpacket);
    if (!channel || !server) {
        if (channel) kfree(channel);
        if (server) unix_socket_unref(server);
        return -EAGAIN;
    }
    memset(channel, 0, sizeof(*channel));
    if (pipe_buffer_init(&channel->to_a, PIPE_CAPACITY) != 0 ||
        pipe_buffer_init(&channel->to_b, PIPE_CAPACITY) != 0) {
        pipe_buffer_fini(&channel->to_a);
        kfree(channel);
        unix_socket_unref(server);
        return -EAGAIN;
    }
    spinlock_init(&channel->lock);
    channel->refs = 2;
    channel->a_open = 1;
    channel->b_open = 1;
    channel->a_credentials = socket->credentials;
    channel->b_credentials = listener->credentials;
    strncpy(channel->a_path, socket->path, sizeof(channel->a_path) - 1U);
    strncpy(channel->b_path, listener->path, sizeof(channel->b_path) - 1U);

    socket->channel = channel;
    socket->side = 0;
    socket->connected = 1;
    server->credentials = listener->credentials;
    strncpy(server->path, listener->path, sizeof(server->path) - 1U);
    server->channel = channel;
    server->side = 1;
    server->connected = 1;

    server->pending_next = NULL;
    if (listener->pending_tail) listener->pending_tail->pending_next = server;
    else listener->pending_head = server;
    listener->pending_tail = server;
    listener->pending_count++;
    return 0;
}

struct unix_socket *unix_socket_accept(struct unix_socket *socket) {
    if (!socket || !socket->listening || socket->pending_count == 0) return NULL;
    struct unix_socket *accepted = socket->pending_head;
    socket->pending_head = accepted->pending_next;
    if (!socket->pending_head) socket->pending_tail = NULL;
    accepted->pending_next = NULL;
    socket->pending_count--;
    return accepted;
}

static int64_t unix_socket_read_data(struct unix_socket *socket, size_t size,
                                     void *buffer, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!socket || !socket->connected || !socket->channel) return -ENOTCONN;
    if (own_read_shutdown(socket)) return 0;
    struct pipe_buffer *pipe = incoming(socket);
    if (!pipe) return -ENOTCONN;
    uint8_t *out = (uint8_t *)buffer;

    if (socket->seqpacket) {
        struct unix_record_queue *records = incoming_records(socket);
        if (!records) return -ENOTCONN;
        if (records->count == 0) return peer_write_open(socket) ? -EAGAIN : 0;
        size_t record = records->lengths[records->head];
        socket->last_sender = records->senders[records->head];
        records->head = (records->head + 1) % records->capacity;
        records->count--;
        size_t deliver = size < record ? size : record;
        for (size_t index = 0; index < record; index++) {
            if (index < deliver) out[index] = pipe->data[pipe->read_pos];
            pipe->read_pos = (pipe->read_pos + 1) % pipe->capacity;
        }
        pipe->count -= record;
        if (consumed) *consumed = record;
        return (int64_t)deliver;
    }

    if (pipe->count == 0) return peer_write_open(socket) ? -EAGAIN : 0;
    size_t amount = size < pipe->count ? size : pipe->count;
    for (size_t index = 0; index < amount; index++) {
        out[index] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % pipe->capacity;
    }
    pipe->count -= amount;
    if (consumed) *consumed = amount;
    return (int64_t)amount;
}

int64_t unix_socket_read(struct unix_socket *socket, size_t size, void *buffer) {
    channel_enter(socket);
    size_t consumed = 0;
    int64_t result = unix_socket_read_data(socket, size, buffer, &consumed);
    struct unix_ancillary_queue *ancillary = incoming_ancillary(socket);
    if (ancillary && ancillary->count > 0) {
        oplock_enter();
        ancillary_consume(ancillary, consumed, NULL, 0, NULL);
        oplock_leave();
    }
    channel_leave(socket);
    return result;
}

static int64_t unix_socket_write_locked(struct unix_socket *socket, size_t size,
                                        const void *buffer) {
    if (!socket || !socket->connected || !socket->channel) return -ENOTCONN;
    if (own_write_shutdown(socket) || peer_read_shutdown(socket) || !peer_open(socket)) return -EPIPE;
    struct pipe_buffer *pipe = outgoing(socket);
    size_t available = pipe->capacity - pipe->count;
    const uint8_t *in = (const uint8_t *)buffer;

    if (socket->seqpacket) {
        struct unix_record_queue *records = outgoing_records(socket);
        if (!records) return -ENOTCONN;
        if (size > pipe->capacity) return -EMSGSIZE;
        if (size > available || record_reserve(records) != 0) return -EAGAIN;
        for (size_t index = 0; index < size; index++) {
            pipe->data[pipe->write_pos] = in[index];
            pipe->write_pos = (pipe->write_pos + 1) % pipe->capacity;
        }
        pipe->count += size;
        records->lengths[records->tail] = (uint32_t)size;
        struct unix_credentials *sender = &records->senders[records->tail];
        const struct credentials *self = cred_current();
        sender->pid = (int32_t)process_current_pid();
        sender->uid = self ? self->euid : 0U;
        sender->gid = self ? self->egid : 0U;
        records->tail = (records->tail + 1) % records->capacity;
        records->count++;
        return (int64_t)size;
    }

    if (!available) return -EAGAIN;
    size_t amount = size < available ? size : available;
    for (size_t index = 0; index < amount; index++) {
        pipe->data[pipe->write_pos] = in[index];
        pipe->write_pos = (pipe->write_pos + 1) % pipe->capacity;
    }
    pipe->count += amount;
    return (int64_t)amount;
}

int64_t unix_socket_write(struct unix_socket *socket, size_t size, const void *buffer) {
    channel_enter(socket);
    int64_t result = unix_socket_write_locked(socket, size, buffer);
    channel_leave(socket);
    return result;
}

int64_t unix_socket_send_with_rights(struct unix_socket *socket, size_t size,
                                     const void *buffer, struct file **files,
                                     size_t file_count) {
    if (file_count > UNIX_RIGHTS_MAX) return -EINVAL;
    struct unix_ancillary_queue *queue = outgoing_ancillary(socket);
    if (file_count && (!queue || queue->count >= UNIX_QUEUE_MAX)) return -EAGAIN;
    struct unix_ancillary *message = NULL;
    if (file_count) {
        message = kmalloc(sizeof(*message) + file_count * sizeof(struct file *));
        if (!message) return -EAGAIN;
    }
    struct pipe_buffer *pipe = outgoing(socket);
    size_t offset = pipe ? pipe->count : 0;
    int64_t result = unix_socket_write(socket, size, buffer);
    if (result < 0) {
        kfree(message);
        return result;
    }
    if (message) {
        message->file_count = file_count;
        message->offset = offset;
        for (size_t index = 0; index < file_count; index++) message->files[index] = files[index];
        if (ancillary_push(queue, message) != 0) {
            message->file_count = 0;
            kfree(message);
            for (size_t index = 0; index < file_count; index++) file_unref(files[index]);
        }
    }
    return result;
}

int64_t unix_socket_recv_with_rights(struct unix_socket *socket, size_t size,
                                     void *buffer, struct file **files,
                                     size_t maximum_files, size_t *file_count) {
    if (!file_count) return -EINVAL;
    *file_count = 0;
    struct unix_ancillary_queue *queue = incoming_ancillary(socket);
    size_t limited = socket && socket->seqpacket ? size : ancillary_read_limit(queue, size);
    size_t consumed = 0;
    int64_t result = unix_socket_read_data(socket, limited, buffer, &consumed);
    if (result < 0) return result;
    ancillary_consume(queue, consumed, files, maximum_files, file_count);
    return result;
}

void unix_socket_last_sender(struct unix_socket *socket,
                             struct unix_credentials *out) {
    if (!out) return;
    if (socket && socket->last_sender.pid) { *out = socket->last_sender; return; }
    if (!socket || unix_socket_get_peer_credentials(socket, out) != 0)
        memset(out, 0, sizeof(*out));
}

int unix_socket_read_ready(struct unix_socket *socket) {
    if (!socket) return 0;
    if (socket->listening) return socket->pending_count > 0;
    if (!socket->connected || !socket->channel) return 0;
    if (own_read_shutdown(socket)) return 1;
    if (socket->seqpacket) {
        struct unix_record_queue *records = incoming_records(socket);
        return (records && records->count > 0) || !peer_write_open(socket);
    }
    return incoming(socket)->count > 0 || !peer_write_open(socket);
}

size_t unix_socket_read_available(struct unix_socket *socket) {
    if (!socket || socket->listening || !socket->connected || !socket->channel)
        return 0;
    if (socket->seqpacket) {
        struct unix_record_queue *records = incoming_records(socket);
        if (!records || records->count <= 0) return 0;
        return records->lengths[records->head];
    }
    struct pipe_buffer *queue = incoming(socket);
    return queue ? queue->count : 0;
}

int unix_socket_write_ready(struct unix_socket *socket) {
    if (!socket || !socket->connected || !socket->channel || !peer_open(socket)) return 0;
    if (own_write_shutdown(socket) || peer_read_shutdown(socket)) return 0;
    return outgoing(socket)->count < PIPE_CAPACITY;
}

int unix_socket_peer_closed(struct unix_socket *socket) {
    return socket && socket->connected && socket->channel && !peer_write_open(socket);
}

int unix_socket_shutdown(struct unix_socket *socket, int how) {
    if (!socket || !socket->connected || !socket->channel) return -ENOTCONN;
    if (how < 0 || how > 2) return -EINVAL;
    if (how == 0 || how == 2) {
        if (socket->side == 0) socket->channel->a_read_shutdown = 1;
        else socket->channel->b_read_shutdown = 1;
        clear_pipe(incoming(socket));
        ancillary_queue_clear(incoming_ancillary(socket));
    }
    if (how == 1 || how == 2) {
        if (socket->side == 0) socket->channel->a_write_shutdown = 1;
        else socket->channel->b_write_shutdown = 1;
    }
    return 0;
}

int unix_socket_is_seqpacket(struct unix_socket *socket) {
    return socket && socket->seqpacket;
}

int unix_socket_is_listener(struct unix_socket *socket) {
    return socket && socket->listening;
}
