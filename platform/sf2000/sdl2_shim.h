/*
 * SDL2 shim for the Data Frog SF2000 / GB300 -- platform side.
 *
 * The shim itself (sdl2_shim.c) talks to SDL, but four services only the host
 * can provide live here and are implemented in libretro_core.cpp:
 * presenting a frame, pumping input, clock, and sleeping. Keeping them behind
 * these hooks is also what lets the exact same shim object code be linked into
 * the host test model, so what the runner exercises is what the console runs.
 */
#ifndef SF2K_SDL2_SHIM_H
#define SF2K_SDL2_SHIM_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Implemented by libretro_core.cpp. */
void     sf2k_present(const void *pixels, int width, int height, int pitch);
void     sf2k_pump_events(void);
uint32_t sf2k_ticks_ms(void);
uint64_t sf2k_ticks_us(void);
void     sf2k_delay_ms(uint32_t ms);
int      sf2k_present_width(void);
int      sf2k_present_height(void);

/* Implemented by sdl2_shim.c. */
void      sf2k_sdl_set_mouse(int x, int y, int dx, int dy, unsigned buttons, int relative);
int       sf2k_sdl_send_key(SDL_Scancode scancode, int pressed);
void      sf2k_sdl_tick_keys(void);
void      sf2k_audio_pump(void *stream, int frames);

#ifdef __cplusplus
}
#endif

#endif /* SF2K_SDL2_SHIM_H */