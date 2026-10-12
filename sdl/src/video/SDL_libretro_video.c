/*
  SDL 1.2 libretro video driver
*/

#include "SDL_config.h"

#if SDL_VIDEO_DRIVER_LIBRETRO

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_sysvideo.h"
#include "SDL_pixels_c.h"
#include "SDL_events_c.h"

#include <stdint.h>

#define _THIS SDL_VideoDevice *this

#define LIBRETRO_DRIVER_NAME "libretro"

/* The panel itself is 320x240, but cores are free to render at their own
 * internal resolution: the multicore frontend scales the OSD region down to
 * the LCD in hardware. Hardcoding 320x240 here (as this driver used to) made
 * SDL_GetVideoInfo() report 320x240, which in turn pinned DevilutionX's
 * internal resolution to 320x240 -- and since its UI is laid out for 640x480
 * (see CalculateUIRectangle()), every dialog was drawn at offset (-160,-120)
 * and clipped.
 *
 * So honour whatever the core asks for, clamped to what the OSD scaler and
 * this device's RAM can take. */
#define LIBRETRO_MAX_WIDTH  640
#define LIBRETRO_MAX_HEIGHT 480

static uint16_t *s_libretro_framebuffer = NULL;
static size_t s_libretro_framebuffer_size = 0;
static int s_libretro_fb_width = 0;
static int s_libretro_fb_height = 0;

/* Report the live framebuffer size to the core wrapper, which needs it for the
 * frame it presents from libretro_delay() when no flip happened yet. */
int libretro_screen_width(void);
int libretro_screen_height(void);
uint16_t *libretro_screen_pixels(void);

int libretro_screen_width(void) { return s_libretro_fb_width; }
int libretro_screen_height(void) { return s_libretro_fb_height; }
uint16_t *libretro_screen_pixels(void) { return s_libretro_framebuffer; }

extern void libretro_video_flip(const void *pixels, int width, int height, int pitch);
extern void libretro_poll_events(void);

static int LIBRETRO_Available(void)
{
    return 1;
}

static int LIBRETRO_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
    vformat->BitsPerPixel = 16;
    vformat->BytesPerPixel = 2;
    vformat->Rloss = 3;
    vformat->Gloss = 2;
    vformat->Bloss = 3;
    vformat->Aloss = 8;
    vformat->Rshift = 11;
    vformat->Gshift = 5;
    vformat->Bshift = 0;
    vformat->Ashift = 0;
    vformat->Rmask = 0xF800;
    vformat->Gmask = 0x07E0;
    vformat->Bmask = 0x001F;
    vformat->Amask = 0;

    return 0;
}

static SDL_Rect **LIBRETRO_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
    return (SDL_Rect **) -1;
}

static SDL_Surface *LIBRETRO_SetVideoMode(_THIS, SDL_Surface *current,
                                         int width, int height, int bpp, Uint32 flags)
{
    if (width  < 1 || width  > LIBRETRO_MAX_WIDTH)  width  = LIBRETRO_MAX_WIDTH;
    if (height < 1 || height > LIBRETRO_MAX_HEIGHT) height = LIBRETRO_MAX_HEIGHT;

    size_t need = (size_t)width * height * sizeof(uint16_t);
    if (need != s_libretro_framebuffer_size) {
        uint16_t *buf = (uint16_t *)SDL_realloc(s_libretro_framebuffer, need);
        if (buf == NULL) return NULL;
        s_libretro_framebuffer = buf;
        s_libretro_framebuffer_size = need;
    }
    s_libretro_fb_width = width;
    s_libretro_fb_height = height;

    current->flags = flags | SDL_PREALLOC;
    current->w = width;
    current->h = height;
    current->pitch = width * sizeof(uint16_t);
    current->pixels = s_libretro_framebuffer;

    if (!SDL_ReallocFormat(current, 16, 0xF800, 0x07E0, 0x001F, 0)) {
        return NULL;
    }

    return current;
}

static void LIBRETRO_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
    (void)rects;
    (void)numrects;
    libretro_video_flip(s_libretro_framebuffer, s_libretro_fb_width, s_libretro_fb_height,
        s_libretro_fb_width * sizeof(uint16_t));
}

static int LIBRETRO_AllocHWSurface(_THIS, SDL_Surface *surface) { return -1; }
static void LIBRETRO_FreeHWSurface(_THIS, SDL_Surface *surface) {}
static int LIBRETRO_LockHWSurface(_THIS, SDL_Surface *surface) { return 0; }
static void LIBRETRO_UnlockHWSurface(_THIS, SDL_Surface *surface) {}
static void LIBRETRO_VideoQuit(_THIS)
{
    SDL_free(s_libretro_framebuffer);
    s_libretro_framebuffer = NULL;
    s_libretro_framebuffer_size = 0;
    s_libretro_fb_width = 0;
    s_libretro_fb_height = 0;
}

static void LIBRETRO_InitOSKeymap(_THIS)
{
}

static void LIBRETRO_PumpEvents(_THIS)
{
    libretro_poll_events();
}

static void LIBRETRO_DeleteDevice(SDL_VideoDevice *device)
{
    if (device) {
        SDL_free(device);
    }
}

static SDL_VideoDevice *LIBRETRO_CreateDevice(int devindex)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
    if (!device) return NULL;
    SDL_memset(device, 0, sizeof(*device));

    device->VideoInit = LIBRETRO_VideoInit;
    device->ListModes = LIBRETRO_ListModes;
    device->SetVideoMode = LIBRETRO_SetVideoMode;
    device->UpdateRects = LIBRETRO_UpdateRects;
    device->VideoQuit = LIBRETRO_VideoQuit;
    device->AllocHWSurface = LIBRETRO_AllocHWSurface;
    device->LockHWSurface = LIBRETRO_LockHWSurface;
    device->UnlockHWSurface = LIBRETRO_UnlockHWSurface;
    device->FreeHWSurface = LIBRETRO_FreeHWSurface;
    device->InitOSKeymap = LIBRETRO_InitOSKeymap;
    device->PumpEvents = LIBRETRO_PumpEvents;
    device->free = LIBRETRO_DeleteDevice;

    return device;
}

VideoBootStrap LIBRETRO_bootstrap = {
    LIBRETRO_DRIVER_NAME, "SDL libretro video driver",
    LIBRETRO_Available, LIBRETRO_CreateDevice
};

#endif /* SDL_VIDEO_DRIVER_LIBRETRO */
