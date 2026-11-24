#ifndef TUNIX_CDROM_H
#define TUNIX_CDROM_H

#include <stdint.h>

#define CDROM_SENSE_NOT_READY      2
#define CDROM_SENSE_UNIT_ATTENTION 6

// Sends the 12-byte SCSI command COMMAND to a drive, reading BYTES of its data
// into BUFFER: 0 when it succeeds, the sense key (1 to 15) when the drive
// refuses it, -1 when the transport fails.
typedef int (*cdrom_packet_fn)(void *context, const uint8_t *command, void *buffer,
                               uint32_t bytes);

// Publishes an optical drive as srN, with or without a disc in it; MAX_BYTES is
// the most one command moves. The drive is watched for discs coming and going.
int cdrom_add(const char *driver, cdrom_packet_fn packet, void *context, uint32_t max_bytes);

// Looks at every drive now, for a disc that has only just become ready.
void cdrom_poll(void);

// Starts the thread that looks at the drives every few seconds.
void cdrom_start(void);

#endif
