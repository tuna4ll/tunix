#ifndef TUNIX_INPUT_H
#define TUNIX_INPUT_H

#include <stddef.h>
#include <stdint.h>

struct input_reader;
struct tunix_input_device_info;

void input_init(void);
void input_poll(void);
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
/* Linux's evdev ioctls. Takes the reader because EVIOCSCLOCKID and EVIOCGRAB
   are per-descriptor state, not per-device. */
int64_t input_reader_ioctl(struct input_reader *reader, unsigned device_id,
                           unsigned long request, uint64_t user_argument);
int64_t input_reader_read(struct input_reader *reader, size_t size, void *buffer);

#endif
