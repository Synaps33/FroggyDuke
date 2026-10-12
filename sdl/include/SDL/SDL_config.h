/*
  Minimal SDL_config.h for FHeroes2 Libretro Core
*/

#ifndef _SDL_config_h
#define _SDL_config_h

#include "SDL_platform.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define HAVE_LIBC 1
#define HAVE_STDARG_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_QSORT 1
#define HAVE_ABS 1

#define SDL_BYTEORDER SDL_LIL_ENDIAN
#define SDL_HAS_64BIT_TYPE 1

/* Enable libretro video driver */
#define SDL_VIDEO_DRIVER_LIBRETRO 1

/* Enable generic / dummy sub-systems */
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_CDROM_DISABLED 1
#define SDL_JOYSTICK_DISABLED 1
#define SDL_LOADSO_DISABLED 1
#define SDL_THREADS_DISABLED 1
#define SDL_TIMER_DUMMY 1

#endif /* _SDL_config_h */
