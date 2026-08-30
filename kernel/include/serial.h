#ifndef TUNIX_SERIAL_H
#define TUNIX_SERIAL_H

void serial_init(void);
/* Whether there is a 16550 at 0x3F8 at all. Everything below is a no-op when
   there is not; see serial.c for why that matters. */
int serial_present(void);
void serial_write_char(char c);
/* One byte, or -1 when there is none waiting. */
int serial_read_char(void);
/* How many bytes one poll may take before it must stop, whatever the port
   says. A port stuck reporting data must not be able to hold a tick. */
unsigned serial_read_limit(void);

#endif
