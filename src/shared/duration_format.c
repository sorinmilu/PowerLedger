#define _POSIX_C_SOURCE 200809L

#include "duration_format.h"

#include <stdio.h>

void duration_format_mins(int32_t mins, char *buf, size_t buflen)
{
    unsigned long hours;
    unsigned long minutes;
    unsigned long total;

    if (buf == NULL || buflen == 0U || mins < 0) {
        return;
    }

    total = (unsigned long)mins;
    hours = total / 60UL;
    minutes = total % 60UL;

    if (hours > 0UL) {
        (void)snprintf(buf, buflen, "%luh %02lum", hours, minutes);
    } else {
        (void)snprintf(buf, buflen, "%lum", minutes);
    }
}
