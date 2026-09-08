#ifndef TUNIX_EVENTFS_H
#define TUNIX_EVENTFS_H

#include <stddef.h>
#include <stdint.h>

#define EVENTFS_MAX_EVENT 4096U
#define EVENTFS_QUEUE_BYTES 8192U

enum eventfs_channel {
    EVENTFS_PROCESS = 1,
    EVENTFS_FILES,
    EVENTFS_DEVICES,
    EVENTFS_NETWORK,
    EVENTFS_CHANNEL_COUNT
};

struct eventfs_subscriber;

/* Runtime APIs require Tunix's exclusive kernel lock. */
void eventfs_init(void);
struct eventfs_subscriber *eventfs_subscribe(enum eventfs_channel channel);
void eventfs_unsubscribe(struct eventfs_subscriber *subscriber);
int64_t eventfs_read(struct eventfs_subscriber *subscriber, size_t size,
                     void *buffer);
int eventfs_read_ready(const struct eventfs_subscriber *subscriber);
const void *eventfs_wait_channel(const struct eventfs_subscriber *subscriber);
int eventfs_interested(enum eventfs_channel channel, uint32_t uid,
                       int system_event);

void eventfs_emit_process_exec(uint32_t uid, uint64_t pid, const char *name);
void eventfs_emit_process_fork(uint32_t uid, uint64_t parent, uint64_t child);
void eventfs_emit_process_exit(uint32_t uid, uint64_t pid, int status);
void eventfs_emit_process_signal(uint32_t uid, uint64_t pid, int signal_number);
void eventfs_emit_process_fault(uint32_t uid, uint64_t pid, const char *type,
                                const char *name);

void eventfs_emit_file_create(uint32_t uid, uint64_t pid, const char *path);
void eventfs_emit_file_write(uint32_t uid, uint64_t pid, const char *path);
void eventfs_emit_file_rename(uint32_t uid, uint64_t pid, const char *old_path,
                              const char *new_path);
void eventfs_emit_file_remove(uint32_t uid, uint64_t pid, const char *path);

void eventfs_emit_device_attach(const char *type, const char *name);
void eventfs_emit_device_remove(const char *type, const char *name);

void eventfs_emit_network_connect(uint32_t uid, uint64_t pid, const char *proto,
                                  uint32_t local_address, uint16_t local_port,
                                  uint32_t remote_address, uint16_t remote_port);
void eventfs_emit_network_accept(uint32_t uid, uint64_t pid, const char *proto,
                                 uint32_t local_address, uint16_t local_port,
                                 uint32_t remote_address, uint16_t remote_port);
void eventfs_emit_network_close(uint32_t uid, uint64_t pid, const char *proto,
                                uint32_t local_address, uint16_t local_port,
                                uint32_t remote_address, uint16_t remote_port);

#endif
