#include "tunix_libc.h"
#include "procutil.h"

int main(void) {
    struct memory_info memory;
    if (proc_memory(&memory) != 0) {
        t_puterr("free: cannot read /proc/meminfo\n");
        return 1;
    }
    out_text("              total        used        free   available\n");
    out_text("Mem:     ");
    out_pad_number(memory.total_kb, 11); out_text(" ");
    out_pad_number(memory.used_kb, 11); out_text(" ");
    out_pad_number(memory.free_kb, 11); out_text(" ");
    out_pad_number(memory.available_kb, 11); out_text(" kB\n");
    out_text("Swap:              0           0           0           0 kB\n");
    return 0;
}
