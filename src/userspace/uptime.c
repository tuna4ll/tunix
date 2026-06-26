#include "procutil.h"

int main(void) {
    uint64_t seconds = proc_uptime_centiseconds() / 100ULL;
    uint64_t days = seconds / 86400ULL;
    uint64_t hours = (seconds / 3600ULL) % 24ULL;
    uint64_t minutes = (seconds / 60ULL) % 60ULL;
    uint64_t remaining = seconds % 60ULL;
    out_text("up ");
    if (days) { out_number(days); out_text(days == 1 ? " day, " : " days, "); }
    if (hours < 10) out_text("0");
    out_number(hours);
    out_text(":");
    if (minutes < 10) out_text("0");
    out_number(minutes);
    out_text(":");
    if (remaining < 10) out_text("0");
    out_number(remaining);
    out_text("\n");
    return 0;
}
