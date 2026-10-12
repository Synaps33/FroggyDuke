/*
 * Trace logging for the console, independent of the frontend's log sink.
 *
 * The multicore frontend exports core_log_cb(), but it funnels into xlog(),
 * which keeps the log file buffered. When the core stops responding the
 * console reboots into FrogUI and the unwritten tail is lost -- so the one
 * line that matters, the last one before the hang, is exactly the one that
 * never reaches the card. Everything after "retro_load_game returned 1" was
 * missing from Multicore.log for that reason.
 *
 * So the core writes its own trace next to the game data and flushes every
 * line. Same directory as DUKE3D.GRP, which the core has already chdir()ed
 * into by the time anything is traced, and which the player can read from a
 * card reader after a failed run.
 */
#ifndef SF2K_LOG_H
#define SF2K_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Named before the first trace so the file exists even if the very first
 * thing that goes wrong is the one we want to see. */
void sf2k_trace_open(const char *path);

void sf2k_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#define SF2K_LOG(...) sf2k_trace(__VA_ARGS__)

#endif /* SF2K_LOG_H */