#ifndef TUNIX_HDA_H
#define TUNIX_HDA_H

/* Probe the Intel HD Audio controller and register it with the sound core.
   Returns -1 when the machine has none. */
int hda_init(void);

#endif
