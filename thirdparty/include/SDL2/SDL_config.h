/*
 * SDL_config.h for the SF2000 / GB300.
 *
 * Replaces Debian's multiarch shim (which includes <SDL2/_real_SDL_config.h> and
 * so only works with a multiarch include path). The console has no X11, no
 * Wayland, no PulseAudio/ALSA and no threads, which is the whole point: every
 * subsystem is provided by platform/sf2000/sdl2_shim.c instead.
 */
#ifndef _SDL_config_h
#define _SDL_config_h

#include <stddef.h>
#include <stdint.h>

#define HAVE_LIBC            1
#define HAVE_STDIO_H         1
#define HAVE_STDLIB_H        1
#define HAVE_STDARG_H        1
#define HAVE_STRING_H        1
#define HAVE_INTTYPES_H      1
#define HAVE_STDINT_H        1
#define HAVE_SYS_TYPES_H     1

#define HAVE_MALLOC          1
#define HAVE_CALLOC          1
#define HAVE_REALLOC         1
#define HAVE_FREE            1
#define HAVE_MEMCPY          1
#define HAVE_MEMSET          1
#define HAVE_ABS             1

/* MIPS32 is little-endian in every build the console firmware ships. */
#define SDL_BYTEORDER        SDL_LIL_ENDIAN
#define SDL_HAS_64BIT_TYPE   1

/* Not a POSIX system as far as SDL is concerned: there is no dlopen, no
 * makecontext and no signal handling on the console, and pretending otherwise
 * would pull in code paths that cannot work. */
#define SDL_PLATFORM_UNIX    0

/* Threads: the console has none. */
#define SDL_THREAD_PTHREAD   0
#define SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 0
#define SDL_THREAD_WINDOWS   0

/* Video, audio, joystick: all supplied by this port's shim. */
#define SDL_VIDEO_DRIVER_LIBRETRO 1
#define SDL_VIDEO_OPENGL      0
#define SDL_VIDEO_OPENGLES   0
#define SDL_VIDEO_DUMMY      0

#define SDL_AUDIO_DRIVER_SF2000 1
#define SDL_AUDIO_DRIVER_DISK  0
#define SDL_AUDIO_DRIVER_PULSE 0
#define SDL_AUDIO_DRIVER_ALSA  0
#define SDL_AUDIO_DRIVER_DUMMY 0

#define SDL_JOYSTICK_SF2000  1
#define SDL_JOYSTICK_LINUX   0
#define SDL_JOYSTICK_DUMMY   0
#define SDL_JOYSTICK_HIDAPI  0

/* No dynamic loading, no clipboard, no power management, no threads API. */
#define SDL_LOADSO_DUMMY     1
#define SDL_CLIPBOARD_DUMMY  1
#define SDL_POWER_DUMMY      1
#define SDL_THREADS_DISABLED 1

#endif /* _SDL_config_h */