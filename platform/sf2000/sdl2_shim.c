/*
 * SDL2 shim for the Data Frog SF2000 / GB300.
 *
 * eduke32 at current HEAD is SDL2-only (sdlayer12.cpp is dead code that shares
 * a dozen file-statics with sdlayer.cpp and no longer compiles against the
 * current headers). So rather than revive the SDL 1.2 front end, this implements
 * the ~110 SDL2 entry points eduke32 actually calls, on top of libretro.
 *
 * Design constraints, all forced by the console:
 *
 *   - No GPU, so the window is a plain RGB565 software framebuffer. The
 *     multicore frontend scales whatever we present down to the 320x240 panel
 *     in hardware. Presenting at 320x200 -- BUILD's native resolution -- means
 *     softsurface_blitBuffer() runs its scanline table at 1:1 and the scaler
 *     never costs the CPU anything.
 *
 *   - No FPU and no threads, so there is no audio thread and no timer thread.
 *     The audio callback is pumped inline from retro_run(), which is legal
 *     because SDL_OpenAudioDevice() is what arms it and the ring buffer is
 *     drained by the frontend. SDL_CreateThread() fails, exactly like the SDL
 *     1.2 build shipped for these boards does.
 *
 *   - Input is a digital pad with no analog sticks, so there is no real cursor
 *     and no real keyboard. Events are synthesised by sf2000_input.c and pushed
 *     into the queue here; eduke32's own binding layer then sees ordinary
 *     scancodes and mouse motion, which is what makes rebinding work.
 */

/* SDL2 declares SDL_Cursor, SDL_Renderer and SDL_atomic_t as opaque. eduke32
 * only ever handles those as pointers -- it dereferences SDL_Surface and
 * SDL_PixelFormat, which SDL2 does publish -- so rename them here and give this
 * file real definitions to allocate. */
#define SDL_Cursor     sf2k_Cursor
#define SDL_Renderer   sf2k_Renderer
#define SDL_atomic_t   sf2k_atomic_t

#include "sdl2_shim.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct sf2k_Cursor  { int unused; } sf2k_Cursor;
typedef struct sf2k_Renderer{ int unused; } sf2k_Renderer;

/* ------------------------------------------------------------------ state */

#define SF2K_MAX_WINDOWS     4
#define SF2K_EVENT_QUEUE     64
#define SF2K_AUDIO_RING      8192   /* stereo frames */

typedef struct
{
    int             used;
    int             w, h;
    Uint16         *pixels;        /* RGB565, w*h, the surface and the frame */
    int             pitch_in_px;
    SDL_Surface     surface;       /* the window surface handed to BUILD      */
    SDL_PixelFormat fmt;
} sf2k_window;

static sf2k_window          s_windows[SF2K_MAX_WINDOWS];
static SDL_Event            s_events[SF2K_EVENT_QUEUE];
static int                  s_event_head, s_event_tail;
static Uint8                s_keystate[SDL_NUM_SCANCODES];

static int                  s_initialized;
static Uint32               s_initialized_subsystems;
static Uint32               s_quit_requested;

static char                 s_error[256];

static int                  s_mouse_x, s_mouse_y;
static int                  s_mouse_dx, s_mouse_dy;
static int                  s_mouse_buttons;
static SDL_bool             s_mouse_relative;
static SDL_bool             s_cursor_visible = SDL_TRUE;

/* audio */
static int16_t              s_audio_ring[SF2K_AUDIO_RING * 2];
static int                  s_audio_head, s_audio_tail;
static SDL_AudioDeviceID    s_audio_device;
static SDL_AudioSpec        s_audio_spec;
static SDL_AudioCallback    s_audio_callback;
static void                *s_audio_userdata;
static int                  s_audio_running;
static Uint32               s_audio_underruns;

static char                *s_clipboard;

/* Backing store for SDL_RWFromConstMem. */
typedef struct
{
    const Uint8 *base;
    Sint64       size;
    Sint64       pos;
} RWops_mem;

static void ring_push(const int16_t *src, int frames);
static int  ring_pop(int16_t *dst, int frames);

/* ------------------------------------------------------------------ misc */

const char *SDL_GetError(void)
{
    return s_error;
}

int SDL_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
    return -1;
}

void SDL_ClearError(void)
{
    s_error[0] = '\0';
}

void *SDL_malloc(size_t size)
{
    /* The console's allocator is plain newlib malloc. Keep the zero-fill
     * guarantee: several SDL structures are only partly initialised and the
     * rest of the library assumes the unused fields are zero. */
    void *p = calloc(1, size ? size : 1);
    return p;
}

void SDL_free(void *p)
{
    free(p);
}

void *SDL_calloc(size_t n, size_t size)
{
    return calloc(n ? n : 1, size ? size : 1);
}

void *SDL_realloc(void *p, size_t size)
{
    return realloc(p, size ? size : 1);
}

void SDL_GetVersion(SDL_version *v)
{
    if (v) {
        SDL_version ver = { SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL };
        *v = ver;
    }
}

const char *SDL_GetRevision(void)
{
    return "sf2000-shim";
}


Uint32 SDL_GetTicks(void)
{
    return (Uint32)sf2k_ticks_ms();
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)sf2k_ticks_us();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return 1000000;
}

void SDL_Delay(Uint32 ms)
{
    sf2k_delay_ms(ms);
}

char *SDL_getenv(const char *name)
{
    return getenv(name);
}

int SDL_setenv(const char *name, const char *value, int overwrite)
{
    (void)name; (void)value; (void)overwrite;
    return -1;
}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
    /* Nothing here is a real X11/Windows hint. In particular the console has no
     * signal handlers worth installing and no XINPUT, so both are accepted and
     * ignored rather than being rejected, which would make eduke32's capability
     * probing think the platform is broken. */
    (void)name; (void)value;
    return SDL_TRUE;
}

SDL_bool SDL_GetHintBoolean(const char *name, SDL_bool default_value)
{
    (void)name;
    return default_value;
}

int SDL_InitSubSystem(Uint32 flags)
{
    if (!s_initialized) {
        memset(s_keystate, 0, sizeof(s_keystate));
        s_event_head = s_event_tail = 0;
        s_audio_head = s_audio_tail = 0;
        s_initialized = 1;
    }
    s_initialized_subsystems |= flags;
    return 0;
}

int SDL_Init(Uint32 flags)
{
    int r = SDL_InitSubSystem(flags);
    if (r == 0)
        s_initialized_subsystems |= SDL_INIT_NOPARACHUTE;
    return r;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    s_initialized_subsystems &= ~flags;
    if (!s_initialized_subsystems)
        s_initialized = 0;
}

void SDL_Quit(void)
{
    int i;

    SDL_CloseAudio();

    for (i = 0; i < SF2K_MAX_WINDOWS; i++) {
        if (s_windows[i].used) {
            free(s_windows[i].pixels);
            s_windows[i].used = 0;
        }
    }
    s_initialized = 0;
    s_initialized_subsystems = 0;
    free(s_clipboard);
    s_clipboard = NULL;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    if (!flags)
        return s_initialized_subsystems;
    return s_initialized_subsystems & flags;
}

const char *SDL_GetCurrentVideoDriver(void)
{
    return "sf2000";
}

const char *SDL_GetCurrentAudioDriver(void)
{
    return "sf2000";
}

int SDL_GetNumVideoDrivers(void) { return 1; }
const char *SDL_GetVideoDriver(int i) { return (i == 0) ? "sf2000" : NULL; }
int SDL_GetNumAudioDrivers(void) { return 1; }
const char *SDL_GetAudioDriver(int i) { return (i == 0) ? "sf2000" : NULL; }

/* -------------------------------------------------------------- events */

int SDL_PushEvent(SDL_Event *ev)
{
    int next = (s_event_tail + 1) % SF2K_EVENT_QUEUE;
    if (next == s_event_head)
        return 0;                   /* full: drop, the queue is a frame deep */
    s_events[s_event_tail] = *ev;
    s_event_tail = next;
    return 1;
}

int SDL_PollEvent(SDL_Event *ev)
{
    if (s_event_head == s_event_tail)
        return 0;
    if (ev) {
        *ev = s_events[s_event_head];
    }
    s_event_head = (s_event_head + 1) % SF2K_EVENT_QUEUE;
    return 1;
}

int SDL_PeepEvents(SDL_Event *events, int numevents, SDL_eventaction action,
                   Uint32 minType, Uint32 maxType)
{
    int count = 0;

    if (action == SDL_PEEKEVENT) {
        int i = s_event_head;
        while (i != s_event_tail && count < numevents) {
            Uint32 t = s_events[i].type;
            if (t >= minType && t <= maxType) {
                if (events)
                    events[count] = s_events[i];
                count++;
            }
            i = (i + 1) % SF2K_EVENT_QUEUE;
        }
        return count;
    }

    if (action == SDL_GETEVENT) {
        int i = s_event_head;
        while (i != s_event_tail && count < numevents) {
            Uint32 t = s_events[i].type;
            if (t >= minType && t <= maxType) {
                if (events)
                    events[count] = s_events[i];
                s_event_head = (i + 1) % SF2K_EVENT_QUEUE;
                count++;
                i = s_event_head;
            } else {
                i = (i + 1) % SF2K_EVENT_QUEUE;
            }
        }
        return count;
    }

    return -1;
}

void SDL_FlushEvent(Uint32 minType)
{
    int i = s_event_head;
    while (i != s_event_tail) {
        Uint32 t = s_events[i].type;
        if (t != minType) {
            int j = (i + 1) % SF2K_EVENT_QUEUE;
            if (i == s_event_head)
                s_event_head = j;
            else
                s_events[i] = s_events[j];
            continue;
        }
        i = (i + 1) % SF2K_EVENT_QUEUE;
    }
}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    (void)maxType;
    SDL_FlushEvent(minType);
}

void SDL_PumpEvents(void)
{
    /* The pad is sampled once per frame by retro_run() rather than from here.
     * Doing it in SDL_PumpEvents would make it re-entrant: eduke32's event loop
     * is `while (SDL_PollEvent(&ev))`, so a pump inside the poll would generate
     * events while the loop that is draining them is still running. */
}

int SDL_WaitEventTimeout(SDL_Event *ev, int timeout)
{
    Uint32 deadline = SDL_GetTicks() + (timeout > 0 ? (Uint32)timeout : 0);

    for (;;) {
        SDL_PumpEvents();
        if (SDL_PollEvent(ev))
            return 1;
        if (timeout == 0)
            return 0;
        if (timeout > 0 && (int)(SDL_GetTicks() - deadline) >= 0)
            return 0;
        SDL_Delay(1);
    }
}

int SDL_WaitEvent(SDL_Event *ev)
{
    return SDL_WaitEventTimeout(ev, -1);
}


Uint8 SDL_EventState(Uint32 type, int state)
{
    (void)type;
    (void)state;
    return SDL_ENABLE;
}


void SDL_StartTextInput(void) { }
SDL_bool SDL_IsTextInputActive(void) { return SDL_FALSE; }
void SDL_StopTextInput(void) {}
SDL_bool SDL_HasScreenKeyboardSupport(void) { return SDL_FALSE; }

/* ------------------------------------------------------------ keyboard */

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
    if (numkeys)
        *numkeys = SDL_NUM_SCANCODES;
    return s_keystate;
}

int sf2k_sdl_send_key(SDL_Scancode scancode, int pressed)
{
    Uint8 want;

    if (scancode >= SDL_NUM_SCANCODES)
        return 0;

    want = (Uint8)(pressed ? SDL_PRESSED : SDL_RELEASED);

    /* Only report edges. The input layer asks about every key every frame, so
     * without this the queue fills with a KEYUP per key per frame -- 24 of them
     * -- and wraps before the game ever drains the KEYDOWN behind them. */
    if (s_keystate[scancode] == want)
        return 0;

    s_keystate[scancode] = want;

    {
        SDL_Event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.timestamp = SDL_GetTicks();
        ev.key.windowID = 1;
        ev.key.state = (Uint8)(pressed ? SDL_PRESSED : SDL_RELEASED);
        ev.key.repeat = 0;
        ev.key.keysym.scancode = scancode;
        ev.key.keysym.sym = (SDL_Keycode)SDL_SCANCODE_TO_KEYCODE(scancode);
        ev.key.keysym.mod = KMOD_NONE;
        return SDL_PushEvent(&ev);
    }
}

SDL_bool SDL_EnableUNICODE(SDL_bool enable)
{
    (void)enable;
    return SDL_TRUE;
}

SDL_bool SDL_EnableKeyRepeat(int delay, int interval)
{
    (void)delay; (void)interval;
    return SDL_TRUE;
}

const char *SDL_GetScancodeName(SDL_Scancode code)
{
    (void)code;
    return "";
}

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key)
{
    (void)key;
    return SDL_SCANCODE_UNKNOWN;
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode)
{
    return (SDL_Keycode)SDL_SCANCODE_TO_KEYCODE(scancode);
}

/* --------------------------------------------------------------- mouse */

void sf2k_sdl_set_mouse(int x, int y, int dx, int dy, unsigned buttons, int relative)
{
    s_mouse_x = x;
    s_mouse_y = y;
    s_mouse_dx = dx;
    s_mouse_dy = dy;
    s_mouse_buttons = (int)buttons;
    s_mouse_relative = relative ? SDL_TRUE : SDL_FALSE;
}

Uint32 SDL_GetMouseState(int *x, int *y)
{
    if (x) *x = s_mouse_x;
    if (y) *y = s_mouse_y;
    return (unsigned)s_mouse_buttons;
}

SDL_Window *SDL_GetMouseFocus(void)
{
    return (SDL_Window *)(uintptr_t)1;
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    if (x) *x = s_mouse_dx;
    if (y) *y = s_mouse_dy;
    return s_mouse_buttons;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    s_mouse_relative = enabled;
    return SDL_TRUE;
}

SDL_bool SDL_GetRelativeMouseMode(void)
{
    return s_mouse_relative;
}

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
    (void)window;
    s_mouse_x = x;
    s_mouse_y = y;
}

int SDL_WarpMouseGlobal(int x, int y)
{
    s_mouse_x = x;
    s_mouse_y = y;
    return 0;
}

Uint32 SDL_GetGlobalMouseState(int *x, int *y)
{
    if (x) *x = s_mouse_x;
    if (y) *y = s_mouse_y;
    return (Uint32)s_mouse_buttons;
}

int SDL_CaptureMouse(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

int SDL_ShowCursor(int toggle)
{
    if (toggle == SDL_ENABLE || toggle == SDL_QUERY)
        return s_cursor_visible ? 1 : 0;
    s_cursor_visible = (toggle == SDL_DISABLE) ? SDL_FALSE : SDL_TRUE;
    return s_cursor_visible ? 1 : 0;
}

SDL_bool SDL_GetCursorDisplayState(void)
{
    return s_cursor_visible;
}

/* Cursors: the console draws the crosshair itself, so every system cursor is
 * the same empty object. Only its identity matters, because eduke32 compares
 * cursors to decide whether to restore the one it saved. */
static sf2k_Cursor s_cursor_storage[8];

sf2k_Cursor *SDL_CreateSystemCursor(SDL_SystemCursor id)
{
    return &s_cursor_storage[(id < 8) ? id : 0];
}

void SDL_DestroySystemCursor(sf2k_Cursor *cur) { (void)cur; }

sf2k_Cursor *SDL_CreateCursor(const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y)
{
    (void)data; (void)mask; (void)w; (void)h; (void)hot_x; (void)hot_y;
    return &s_cursor_storage[0];
}

sf2k_Cursor *SDL_CreateCursorFromBitmap(const Uint8 *data, const Uint8 *mask, int w, int h, int hot_x, int hot_y)
{
    return SDL_CreateCursor(data, mask, w, h, hot_x, hot_y);
}

void SDL_DestroyCursor(sf2k_Cursor *cur) { (void)cur; }
void SDL_SetCursor(sf2k_Cursor *cur) { (void)cur; }
sf2k_Cursor *SDL_GetCursor(void) { return &s_cursor_storage[0]; }
sf2k_Cursor *SDL_GetDefaultCursor(void) { return &s_cursor_storage[0]; }

/* ------------------------------------------------------------- windows */

static sf2k_window *window_alloc(int w, int h)
{
    int i;
    sf2k_window *win;

    for (i = 0; i < SF2K_MAX_WINDOWS; i++)
        if (!s_windows[i].used)
            break;
    if (i == SF2K_MAX_WINDOWS)
        return NULL;

    win = &s_windows[i];
    memset(win, 0, sizeof(*win));
    win->pixels = (Uint16 *)SDL_malloc((size_t)w * h * sizeof(Uint16));
    if (!win->pixels)
        return NULL;

    win->used = 1;
    win->w = w;
    win->h = h;
    win->pitch_in_px = w;

    /* RGB565 little-endian, which is what the multicore frontend wants. */
    win->fmt.format = SDL_PIXELFORMAT_RGB565;
    win->fmt.palette = NULL;
    win->fmt.BitsPerPixel = 16;
    win->fmt.BytesPerPixel = 2;
    win->fmt.Rmask = 0xF800;
    win->fmt.Gmask = 0x07E0;
    win->fmt.Bmask = 0x001F;
    win->fmt.Amask = 0;
    win->fmt.Rloss = 3;
    win->fmt.Gloss = 2;
    win->fmt.Bloss = 3;
    win->fmt.Aloss = 8;
    win->fmt.Rshift = 11;
    win->fmt.Gshift = 5;
    win->fmt.Bshift = 0;
    win->fmt.Ashift = 0;
    win->fmt.refcount = 1;
    win->fmt.next = NULL;

    win->surface.format = &win->fmt;
    win->surface.flags = SDL_PREALLOC;
    win->surface.w = w;
    win->surface.h = h;
    win->surface.pitch = w * (int)sizeof(Uint16);
    win->surface.pixels = win->pixels;
    win->surface.userdata = NULL;
    win->surface.locked = 0;
    win->surface.list_blitmap = NULL;
    win->surface.map = NULL;
    win->surface.refcount = 1;
    SDL_memset(&win->surface.clip_rect, 0, sizeof(win->surface.clip_rect));

    return win;
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    sf2k_window *win;
    win = window_alloc(w, h);
    (void)title; (void)x; (void)y;
    if (!win)
        return NULL;
    /* Only SDL_WINDOW_OPENGL would need more than a software surface, and we
     * build with USE_OPENGL=0. */
    (void)flags;
    return (SDL_Window *)(uintptr_t)(win - s_windows + 1);
}

SDL_Surface *SDL_GetWindowSurface(SDL_Window *window)
{
    sf2k_window *win;
    if (!window)
        return NULL;
    win = &s_windows[(uintptr_t)window - 1];
    if (!win->used)
        return NULL;
    return &win->surface;
}

int SDL_UpdateWindowSurface(SDL_Window *window)
{
    sf2k_window *win;
    if (!window)
        return -1;
    win = &s_windows[(uintptr_t)window - 1];
    if (!win->used)
        return -1;

    /* Present. The frontend scales to the panel, so hand over the raw surface. */
    sf2k_present(win->pixels, win->w, win->h, win->surface.pitch);
    return 0;
}

int SDL_UpdateWindowSurfaceRects(SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)rects; (void)numrects;
    return SDL_UpdateWindowSurface(window);
}

void SDL_DestroyWindow(SDL_Window *window)
{
    sf2k_window *win;
    if (!window)
        return;
    win = &s_windows[(uintptr_t)window - 1];
    if (!win->used)
        return;
    free(win->pixels);
    memset(win, 0, sizeof(*win));
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    sf2k_window *win = &s_windows[(uintptr_t)window - 1];
    if (w) *w = win->used ? win->w : 0;
    if (h) *h = win->used ? win->h : 0;
}

void SDL_SetWindowSize(SDL_Window *window, int w, int h) { (void)window; (void)w; (void)h; }
void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y) { (void)window; if (x) *x = 0; if (y) *y = 0; }
void SDL_SetWindowPosition(SDL_Window *window, int x, int y) { (void)window; (void)x; (void)y; }
void SDL_SetWindowTitle(SDL_Window *window, const char *title) { (void)window; (void)title; }
Uint32 SDL_GetWindowFlags(SDL_Window *window) { (void)window; return SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN; }
Uint32 SDL_GetWindowID(SDL_Window *window) { (void)window; return 1; }
int SDL_GetWindowDisplayIndex(SDL_Window *window) { (void)window; return 0; }

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
    (void)window; (void)flags;
    return 0;
}

void SDL_SetWindowBordered(SDL_Window *window, SDL_bool bordered)
{
    (void)window; (void)bordered;
}

void SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed) { (void)window; (void)grabbed; }
int SDL_SetWindowBrightness(SDL_Window *window, float b) { (void)window; (void)b; return 0; }
float SDL_GetWindowBrightness(SDL_Window *window) { (void)window; return 1.0f; }
void SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) { (void)window; (void)icon; }
int SDL_SetWindowModalFor(SDL_Window *w, SDL_Window *o) { (void)w; (void)o; return 0; }
int SDL_SetWindowInputFocus(SDL_Window *w) { (void)w; return 0; }
SDL_Window *SDL_GetKeyboardFocus(void) { return (SDL_Window *)(uintptr_t)1; }
SDL_Window *SDL_GetWindowFromID(Uint32 id) { (void)id; return (SDL_Window *)(uintptr_t)1; }

int SDL_SetWindowGammaRamp(SDL_Window *window, const Uint16 *r, const Uint16 *g, const Uint16 *b)
{
    (void)window; (void)r; (void)g; (void)b;
    return 0;
}

int SDL_GetWindowGammaRamp(SDL_Window *window, Uint16 *r, Uint16 *g, Uint16 *b)
{
    int i;
    for (i = 0; i < 256; i++) {
        Uint16 v = (Uint16)(i * 257);
        if (r) r[i] = v;
        if (g) g[i] = v;
        if (b) b[i] = v;
    }
    (void)window;
    return 0;
}

int SDL_SetWindowDisplayMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
    (void)window; (void)mode;
    return 0;
}

int SDL_GetWindowDisplayMode(SDL_Window *window, SDL_DisplayMode *mode)
{
    sf2k_window *win = &s_windows[(uintptr_t)window - 1];
    if (mode) {
        mode->format = SDL_PIXELFORMAT_RGB565;
        mode->w = win->used ? win->w : 320;
        mode->h = win->used ? win->h : 200;
        mode->refresh_rate = 60;
        mode->driverdata = NULL;
    }
    return 0;
}

int SDL_GetWindowSystemInfo(SDL_Window *w, SDL_SysWMinfo *info)
{
    (void)w;
    if (info) {
        memset(info, 0, sizeof(*info));
        SDL_VERSION(&info->version);
        info->subsystem = SDL_SYSWM_UNKNOWN;
    }
    return 0;
}

int SDL_GetDisplayDPI(int d, float *ddpi, float *hdpi, float *vdpi)
{
    (void)d;
    if (ddpi) *ddpi = 72.0f;
    if (hdpi) *hdpi = 72.0f;
    if (vdpi) *vdpi = 72.0f;
    return 0;
}

/* ------------------------------------------------------ display modes */

int SDL_GetNumVideoDisplays(void) { return 1; }
int SDL_GetNumDisplayModes(int d) { (void)d; return 1; }

int SDL_GetDisplayMode(int d, int m, SDL_DisplayMode *mode)
{
    (void)d; (void)m;
    if (mode) {
        mode->format = SDL_PIXELFORMAT_RGB565;
        mode->w = sf2k_present_width();
        mode->h = sf2k_present_height();
        mode->refresh_rate = 60;
        mode->driverdata = NULL;
    }
    return 0;
}

int SDL_GetDesktopDisplayMode(int d, SDL_DisplayMode *mode) { return SDL_GetDisplayMode(d, 0, mode); }
int SDL_GetCurrentDisplayMode(int d, SDL_DisplayMode *mode) { return SDL_GetDisplayMode(d, 0, mode); }

SDL_DisplayMode *SDL_GetClosestDisplayMode(int d, const SDL_DisplayMode *want, SDL_DisplayMode *closest)
{
    (void)d; (void)want;
    if (closest)
        SDL_GetDisplayMode(0, 0, closest);
    return closest;
}

const char *SDL_GetDisplayName(int d) { (void)d; return "sf2000"; }

/* --------------------------------------------------------- surfaces */

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format)
{
    SDL_Surface *s = (SDL_Surface *)SDL_malloc(sizeof(SDL_Surface));
    (void)flags; (void)depth; (void)format;
    if (!s)
        return NULL;
    s->flags = SDL_PREALLOC;
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    s->pixels = SDL_malloc((size_t)s->pitch * h);
    return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    SDL_Surface *s = (SDL_Surface *)SDL_malloc(sizeof(SDL_Surface));
    if (!s)
        return NULL;
    s->flags = SDL_PREALLOC;
    s->w = w;
    s->h = h;
    s->pitch = w * 4;
    s->pixels = SDL_malloc((size_t)s->pitch * h);
    (void)flags; (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return s;
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
                                      Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    SDL_Surface *s = (SDL_Surface *)SDL_malloc(sizeof(SDL_Surface));
    if (!s)
        return NULL;
    s->flags = SDL_PREALLOC;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->pixels = pixels;
    (void)depth; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (!s)
        return;
    if (!(s->flags & SDL_PREALLOC) && s->pixels)
        SDL_free(s->pixels);
    if (s->format && s->format->refcount > 0)
        s->format->refcount = 0;
    SDL_free(s);
}

int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

int SDL_SetSurfaceBlendMode(SDL_Surface *s, SDL_BlendMode m) { (void)s; (void)m; return 0; }
int SDL_GetSurfaceBlendMode(SDL_Surface *s, SDL_BlendMode *m) { if (m) *m = SDL_BLENDMODE_NONE; (void)s; return 0; }
int SDL_SetSurfaceColorMod(SDL_Surface *s, Uint8 r, Uint8 g, Uint8 b) { (void)s; (void)r; (void)g; (void)b; return 0; }
int SDL_SetSurfaceAlphaMod(SDL_Surface *s, Uint8 a) { (void)s; (void)a; return 0; }

SDL_bool SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r) { (void)s; (void)r; return SDL_TRUE; }
void SDL_GetClipRect(SDL_Surface *s, SDL_Rect *r) { (void)s; if (r) SDL_memset(r, 0, sizeof(*r)); }

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, const SDL_PixelFormat *fmt, Uint32 flags)
{
    (void)fmt; (void)flags;
    return src;
}

SDL_Surface *SDL_ConvertSurfaceFormat(SDL_Surface *src, Uint32 fmt, Uint32 flags)
{
    (void)fmt; (void)flags;
    return src;
}

int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    (void)src; (void)sr; (void)dst; (void)dr;
    return 0;
}

int SDL_FillRect(SDL_Surface *dst, const SDL_Rect *r, Uint32 color)
{
    (void)dst; (void)r; (void)color;
    return 0;
}

Uint32 SDL_MapRGB(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b)
{
    if (f && f->BitsPerPixel == 16)
        return (Uint32)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return (Uint32)((r << 16) | (g << 8) | b);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)a;
    return SDL_MapRGB(f, r, g, b);
}

void SDL_GetRGB(Uint32 p, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b)
{
    Uint8 R = (Uint8)((p >> (f ? f->Rshift : 16)) & 0xFF);
    Uint8 G = (Uint8)((p >> (f ? f->Gshift : 8)) & 0xFF);
    Uint8 B = (Uint8)((p >> (f ? f->Bshift : 0)) & 0xFF);
    if (f && f->BitsPerPixel == 16) { R = (Uint8)(R << 3 | R >> 5); G = (Uint8)(G << 2 | G >> 6); B = (Uint8)(B << 3 | B >> 5); }
    if (r) *r = R;
    if (g) *g = G;
    if (b) *b = B;
}

void SDL_GetRGBA(Uint32 p, const SDL_PixelFormat *f, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a)
{
    SDL_GetRGB(p, f, r, g, b);
    if (a) *a = 255;
}

SDL_Surface *SDL_DuplicateSurface(SDL_Surface *s) { (void)s; return NULL; }
SDL_bool SDL_IsScreenSaverEnabled(void) { return SDL_FALSE; }
void SDL_EnableScreenSaver(void) {}
void SDL_DisableScreenSaver(void) {}

/* ---------------------------------------------------------- renderer */

/* eduke32's imgui backend is compiled in, but only the console/menu path can
 * reach it and that path is SDL2-only. There is no GPU, so the renderer is a
 * no-op object that the backend can talk to without crashing. imgui draws its
 * windows with its own vertex buffers, which the backend uploads through
 * SDL_Renderer's data path -- not implemented -- so anything drawn there is
 * simply absent rather than fatal. */
static sf2k_Renderer s_renderer_storage;

sf2k_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    (void)window; (void)index; (void)flags;
    memset(&s_renderer_storage, 0, sizeof(s_renderer_storage));
    return &s_renderer_storage;
}

void SDL_DestroyRenderer(sf2k_Renderer *r) { (void)r; }
int SDL_RenderSetLogicalSize(sf2k_Renderer *r, int w, int h) { (void)r; (void)w; (void)h; return 0; }
int SDL_RenderSetViewport(sf2k_Renderer *r, const SDL_Rect *v) { (void)r; (void)v; return 0; }
void SDL_RenderGetViewport(sf2k_Renderer *r, SDL_Rect *v) { (void)r; if (v) SDL_memset(v, 0, sizeof(*v)); }
int SDL_RenderSetScale(sf2k_Renderer *r, float x, float y) { (void)r; (void)x; (void)y; return 0; }
void SDL_RenderGetScale(sf2k_Renderer *r, float *x, float *y) { (void)r; if (x) *x = 1.f; if (y) *y = 1.f; }
int SDL_RenderSetVSync(sf2k_Renderer *r, int vsync) { (void)r; (void)vsync; return 0; }
int SDL_GetRendererOutputSize(sf2k_Renderer *r, int *w, int *h) { (void)r; if (w) *w = sf2k_present_width(); if (h) *h = sf2k_present_height(); return 0; }
int SDL_GetRendererInfo(sf2k_Renderer *r, SDL_RendererInfo *i) { (void)r; if (i) memset(i, 0, sizeof(*i)); return 0; }

int SDL_RenderClear(sf2k_Renderer *r) { (void)r; return 0; }
void SDL_RenderPresent(sf2k_Renderer *r) { (void)r; }
int SDL_RenderCopy(sf2k_Renderer *r, SDL_Texture *t, const SDL_Rect *sr, const SDL_Rect *dr) { (void)r; (void)t; (void)sr; (void)dr; return 0; }
int SDL_RenderCopyEx(sf2k_Renderer *r, SDL_Texture *t, const SDL_Rect *sr, const SDL_Rect *dr, double a, const SDL_Point *c, SDL_RendererFlip f) { (void)r; (void)t; (void)sr; (void)dr; (void)a; (void)c; (void)f; return 0; }
int SDL_RenderFillRect(sf2k_Renderer *r, const SDL_Rect *rc) { (void)r; (void)rc; return 0; }
int SDL_RenderDrawRect(sf2k_Renderer *r, const SDL_Rect *rc) { (void)r; (void)rc; return 0; }
int SDL_RenderDrawLine(sf2k_Renderer *r, int x1, int y1, int x2, int y2) { (void)r; (void)x1; (void)y1; (void)x2; (void)y2; return 0; }
int SDL_RenderReadPixels(sf2k_Renderer *r, const SDL_Rect *rc, Uint32 f, void *px, int pitch) { (void)r; (void)rc; (void)f; (void)px; (void)pitch; return -1; }
SDL_Texture *SDL_CreateTextureFromSurface(sf2k_Renderer *r, SDL_Surface *s) { (void)r; (void)s; return NULL; }
void SDL_DestroyTexture(SDL_Texture *t) { (void)t; }
int SDL_QueryTexture(SDL_Texture *t, Uint32 *f, int *a, int *w, int *h) { (void)t; if (f) *f = 0; if (a) *a = 0; if (w) *w = 0; if (h) *h = 0; return -1; }
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch) { (void)t; (void)r; (void)p; (void)pitch; return -1; }
int SDL_LockTexture(SDL_Texture *t, const SDL_Rect *r, void **p, int *pitch) { (void)t; (void)r; if (p) *p = NULL; if (pitch) *pitch = 0; return -1; }
void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }
int SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode m) { (void)t; (void)m; return 0; }
int SDL_SetTextureColorMod(SDL_Texture *t, Uint8 r, Uint8 g, Uint8 b) { (void)t; (void)r; (void)g; (void)b; return 0; }
int SDL_SetTextureAlphaMod(SDL_Texture *t, Uint8 a) { (void)t; (void)a; return 0; }

/* --------------------------------------------------------- joystick */

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int i) { (void)i; return NULL; }
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
const char *SDL_JoystickName(SDL_Joystick *j) { (void)j; return "sf2000 pad"; }
const char *SDL_JoystickNameForIndex(int i) { (void)i; return "sf2000 pad"; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumBalls(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 0; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *j) { (void)j; return -1; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a) { (void)j; (void)a; return 0; }
int SDL_JoystickGetBall(SDL_Joystick *j, int b, int *x, int *y) { (void)j; (void)b; if (x) *x = 0; if (y) *y = 0; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b) { (void)j; (void)b; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int h) { (void)j; (void)h; return 0; }
int SDL_JoystickEventState(int s) { (void)s; return 0; }

SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int i) { (void)i; return NULL; }
void SDL_GameControllerClose(SDL_GameController *g) { (void)g; }
const char *SDL_GameControllerName(SDL_GameController *g) { (void)g; return "sf2000 pad"; }
const char *SDL_GameControllerNameForIndex(int i) { (void)i; return "sf2000 pad"; }
int SDL_GameControllerNumAxes(SDL_GameController *g) { (void)g; return 0; }
int SDL_GameControllerNumButtons(SDL_GameController *g) { (void)g; return 0; }
int SDL_GameControllerNumTouchpads(SDL_GameController *g) { (void)g; return 0; }
int SDL_GameControllerNumSensorPorts(SDL_GameController *g, SDL_SensorType t) { (void)g; (void)t; return 0; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *g, int a) { (void)g; (void)a; return 0; }
Uint8 SDL_GameControllerGetButton(SDL_GameController *g, int b) { (void)g; (void)b; return 0; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *g) { (void)g; return NULL; }
SDL_bool SDL_GameControllerHasSensor(SDL_GameController *g, SDL_SensorType t) { (void)g; (void)t; return SDL_FALSE; }
SDL_bool SDL_GameControllerHasRumble(SDL_GameController *g) { (void)g; return SDL_FALSE; }
int SDL_GameControllerRumble(SDL_GameController *g, Uint16 a, Uint16 b, Uint32 t) { (void)g; (void)a; (void)b; (void)t; return -1; }
int SDL_GameControllerRumbleTriggers(SDL_GameController *g, Uint16 a, Uint16 b, Uint32 t) { (void)g; (void)a; (void)b; (void)t; return -1; }
int SDL_GameControllerAddMappingsFromRW(SDL_RWops *r, int free) { (void)r; (void)free; return 0; }
int SDL_GameControllerAddMapping(const char *m) { (void)m; return -1; }
const char *SDL_GameControllerGetSerial(SDL_GameController *g) { (void)g; return ""; }
char *SDL_GameControllerMapping(SDL_GameController *g) { (void)g; return NULL; }

/* ------------------------------------------------------------- audio */

/* No audio thread exists. The callback is invoked from sf2k_audio_pump(), which
 * the core calls once per frame from retro_run(); the frontend drains the ring
 * through audio_sample_batch. */
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *dev, int iscapture, const SDL_AudioSpec *want,
                                    SDL_AudioSpec *got, int allowed)
{
    /* SDL2 carries the callback in SDL_AudioSpec rather than as an argument. */
    s_audio_callback = want ? want->callback : NULL;
    s_audio_userdata = want ? want->userdata : NULL;
    s_audio_running = 1;
    s_audio_head = s_audio_tail = 0;
    s_audio_underruns = 0;

    if (want)
        s_audio_spec = *want;
    else
        memset(&s_audio_spec, 0, sizeof(s_audio_spec));

    if (got)
        *got = s_audio_spec;
    (void)dev; (void)iscapture; (void)allowed;

    s_audio_device = 1;
    return s_audio_device;
}

int SDL_OpenAudio(SDL_AudioSpec *want, SDL_AudioSpec *got)
{
    /* Legacy entry point: eduke32's audiolib uses it with a mixing callback.
     * SDL 1.2's callback shape is void(*)(void*, Uint8*, int), which is set up
     * through SDL_Init/SDL_Quit rather than passed here. */
    if (want)
        s_audio_spec = *want;
    if (got)
        *got = s_audio_spec;
    s_audio_callback = NULL;
    s_audio_running = 1;
    s_audio_device = 1;
    return 1;
}

void SDL_CloseAudio(void)
{
    SDL_CloseAudioDevice(s_audio_device);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    (void)dev;
    s_audio_running = 0;
    s_audio_device = 0;
    s_audio_callback = NULL;
}

void SDL_PauseAudio(int p) { (void)p; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int p) { (void)d; (void)p; }
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
void SDL_LockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID d) { (void)d; }

SDL_AudioDeviceID SDL_GetOpenAudioDevice(void) { return s_audio_device; }
const char *SDL_GetAudioDeviceName(int i, int iscapture) { (void)i; (void)iscapture; return "sf2000"; }
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID d) { (void)d; return SDL_AUDIO_PLAYING; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d) { (void)d; return 0; }
int SDL_GetAlsaAudioDeviceStatus(int d) { (void)d; return 0; }

SDL_AudioStatus SDL_GetAudioStatus(void)
{
    return s_audio_running ? SDL_AUDIO_PLAYING : SDL_AUDIO_STOPPED;
}

/* Fills `len` frames from the ring, running the callback to make more first. */
void sf2k_audio_pump(void *stream, int len)
{
    Uint8 *out = (Uint8 *)stream;
    int produced = 0;

    if (s_audio_callback) {
        /* The callback wants `len` bytes in the device format; drive it once per
         * frame for a fixed 20 ms block and keep whatever it produced. */
        Uint8 block[4096];
        int want = s_audio_spec.samples ? (int)s_audio_spec.samples * 2 : 1024;
        int bytes = want * (int)sizeof(int16_t);
        if (bytes > (int)sizeof(block))
            bytes = (int)sizeof(block);
        memset(block, 0, (size_t)bytes);
        s_audio_callback(s_audio_userdata, block, bytes);
        ring_push((const int16_t *)block, bytes / (int)sizeof(int16_t));
    }

    produced = ring_pop((int16_t *)out, len);
    if (produced < len)
        memset(out + produced * 2, 0, (size_t)(len - produced) * 2);
}

int sf2k_audio_available(void)
{
    return SF2K_AUDIO_RING;
}

/* -------------------------------------------------------------- threads */

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
{
    (void)fn; (void)name; (void)data;
    /* The console has no threads. Returning NULL is what the SDL 1.2 build for
     * these boards does, and eduke32 already copes with a failed thread. */
    SDL_SetError("Threads are not supported on this platform");
    return NULL;
}

void SDL_DetachThread(SDL_Thread *t) { (void)t; }
void SDL_WaitThread(SDL_Thread *t, int *status) { (void)t; if (status) *status = -1; }
SDL_threadID SDL_GetThreadID(SDL_Thread *thread) { return 0; }
SDL_threadID SDL_ThreadID(void) { return 0; }
int SDL_ThreadRun(void) { return 0; }
int SDL_SetThreadPriority(SDL_ThreadPriority p) { (void)p; return 0; }

SDL_mutex *SDL_CreateMutex(void) { return (SDL_mutex *)SDL_malloc(64); }
int SDL_LockMutex(SDL_mutex *m) { (void)m; return 0; }
int SDL_TryLockMutex(SDL_mutex *m) { (void)m; return 0; }
int SDL_UnlockMutex(SDL_mutex *m) { (void)m; return 0; }
void SDL_DestroyMutex(SDL_mutex *m) { if (m) SDL_free(m); }

SDL_sem *SDL_CreateSemaphore(Uint32 v) { return (SDL_sem *)(uintptr_t)v; }
int SDL_SemWait(SDL_sem *s) { (void)s; return 0; }
int SDL_SemTryWait(SDL_sem *s) { (void)s; return 0; }
int SDL_SemWaitTimeout(SDL_sem *s, Uint32 t) { (void)s; (void)t; return 0; }
int SDL_SemPost(SDL_sem *s) { (void)s; return 0; }
void SDL_DestroySemaphore(SDL_sem *s) { (void)s; }
Uint32 SDL_SemValue(SDL_sem *s) { return (Uint32)(uintptr_t)s; }

SDL_cond *SDL_CreateCond(void) { return (SDL_cond *)SDL_malloc(64); }
int SDL_CondSignal(SDL_cond *c) { (void)c; return 0; }
int SDL_CondBroadcast(SDL_cond *c) { (void)c; return 0; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) { (void)c; (void)m; return 0; }
int SDL_CondWaitTimeout(SDL_cond *c, SDL_mutex *m, Uint32 t) { (void)c; (void)m; (void)t; return 0; }
void SDL_DestroyCond(SDL_cond *c) { if (c) SDL_free(c); }

sf2k_atomic_t s_atomic_init;
sf2k_atomic_t *SDL_AtomicCreate(sf2k_atomic_t v) { (void)v; return &s_atomic_init; }
void SDL_AtomicDestroy(sf2k_atomic_t *a) { (void)a; }
int SDL_AtomicSet(sf2k_atomic_t *a, int v) { if (a) a->value = v; return 0; }
int SDL_AtomicAdd(sf2k_atomic_t *a, int v) { if (!a) return 0; int old = a->value; a->value = old + v; return old; }
SDL_bool SDL_AtomicCAS(sf2k_atomic_t *a, int oldv, int newv)
{
    if (a && a->value == oldv) { a->value = newv; return SDL_TRUE; }
    return SDL_FALSE;
}
int SDL_AtomicGet(sf2k_atomic_t *a) { return a ? a->value : 0; }
void SDL_AtomicLock(SDL_SpinLock *a) { (void)a; }
void SDL_AtomicUnlock(SDL_SpinLock *a) { (void)a; }
SDL_bool SDL_AtomicTryLock(SDL_SpinLock *a) { (void)a; return SDL_TRUE; }

int SDL_SendSystemEvent(SDL_Event *ev) { return ev ? SDL_PushEvent(ev) : 0; }

/* ------------------------------------------------------- misc / dialogs */

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title,
                             const char *message, SDL_Window *window)
{
    (void)flags; (void)title; (void)window; (void)message;
    /* A modal dialog is impossible with no OS. Say what it was on stdout so a
     * failure is still diagnosable from the SD card's log. */
    fprintf(stdout, "[SDL] messagebox: %s\n", message ? message : "");
    return 0;
}

int SDL_ShowMessageBox(const SDL_MessageBoxData *d, int *buttonid)
{
    (void)d;
    if (buttonid)
        *buttonid = -1;
    return 0;
}

int SDL_OpenURL(const char *url)
{
    (void)url;
    return -1;
}

SDL_bool SDL_HasClipboardText(void) { return s_clipboard ? SDL_TRUE : SDL_FALSE; }

char *SDL_GetClipboardText(void)
{
    return s_clipboard ? s_clipboard : SDL_strdup("");
}

int SDL_SetClipboardText(const char *text)
{
    free(s_clipboard);
    s_clipboard = text ? SDL_strdup(text) : NULL;
    return 0;
}

/* ------------------------------------------------------------- GL stubs */

void *SDL_GL_GetProcAddress(const char *name) { (void)name; return NULL; }
int SDL_GL_LoadLibrary(const char *p) { (void)p; return -1; }
void SDL_GL_UnloadLibrary(void) {}
int SDL_GL_SetAttribute(SDL_GLattr a, int v) { (void)a; (void)v; return 0; }
int SDL_GL_GetAttribute(SDL_GLattr a, int *v) { (void)a; if (v) *v = 0; return 0; }
int SDL_GL_SetSwapInterval(int i) { (void)i; return 0; }
int SDL_GL_GetSwapInterval(void) { return 0; }
SDL_GLContext SDL_GL_GetCurrentContext(void) { return NULL; }
int SDL_GL_MakeCurrent(SDL_Window *w, SDL_GLContext c) { (void)w; (void)c; return 0; }
void SDL_GL_SwapWindow(SDL_Window *w) { (void)w; }
void SDL_GL_DeleteContext(SDL_GLContext c) { (void)c; }

/* ------------------------------------------------------------- helpers */

char *SDL_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

size_t SDL_strlcpy(char *dst, const char *src, size_t maxlen)
{
    size_t n = strlen(src);
    if (maxlen) {
        size_t c = n < maxlen - 1 ? n : maxlen - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t SDL_strlcat(char *dst, const char *src, size_t maxlen)
{
    size_t d = strnlen(dst, maxlen);
    size_t n = strlen(src);
    if (d == maxlen)
        return d + n;
    if (n < maxlen - d - 1) {
        memcpy(dst + d, src, n + 1);
    } else {
        memcpy(dst + d, src, maxlen - d - 1);
        dst[maxlen - 1] = '\0';
    }
    return d + n;
}

int SDL_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int SDL_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
char *SDL_strupr(char *s) { char *p = s; while (*p) { if (*p >= 'a' && *p <= 'z') *p -= 32; p++; } return s; }
char *SDL_strlwr(char *s) { char *p = s; while (*p) { if (*p >= 'A' && *p <= 'Z') *p += 32; p++; } return s; }
int SDL_snprintf(char *s, size_t n, const char *f, ...) { va_list a; int r; va_start(a, f); r = vsnprintf(s, n, f, a); va_end(a); return r; }
int SDL_atoi(const char *s) { return atoi(s); }
unsigned long SDL_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
double SDL_strtod(const char *s, char **e) { return strtod(s, e); }


/* power/accelerometer: not present, reported as unsupported */
SDL_PowerState SDL_GetPowerInfo(int *secs, int *pct) { if (secs) *secs = -1; if (pct) *pct = -1; return SDL_POWERSTATE_UNKNOWN; }
void SDL_AndroidGetActivityToken(SDL_Window *w, void **t) { (void)w; (void)t; }
void SDL_AndroidGetInternalActivityToken(void **t) { (void)t; }
int SDL_AndroidShowActivity(SDL_Window *w, const char *a, const char *d) { (void)w; (void)a; (void)d; return -1; }
int SDL_IsAndroidScreenSaverEnabled(void) { return 0; }
int SDL_GetAndroidSDKVersion(void) { return 0; }
int SDL_GetIOSVersion(void) { return 0; }
int SDL_Vulkan_GetDrawableSize(SDL_Window *w, int *x, int *y) { (void)w; if (x) *x = sf2k_present_width(); if (y) *y = sf2k_present_height(); return 0; }

/* ------------------------------------------------------------- ring */

static void ring_push(const int16_t *src, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        int next = (s_audio_tail + 1) % SF2K_AUDIO_RING;
        if (next == s_audio_head)
            break;                  /* full: drop the rest, the frontend is late */
        s_audio_ring[s_audio_tail * 2 + 0] = src[i * 2 + 0];
        s_audio_ring[s_audio_tail * 2 + 1] = src[i * 2 + 1];
        s_audio_tail = next;
    }
}

static int ring_pop(int16_t *dst, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        if (s_audio_head == s_audio_tail) {
            s_audio_underruns++;
            break;
        }
        dst[i * 2 + 0] = s_audio_ring[s_audio_head * 2 + 0];
        dst[i * 2 + 1] = s_audio_ring[s_audio_head * 2 + 1];
        s_audio_head = (s_audio_head + 1) % SF2K_AUDIO_RING;
    }
    return i;
}
/* ------------------------------------------------------------- RWops */

/* eduke32 only reaches SDL_RWops through SDL_RWFromConstMem, to hand the
 * gamecontroller.db it reads off disk to SDL_GameControllerAddMappingsFromRW.
 * There is no such database on the console, so this exists to keep the call
 * well-defined rather than to be fast. Reading is implemented properly anyway:
 * the read-only memory source is a few lines and returning a real object beats
 * returning NULL, which the caller treats as a hard error. */
static SDL_RWops s_mem_rwops;

static Sint64 SDLCALL mem_size(SDL_RWops *ctx)
{
    (void)ctx;
    return 0;
}

static Sint64 SDLCALL mem_seek(SDL_RWops *ctx, Sint64 off, int whence)
{
    RWops_mem *m = (RWops_mem *)ctx->hidden.unknown.data1;
    Sint64 base;

    switch (whence) {
    case RW_SEEK_SET: base = 0; break;
    case RW_SEEK_CUR: base = m->pos; break;
    case RW_SEEK_END: base = m->size; break;
    default: return SDL_SetError("invalid seek origin");
    }
    if (base + off < 0)
        return SDL_SetError("negative seek position");
    m->pos = base + off;
    return m->pos;
}

static size_t SDLCALL mem_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum)
{
    RWops_mem *m = (RWops_mem *)ctx->hidden.unknown.data1;
    size_t want = size * maxnum;
    size_t avail;

    if (size == 0)
        return 0;
    avail = (m->pos < m->size) ? (size_t)(m->size - m->pos) : 0;
    if (want > avail)
        want = avail;
    memcpy(ptr, (const Uint8 *)m->base + m->pos, want);
    m->pos += (Sint64)want;
    return want / size;
}

static size_t SDLCALL mem_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
    (void)ctx; (void)ptr;
    return 0;   /* the source is const memory */
}

static int SDLCALL mem_close(SDL_RWops *ctx)
{
    if (!ctx)
        return 0;
    if (ctx->hidden.unknown.data1)
        SDL_free(ctx->hidden.unknown.data1);
    SDL_FreeRW(ctx);
    return 0;
}

SDL_RWops *SDL_RWFromConstMem(const void *mem, int size)
{
    SDL_RWops *rw;
    RWops_mem *m;

    if (!mem || size < 0)
        return SDL_SetError("SDL_RWFromConstMem: no memory specified"), NULL;

    rw = SDL_AllocRW();
    if (!rw)
        return NULL;

    m = (RWops_mem *)SDL_malloc(sizeof(*m));
    if (!m) {
        SDL_FreeRW(rw);
        return NULL;
    }
    m->base = mem;
    m->size = size;
    m->pos = 0;

    rw->type = SDL_RWOPS_UNKNOWN;
    rw->hidden.unknown.data1 = m;
    rw->size = mem_size;
    rw->seek = mem_seek;
    rw->read = mem_read;
    rw->write = mem_write;
    rw->close = mem_close;
    return rw;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    return SDL_RWFromConstMem(mem, size);
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    SDL_RWops *rw = SDL_AllocRW();
    (void)file; (void)mode;
    if (rw) {
        rw->type = SDL_RWOPS_UNKNOWN;
        rw->close = mem_close;
    }
    return rw;
}

SDL_RWops *SDL_AllocRW(void)
{
    SDL_RWops *rw = (SDL_RWops *)SDL_malloc(sizeof(SDL_RWops));
    if (rw)
        memset(rw, 0, sizeof(*rw));
    return rw;
}

void SDL_FreeRW(SDL_RWops *rw)
{
    SDL_free(rw);
}

Sint64 SDL_RWsize(SDL_RWops *rw) { return (rw && rw->size) ? rw->size(rw) : 0; }
Sint64 SDL_RWseek(SDL_RWops *rw, Sint64 off, int whence) { return rw && rw->seek ? rw->seek(rw, off, whence) : 0; }
Sint64 SDL_RWtell(SDL_RWops *rw) { return SDL_RWseek(rw, 0, RW_SEEK_CUR); }
size_t SDL_RWread(SDL_RWops *rw, void *ptr, size_t size, size_t n) { return rw && rw->read ? rw->read(rw, ptr, size, n) : 0; }
size_t SDL_RWwrite(SDL_RWops *rw, const void *ptr, size_t size, size_t n) { return rw && rw->write ? rw->write(rw, ptr, size, n) : 0; }
int SDL_RWclose(SDL_RWops *rw) { return rw && rw->close ? rw->close(rw) : 0; }

/* ------------------------------------------------------- remaining surface
 *
 * The entry points below are not in the list derived from sdlayer.cpp and
 * audiolib; they come from imgui's SDL2 backend, audiolib's other drivers and
 * BUILD itself. All of them are things the console has no answer for, so the
 * honest implementations are the ones that report "nothing here".
 */

void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
    if (!window)
        return;
    SDL_GetWindowSize(window, w, h);
}

const char *SDL_GetKeyName(SDL_Keycode key)
{
    (void)key;
    /* eduke32 writes this into its keybinding table, so it has to be stable and
     * unique per key rather than empty, or every binding would read as the same
     * name in the setup menus. */
    static char name[16];
    snprintf(name, sizeof(name), "key%d", (int)key);
    return name;
}

void SDL_SetTextInputRect(const SDL_Rect *rect) { (void)rect; }

int SDL_SetMemoryFunctions(SDL_malloc_func malloc_func, SDL_calloc_func calloc_func,
                           SDL_realloc_func realloc_func, SDL_free_func free_func)
{
    (void)malloc_func; (void)calloc_func; (void)realloc_func; (void)free_func;
    return -1;
}

void *SDL_memset(void *dst, int c, size_t len)
{
    return memset(dst, c, len);
}

void *SDL_memcpy(void *dst, const void *src, size_t len)
{
    return memcpy(dst, src, len);
}

void *SDL_memmove(void *dst, const void *src, size_t len)
{
    return memmove(dst, src, len);
}

int SDL_memcmp(const void *a, const void *b, size_t len)
{
    return memcmp(a, b, len);
}

SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info)
{
    (void)window;
    if (info) {
        memset(info, 0, sizeof(*info));
        SDL_VERSION(&info->version);
        info->subsystem = SDL_SYSWM_UNKNOWN;
    }
    return SDL_FALSE;
}

const char *SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis axis)
{
    (void)axis;
    return "";
}

const char *SDL_GameControllerGetStringForButton(SDL_GameControllerButton button)
{
    (void)button;
    return "";
}

SDL_bool SDL_GameControllerHasButton(SDL_GameController *g, SDL_GameControllerButton b)
{
    (void)g; (void)b;
    return SDL_FALSE;
}

SDL_bool SDL_GameControllerHasAxis(SDL_GameController *g, SDL_GameControllerAxis a)
{
    (void)g; (void)a;
    return SDL_FALSE;
}

void SDL_FreeCursor(sf2k_Cursor *cur) { (void)cur; }

int SDL_JoystickRumble(SDL_Joystick *j, Uint16 lo, Uint16 hi, Uint32 ms)
{
    (void)j; (void)lo; (void)hi; (void)ms;
    return -1;
}

SDL_bool SDL_JoystickHasRumble(SDL_Joystick *j)
{
    (void)j;
    return SDL_FALSE;
}
