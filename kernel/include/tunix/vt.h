#ifndef TUNIX_VT_H
#define TUNIX_VT_H

#include <stddef.h>
#include <stdint.h>

#define VT_COUNT 63U

#define VT_NODE_ACTIVE  0U
#define VT_NODE_CURRENT 0xFFU

struct tty;
struct vfs_node;

void vt_init(void);

struct tty *vt_tty(unsigned index);
struct tty *vt_active_tty(void);
unsigned vt_active_index(void);

unsigned vt_current_index(void);
struct tty *vt_current_tty(void);

int vt_switch(unsigned index);

int vt_wait_active(unsigned index);

const void *vt_switch_wait_channel(void);

const void *vt_input_wait_channel(void);
void vt_input_arrived(void);

int vt_is_hotkey(uint16_t keycode, int ctrl_held, int alt_held);
void vt_run_hotkey(uint16_t keycode, int pressed);

void vt_handle_key(uint16_t keycode, int pressed);

void vt_poll_input(void);
void vt_poll_from_tick(void);

int vt_console_in_front(void);
void vt_poll_serial(void);

int vt_input_delivered_to(unsigned index);

void vt_display_claimed(void);
void vt_display_released(void);

const void *vt_graphics_mode_owner(void);
int vt_graphics_takeover_allowed(const void *holder, const void *claimer);

void vt_process_exited(uint64_t pid, uint64_t sid);

int64_t vt_node_read(struct vfs_node *node, uint64_t offset, size_t size, void *buffer);
int64_t vt_node_write(struct vfs_node *node, uint64_t offset, size_t size, const void *buffer);
int vt_node_ready(struct vfs_node *node);
int64_t vt_node_ioctl(struct vfs_node *node, unsigned long request, uint64_t user_argument);

#endif
