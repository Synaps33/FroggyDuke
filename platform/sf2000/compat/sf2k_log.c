/* See sf2k_log.h for why this does not go through the frontend's xlog(). */

#include "sf2k_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sf2k_trace_open(const char *path)
{
    FILE *f = fopen("/mnt/sda1/eduke32_trace.txt", "w");
    if (!f)
        f = fopen("eduke32_trace.txt", "w");
    if (f) {
        fputs("=== TRACE START ===\n", f);
        fclose(f);
    }
}

void sf2k_trace(const char *fmt, ...)
{
    FILE *f = fopen("/mnt/sda1/eduke32_trace.txt", "a");
    if (!f)
        f = fopen("eduke32_trace.txt", "a");
    if (!f)
        return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);

    fclose(f);
}