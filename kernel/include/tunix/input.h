#ifndef TUNIX_INPUT_H
#define TUNIX_INPUT_H

#include <stddef.h>
#include <stdint.h>

struct input_reader;
struct tunix_input_device_info;

void input_init(void);
void input_poll(void);
int input_poll_due(void);
void input_dispatch_console(void);
int input_console_pending(void);

void input_external_key(uint16_t keycode, int released);
void input_external_mouse(int dx, int dy, int wheel, uint8_t buttons);
void input_external_tablet(int32_t x, int32_t y, int32_t x_max, int32_t y_max, int wheel,
                           uint8_t buttons);
void input_irq(void);
int input_mouse_available(void);
int input_get_device_info(unsigned device_id, struct tunix_input_device_info *info);

void input_scancode_open(void);
void input_scancode_close(void);
int input_scancodes_ready(void);
int64_t input_read_scancodes(size_t size, void *buffer);

struct input_reader *input_reader_open(unsigned device_id);
void input_reader_close(struct input_reader *reader);
int input_reader_ready(struct input_reader *reader);

int64_t input_reader_ioctl(struct input_reader *reader, unsigned device_id,
                           unsigned long request, uint64_t user_argument);
int64_t input_reader_read(struct input_reader *reader, size_t size, void *buffer);

#define INPUT_KEY_HISTORY 128U

struct input_key_event {
    uint32_t millisecond;
    uint16_t code;
    uint16_t value;
    uint16_t readers;
};

unsigned input_key_history_count(void);
int input_key_history_at(unsigned index, struct input_key_event *out);

#endif
