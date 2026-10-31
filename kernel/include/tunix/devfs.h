#ifndef TUNIX_DEVFS_H
#define TUNIX_DEVFS_H

void devfs_init(void);
void devfs_add_block(int index);
void devfs_publish_sound(unsigned card, int playback, int capture);
void devfs_remove_sound(unsigned card);

#endif
