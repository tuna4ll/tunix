#include <stddef.h>
#include <stdint.h>
#include "include/heap.h"
#include "include/file.h"
#include "include/kstring.h"
#include "include/pipe.h"
#include "include/unix_socket.h"

#define EADDRINUSE 98
#define EAFNOSUPPORT 97
#define EAGAIN 11
#define EALREADY 114
#define ECONNREFUSED 111
#define EINVAL 22
#define ENAMETOOLONG 36
#define ENOTCONN 107
#define EPIPE 32

#define UNIX_MAX_LISTENERS 8
#define UNIX_PENDING_MAX 8
#define UNIX_RIGHTS_MAX 8
#define UNIX_ANCILLARY_MAX 8

struct unix_ancillary {
    struct file *files[UNIX_RIGHTS_MAX];
    size_t file_count;
};

struct unix_ancillary_queue {
    struct unix_ancillary entries[UNIX_ANCILLARY_MAX];
    int head;
    int tail;
    int count;
};

struct unix_channel {
    struct pipe_buffer to_a;
    struct pipe_buffer to_b;
    struct unix_ancillary_queue ancillary_to_a;
    struct unix_ancillary_queue ancillary_to_b;
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
    int side;
    int backlog;
    int passcred;
    struct unix_credentials credentials;
    char path[108];
    struct unix_channel *channel;
    struct unix_socket *pending[UNIX_PENDING_MAX];
    int pending_head;
    int pending_tail;
    int pending_count;
};

static struct unix_socket *listeners[UNIX_MAX_LISTENERS];

static struct pipe_buffer *incoming(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->to_a : &socket->channel->to_b;
}

static struct pipe_buffer *outgoing(struct unix_socket *socket) {
    if (!socket || !socket->channel) return NULL;
    return socket->side == 0 ? &socket->channel->to_b : &socket->channel->to_a;
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
        file_unref(message->files[index]);
    memset(message, 0, sizeof(*message));
}

static void ancillary_queue_clear(struct unix_ancillary_queue *queue) {
    if (!queue) return;
    while (queue->count > 0) {
        ancillary_release(&queue->entries[queue->head]);
        queue->head = (queue->head + 1) % UNIX_ANCILLARY_MAX;
        queue->count--;
    }
    queue->head = 0;
    queue->tail = 0;
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
    for (int index = 0; index < UNIX_MAX_LISTENERS; index++) {
        if (listeners[index] == socket) listeners[index] = NULL;
    }
}

struct unix_socket *unix_socket_create(void) {
    struct unix_socket *socket = (struct unix_socket *)kmalloc(sizeof(*socket));
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->refs = 1;
    socket->backlog = UNIX_PENDING_MAX;
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


int unix_socket_pair(struct unix_socket **first, struct unix_socket **second) {
    if (!first || !second) return -EINVAL;
    *first = NULL;
    *second = NULL;
    struct unix_channel *channel = (struct unix_channel *)kmalloc(sizeof(*channel));
    struct unix_socket *a = unix_socket_create();
    struct unix_socket *b = unix_socket_create();
    if (!channel || !a || !b) {
        if (channel) kfree(channel);
        if (a) unix_socket_unref(a);
        if (b) unix_socket_unref(b);
        return -EAGAIN;
    }
    memset(channel, 0, sizeof(*channel));
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
    while (socket->pending_count > 0) {
        struct unix_socket *pending = socket->pending[socket->pending_head];
        socket->pending_head = (socket->pending_head + 1) % UNIX_PENDING_MAX;
        socket->pending_count--;
        unix_socket_unref(pending);
    }

    if (socket->channel) {
        if (socket->side == 0) socket->channel->a_open = 0;
        else socket->channel->b_open = 0;
        socket->channel->refs--;
        if (socket->channel->refs == 0) {
            ancillary_queue_clear(&socket->channel->ancillary_to_a);
            ancillary_queue_clear(&socket->channel->ancillary_to_b);
            kfree(socket->channel);
        }
    }
    kfree(socket);
}

static int copy_path(char destination[108], const struct tunix_sockaddr_un *address,
                     size_t length) {
    if (!address || length < sizeof(address->family) + 2 ||
        address->family != TUNIX_AF_UNIX) return -EAFNOSUPPORT;
    size_t maximum = length - sizeof(address->family);
    if (maximum > sizeof(address->path)) maximum = sizeof(address->path);
    size_t path_length = 0;
    while (path_length < maximum && address->path[path_length]) path_length++;
    if (path_length == 0) return -EINVAL;
    if (path_length >= sizeof(address->path)) return -ENAMETOOLONG;
    memcpy(destination, address->path, path_length);
    destination[path_length] = '\0';
    return 0;
}

int unix_socket_bind(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                     size_t length) {
    if (!socket || socket->connected || socket->listening || socket->path[0]) return -EINVAL;
    char path[108];
    int status = copy_path(path, address, length);
    if (status < 0) return status;
    for (int index = 0; index < UNIX_MAX_LISTENERS; index++) {
        if (listeners[index] && strcmp(listeners[index]->path, path) == 0)
            return -EADDRINUSE;
    }
    strncpy(socket->path, path, sizeof(socket->path) - 1);
    return 0;
}

int unix_socket_listen(struct unix_socket *socket, int backlog) {
    if (!socket || !socket->path[0] || socket->connected) return -EINVAL;
    for (int index = 0; index < UNIX_MAX_LISTENERS; index++) {
        if (listeners[index] == socket) {
            socket->listening = 1;
            return 0;
        }
    }
    for (int index = 0; index < UNIX_MAX_LISTENERS; index++) {
        if (!listeners[index]) {
            listeners[index] = socket;
            socket->listening = 1;
            socket->backlog = backlog > 0 && backlog < UNIX_PENDING_MAX ? backlog : UNIX_PENDING_MAX;
            return 0;
        }
    }
    return -EAGAIN;
}

static struct unix_socket *find_listener(const char *path) {
    for (int index = 0; index < UNIX_MAX_LISTENERS; index++) {
        if (listeners[index] && listeners[index]->listening &&
            strcmp(listeners[index]->path, path) == 0) return listeners[index];
    }
    return NULL;
}

int unix_socket_connect(struct unix_socket *socket, const struct tunix_sockaddr_un *address,
                        size_t length) {
    if (!socket) return -EINVAL;
    if (socket->connected) return -EALREADY;
    char path[108];
    int status = copy_path(path, address, length);
    if (status < 0) return status;
    struct unix_socket *listener = find_listener(path);
    if (!listener || listener->pending_count >= listener->backlog) return -ECONNREFUSED;

    struct unix_channel *channel = (struct unix_channel *)kmalloc(sizeof(*channel));
    struct unix_socket *server = unix_socket_create();
    if (!channel || !server) {
        if (channel) kfree(channel);
        if (server) unix_socket_unref(server);
        return -EAGAIN;
    }
    memset(channel, 0, sizeof(*channel));
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

    listener->pending[listener->pending_tail] = server;
    listener->pending_tail = (listener->pending_tail + 1) % UNIX_PENDING_MAX;
    listener->pending_count++;
    return 0;
}

struct unix_socket *unix_socket_accept(struct unix_socket *socket) {
    if (!socket || !socket->listening || socket->pending_count == 0) return NULL;
    struct unix_socket *accepted = socket->pending[socket->pending_head];
    socket->pending[socket->pending_head] = NULL;
    socket->pending_head = (socket->pending_head + 1) % UNIX_PENDING_MAX;
    socket->pending_count--;
    return accepted;
}

int64_t unix_socket_read(struct unix_socket *socket, size_t size, void *buffer) {
    if (!socket || !socket->connected || !socket->channel) return -ENOTCONN;
    if (own_read_shutdown(socket)) return 0;
    struct pipe_buffer *pipe = incoming(socket);
    if (!pipe) return -ENOTCONN;
    if (pipe->count == 0) return peer_write_open(socket) ? -EAGAIN : 0;
    uint8_t *out = (uint8_t *)buffer;
    size_t amount = size < pipe->count ? size : pipe->count;
    for (size_t index = 0; index < amount; index++) {
        out[index] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count -= amount;
    return (int64_t)amount;
}

int64_t unix_socket_write(struct unix_socket *socket, size_t size, const void *buffer) {
    if (!socket || !socket->connected || !socket->channel) return -ENOTCONN;
    if (own_write_shutdown(socket) || peer_read_shutdown(socket) || !peer_open(socket)) return -EPIPE;
    struct pipe_buffer *pipe = outgoing(socket);
    size_t available = PIPE_CAPACITY - pipe->count;
    if (!available) return -EAGAIN;
    const uint8_t *in = (const uint8_t *)buffer;
    size_t amount = size < available ? size : available;
    for (size_t index = 0; index < amount; index++) {
        pipe->data[pipe->write_pos] = in[index];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count += amount;
    return (int64_t)amount;
}

int64_t unix_socket_send_with_rights(struct unix_socket *socket, size_t size,
                                     const void *buffer, struct file **files,
                                     size_t file_count) {
    if (file_count > UNIX_RIGHTS_MAX) return -EINVAL;
    struct unix_ancillary_queue *queue = outgoing_ancillary(socket);
    if (file_count && (!queue || queue->count >= UNIX_ANCILLARY_MAX)) return -EAGAIN;
    int64_t result = unix_socket_write(socket, size, buffer);
    if (result < 0) return result;
    if (file_count) {
        struct unix_ancillary *message = &queue->entries[queue->tail];
        memset(message, 0, sizeof(*message));
        message->file_count = file_count;
        for (size_t index = 0; index < file_count; index++) message->files[index] = files[index];
        queue->tail = (queue->tail + 1) % UNIX_ANCILLARY_MAX;
        queue->count++;
    }
    return result;
}

int64_t unix_socket_recv_with_rights(struct unix_socket *socket, size_t size,
                                     void *buffer, struct file **files,
                                     size_t maximum_files, size_t *file_count) {
    if (!file_count) return -EINVAL;
    *file_count = 0;
    int64_t result = unix_socket_read(socket, size, buffer);
    if (result <= 0) return result;
    struct unix_ancillary_queue *queue = incoming_ancillary(socket);
    if (!queue || queue->count == 0) return result;
    struct unix_ancillary *message = &queue->entries[queue->head];
    size_t amount = message->file_count < maximum_files ? message->file_count : maximum_files;
    for (size_t index = 0; index < amount; index++) {
        files[index] = message->files[index];
        message->files[index] = NULL;
    }
    for (size_t index = amount; index < message->file_count; index++)
        file_unref(message->files[index]);
    *file_count = amount;
    memset(message, 0, sizeof(*message));
    queue->head = (queue->head + 1) % UNIX_ANCILLARY_MAX;
    queue->count--;
    return result;
}

int unix_socket_read_ready(struct unix_socket *socket) {
    if (!socket) return 0;
    if (socket->listening) return socket->pending_count > 0;
    if (!socket->connected || !socket->channel) return 0;
    if (own_read_shutdown(socket)) return 1;
    return incoming(socket)->count > 0 || !peer_write_open(socket);
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
    }
    if (how == 1 || how == 2) {
        if (socket->side == 0) socket->channel->a_write_shutdown = 1;
        else socket->channel->b_write_shutdown = 1;
    }
    return 0;
}

int unix_socket_is_listener(struct unix_socket *socket) {
    return socket && socket->listening;
}
