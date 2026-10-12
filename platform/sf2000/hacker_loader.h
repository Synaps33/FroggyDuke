#ifndef HACKER_LOADER_H
#define HACKER_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Matches the libretro callback type without pulling libretro.h into the
 * translation units that only draw the panel. */
typedef void (*retro_video_refresh_t)(const void *data, unsigned width,
                                      unsigned height, size_t pitch);

#ifdef __cplusplus
extern "C" {
#endif

void hacker_loader_set_video_cb(retro_video_refresh_t cb);
void hacker_loader_init(const char *pak_path);
void hacker_loader_log(const char *tag, const char *msg, int pct);
void hacker_loader_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* HACKER_LOADER_H */