#include "tunix_libc.h"
#include "procutil.h"

int main(void) {
    struct proc_info items[PROC_MAX];
    int count = proc_list(items, PROC_MAX);
    if (count < 0) {
        t_puterr("ps: cannot read /proc\n");
        return 1;
    }
    out_text("  PID  PPID S   RSS COMMAND\n");
    for (int index = 0; index < count; index++) {
        out_pad_number(items[index].pid, 5); out_text(" ");
        out_pad_number(items[index].ppid, 5); out_text(" ");
        t_write(1, &items[index].state, 1); out_text(" ");
        out_pad_number(items[index].rss_kb, 5); out_text("K ");
        out_text(items[index].command[0] ? items[index].command : items[index].name);
        out_text("\n");
    }
    return 0;
}
