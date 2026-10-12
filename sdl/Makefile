# SDL 1.2 with the libretro video driver, built for two targets:
#   make            -> libsf2k_sdl.a   MIPS32r2, soft-float  (the console)
#   make HOST_ARCH=-m32 -> libSDL_host.a  host, 32-bit        (the gbsim test model)
#
# The tree is stock SDL 1.2.15 with three files replaced by the SF2000 versions:
# SDL_libretro_video.c (video driver that hands frames to libretro),
# SDL_dummyaudio.c    (no audio device on the console) and a minimal
# SDL_config.h. Everything else is upstream.
#
# SDL_THREADS_DISABLED is deliberate: the console has no threads and no FPU, so
# SDL_CreateThread fails and the mutexes/condvars are no-ops. eduke32 is run on a
# libco coroutine, which is where the frame boundary actually is.

MIPS       ?= /opt/frog-toolchain/mipsel-mti-elf_stable/bin/mipsel-mti-elf-
CC         := $(MIPS)gcc
AR         := $(MIPS)ar
RANLIB     := $(MIPS)ranlib

HOSTCC     ?= gcc
HOSTAR     ?= ar
HOSTRANLIB ?= ranlib
HOST_ARCH  ?=

MIPS_ARCH := -EL -march=mips32 -mtune=mips32 -msoft-float -G0 -mno-abicalls -fno-pic \
            -ffast-math -fomit-frame-pointer -ffunction-sections -fdata-sections
MIPS_DEFS := -DSF2000 -DNDEBUG

# The host model deliberately matches the console: -m32 for the pointer model and
# -mfpmath=387 with no SSE so every float goes through the x87 stack exactly like
# the soft-float MIPS build.
HOST_DEFS := -DSF2000 -DNDEBUG

INCS := -Iinclude -Iinclude/SDL -Isrc -Isrc/video -Isrc/events -Isrc/thread -Isrc/audio

SRCS := src/SDL.c \
        src/SDL_error.c \
        src/SDL_fatal.c \
        src/video/SDL_video.c \
        src/video/SDL_surface.c \
        src/video/SDL_pixels.c \
        src/video/SDL_blit.c \
        src/video/SDL_blit_0.c \
        src/video/SDL_blit_1.c \
        src/video/SDL_blit_A.c \
        src/video/SDL_blit_N.c \
        src/video/SDL_RLEaccel.c \
        src/video/SDL_cursor.c \
        src/video/SDL_bmp.c \
        src/video/SDL_libretro_video.c \
        src/video/SDL_stretch.c \
        src/events/SDL_events.c \
        src/events/SDL_keyboard.c \
        src/events/SDL_mouse.c \
        src/events/SDL_active.c \
        src/events/SDL_quit.c \
        src/events/SDL_resize.c \
        src/file/SDL_rwops.c \
        src/timer/SDL_timer.c \
        src/timer/SDL_systimer.c \
        src/thread/SDL_thread.c \
        src/thread/SDL_systhread.c \
        src/thread/SDL_sysmutex.c \
        src/thread/SDL_syssem.c \
        src/thread/SDL_syscond.c \
        src/audio/SDL_audio.c \
        src/audio/SDL_audiocvt.c \
        src/audio/SDL_mixer.c \
        src/audio/SDL_dummyaudio.c \
        src/cpuinfo/SDL_cpuinfo.c \
        src/stdlib/SDL_string.c \
        src/stdlib/SDL_getenv.c

MIPS_OBJS  := $(patsubst %.c,build/mips/%.o,$(SRCS))
HOST_OBJS  := $(patsubst %.c,build/host/%.o,$(SRCS))

all: libsf2k_sdl.a

libsf2k_sdl.a: $(MIPS_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	$(AR) rcs $@ $^
	$(RANLIB) $@

libSDL_host.a: $(HOST_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	$(HOSTAR) rcs $@ $^
	$(HOSTRANLIB) $@

build/mips/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) -c $(MIPS_ARCH) -O2 $(MIPS_DEFS) $(INCS) -MMD -MP $< -o $@

# -fPIC so the host archive can go into a dlopen()ed libretro core.
build/host/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  HOSTCC  $<"
	@$(HOSTCC) $(HOST_ARCH) -c -O2 -fPIC -fno-strict-aliasing -Wno-unused \
	    $(HOST_DEFS) $(INCS) -MMD -MP $< -o $@

clean:
	rm -rf build libsf2k_sdl.a libSDL_host.a

.PHONY: all clean

-include $(MIPS_OBJS:.o=.d) $(HOST_OBJS:.o=.d)