#define TEST_NAME "hwreport"
#include "../tests/test.h"

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sync();
    print("\nTunix hardware report\n\n"
          "  /tunix-hwreport.txt       the report\n"
          "  /tunix-vbios-*.rom        video bios images that were found\n"
          "  /tunix-gpu-regs.bin       nvidia register dump\n\n"
          "They are on the stick's tunix-root partition and already on disk.\n"
          "Switch the machine off with the power button.\n");
    for (;;) sleep_ms(60000);
}
