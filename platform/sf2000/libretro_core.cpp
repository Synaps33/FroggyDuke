/*
 * libretro entry points for eduke32 on the Data Frog SF2000 / GB300.
 *
 * The console has no OS: the multicore frontend dlopen()s (or links) this as a
 * static core loaded at 0x87000000 and calls retro_* directly, yielding to us
 * once per frame from its own loop. eduke32, meanwhile, wants to own the thread
 * and spin SDL_Init / the game loop forever. The gap is closed with libco: the
 * game runs on a coroutine, and the points where it must hand control back --
 * presenting a frame, sleeping -- are co_yield()s.
 *
 * So one retro_run() is:
 *
 *     input poll  ->  resume the coroutine  ->  it runs until it presents
 *
 * and the coroutine is what actually calls eduke32's main loop. That keeps
 * eduke32's own timing untouched; the only thing it sees is that SDL_Flip and
 * SDL_Delay come back to us.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"
#include "libco/co_api.h"

#include "sdl2_shim.h"
#include "sf2000_input.h"
#include "hacker_loader.h"
#include "compat/sf2k_log.h"
#include "eduke32_entry.h"

/* ---------------------------------------------------------------- state */

static retro_video_refresh_t  s_video_cb;
static retro_audio_sample_t  s_audio_cb;
static retro_audio_sample_batch_t s_audio_batch_cb;
static retro_input_poll_t    s_input_poll_cb;
static retro_input_state_t   s_input_state_cb;
static retro_environment_t   s_env_cb;

static int          s_inited;
static int          s_game_loaded;
static int          s_shutdown;
static uint32_t     s_present_w = 320;
static uint32_t     s_present_h = 200;
static const char  *s_game_dir = ".";

/* Set while the coroutine is suspended inside a present, so that a second
 * frame cannot start while the previous one is still mid-swap. */
static int          s_in_frame;

/* The coroutine eduke32 runs on. */
static cothread_t s_game_co;

static int  s_co_running;

/* ---------------------------------------------------------------- clock */

/* gettimeofday() is the clock the console frontend actually implements (see
 * sf2000_multicore's lib.c); clock_gettime() is not available there. Microsecond
 * resolution is more than enough to pace a 30 Hz frame loop and to drive the
 * mouse-look acceleration ramp. */
uint64_t sf2k_ticks_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

uint32_t sf2k_ticks_ms(void)
{
    return (uint32_t)(sf2k_ticks_us() / 1000);
}

void sf2k_delay_ms(uint32_t ms)
{
    /* The core is driven by the frontend's own vsync-ish loop, so sleeping here
     * would stall the whole machine. A yield is the correct way to say "I have
     * nothing to do until the next frame". */
    if (s_co_running && ms > 0)
        sf2k_co_yield();
}

int sf2k_present_width(void)  { return (int)s_present_w; }
int sf2k_present_height(void) { return (int)s_present_h; }

/* Duke is directory-based: the game data is DUKE3D.GRP and DUKE.RTS sitting in
 * a directory, not a single ROM file. The two frontends disagree about what to
 * pass, so accept both: the multicore passes the stub file it was launched with
 * ("/mnt/sda1/ROMS/eduke32/eduke32;eduke32;eduke32.gba") while the test runner
 * passes the directory itself ("/path/to/game", or "./game"). A path whose last
 * component carries an extension is a file, so take its directory; anything
 * else is already the directory.
 *
 * The returned pointer is either the input or a malloc()ed copy; the caller
 * owns neither beyond the life of the core. */
static const char *sf2k_game_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name  = slash ? slash + 1 : path;
    const char *dot   = strrchr(name, '.');

    if (dot && dot != name) {
        size_t n = (size_t)(slash ? (slash - path) : 0);
        char *dir;
        if (n == 0)
            return ".";
        dir = (char *)malloc(n + 1);
        if (!dir)
            return path;
        memcpy(dir, path, n);
        dir[n] = '\0';
        return dir;
    }
    return path;
}

/* ------------------------------------------------------------- present */

void sf2k_present(const void *pixels, int width, int height, int pitch)
{
    if (s_video_cb)
        s_video_cb(pixels, (unsigned)width, (unsigned)height, (size_t)pitch);

    if (s_co_running) {
        s_in_frame = 0;
        /* Back to retro_run(): let the frontend do whatever it does between
         * frames, then resume us. */
        sf2k_co_yield();
    }
}

/* ---------------------------------------------------------------- input */

#if defined(SF2000)
extern "C" volatile uint32_t g_joy_task_state;
#endif

/* The pad is the only input device. Read it here rather than in the input
 * module so that sf2000_input.c stays free of any libretro dependency. */
static unsigned sf2k_read_pad(void)
{
#if defined(SF2000)
    uint32_t raw = g_joy_task_state;
    unsigned mask = 0;

    if (raw & 0x0010) mask |= SF2K_PAD_UP;
    if (raw & 0x0040) mask |= SF2K_PAD_DOWN;
    if (raw & 0x0080) mask |= SF2K_PAD_LEFT;
    if (raw & 0x0020) mask |= SF2K_PAD_RIGHT;
    if (raw & 0x2000) mask |= SF2K_PAD_A;
    if (raw & 0x4000) mask |= SF2K_PAD_B;
    if (raw & 0x0400) mask |= SF2K_PAD_X;
    if (raw & 0x0800) mask |= SF2K_PAD_Y;
    if (raw & 0x1000) mask |= SF2K_PAD_L;
    if (raw & 0x8000) mask |= SF2K_PAD_R;
    if (raw & 0x0008) mask |= SF2K_PAD_START;
    if (raw & 0x0001) mask |= SF2K_PAD_SELECT;

    return mask;
#else
    static const struct { unsigned bit; unsigned id; } map[] = {
        { SF2K_PAD_UP,     RETRO_DEVICE_ID_JOYPAD_UP },
        { SF2K_PAD_DOWN,   RETRO_DEVICE_ID_JOYPAD_DOWN },
        { SF2K_PAD_LEFT,   RETRO_DEVICE_ID_JOYPAD_LEFT },
        { SF2K_PAD_RIGHT,  RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { SF2K_PAD_A,      RETRO_DEVICE_ID_JOYPAD_A },
        { SF2K_PAD_B,      RETRO_DEVICE_ID_JOYPAD_B },
        { SF2K_PAD_X,      RETRO_DEVICE_ID_JOYPAD_X },
        { SF2K_PAD_Y,      RETRO_DEVICE_ID_JOYPAD_Y },
        { SF2K_PAD_L,      RETRO_DEVICE_ID_JOYPAD_L },
        { SF2K_PAD_R,      RETRO_DEVICE_ID_JOYPAD_R },
        { SF2K_PAD_START,  RETRO_DEVICE_ID_JOYPAD_START },
        { SF2K_PAD_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT },
    };
    unsigned mask = 0;
    size_t i;

    if (!s_input_state_cb)
        return 0;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (s_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
            mask |= map[i].bit;
    }
    return mask;
#endif
}


void sf2k_pump_events(void)
{
}

/* ---------------------------------------------------------------- audio */

static int16_t s_audio_buf[4096 * 2];

static size_t pump_audio(void)
{
    int frames = 2048;

    sf2k_audio_pump(s_audio_buf, frames);

    if (s_audio_batch_cb)
        return s_audio_batch_cb(s_audio_buf, (size_t)frames);
    if (s_audio_cb) {
        size_t i;
        for (i = 0; i < (size_t)frames; i++)
            s_audio_cb(s_audio_buf[i * 2], s_audio_buf[i * 2 + 1]);
        return (size_t)frames;
    }
    return 0;
}

/* ------------------------------------------------------------- libretro */

void retro_set_environment(retro_environment_t cb) { s_env_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { s_video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { s_audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { s_audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { s_input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { s_input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "eduke32";
    info->library_version  = "1.0-sf2000";
    info->valid_extensions = "grp|rts";
    /* The core opens DUKE3D.GRP and DUKE.RTS itself, relative to the directory
     * it derives from info->path, and never looks at info->data.
     *
     * Asking for a full path matters a lot here. multicore's
     * wrap_retro_load_game() does
     *
     *     if (!sysinfo.need_fullpath || core_supports_rom_in_buffer)
     *         buffer = malloc(size); fread(buffer, 1, size, hfile);
     *
     * so a core that does not ask gets the *entire* selected file slurped into
     * the heap -- 11 MB of GRP, read through the console's FAT stack, out of a
     * 58 MiB budget, for data the core was never going to use. With
     * need_fullpath set, multicore logs "core loads content directly from
     * file" and passes the path with data left NULL. */
    info->need_fullpath    = true;
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = s_present_w;
    info->geometry.base_height  = s_present_h;
    info->geometry.max_width    = s_present_w;
    info->geometry.max_height   = s_present_h;
    /* Shareware Duke runs its game logic at 30 Hz on a 320x200 screen. The
     * frontend throttles to this, so it is what the coroutine's pacing has to
     * match. */
    info->timing.fps            = 30.0;
    info->timing.sample_rate    = 44100.0;
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = s_game_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        return false;
    }
}

void retro_init(void)
{
    if (s_inited)
        return;
    s_inited = 1;

    /* Earliest point the frontend reaches inside the core. Tracing it here
     * separates "the core was never entered" from "it entered and stopped in
     * retro_load_game", which the on-card log alone cannot show. */
    SF2K_LOG("retro_init: core entered");

    if (s_env_cb) {
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
        s_env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
    }

    hacker_loader_set_video_cb(s_video_cb);
    hacker_loader_init("duke3d.grp");
    hacker_loader_log("CORE", "EDuke32 1.0 for SF2000/GB300", 10);
    hacker_loader_log("CPU", "MIPS32r2 soft-float, no FPU", 18);
    hacker_loader_log("VFS", "MOUNT /dev/sdcard ... [OK]", 26);

    sf2k_input_init();
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!s_inited)
        retro_init();

    SF2K_LOG("load_game: enter, path=%s", game && game->path ? game->path : "(null)");
    if (game && game->path)
        s_game_dir = sf2k_game_dir(game->path);
    SF2K_LOG("load_game: game_dir=%s", s_game_dir);

    hacker_loader_log("GAME", "locating DUKE3D.GRP ...", 35);
    if (sf2k_game_data_present(s_game_dir) != 0) {
        hacker_loader_log("GAME", "DUKE3D.GRP NOT FOUND", 35);
        hacker_loader_log("VFS", "copy the shareware data to", 35);
        hacker_loader_log("VFS", "ROMS/eduke32/ and reload", 35);
        hacker_loader_stop();
        return false;
    }
    hacker_loader_log("GAME", "DUKE3D.GRP found, parsing GRP", 45);
    SF2K_LOG("load_game: DUKE3D.GRP present");

    /* Install the pad's key bindings if the player has none yet. Not replacing
     * an existing autoexec.cfg: once written it belongs to the player, who can
     * edit it or rebind from the game's own setup menu, and both work because the
     * pad only ever sends ordinary keys. */
    if (!sf2k_install_default_config(s_game_dir)) {
        hacker_loader_log("CFG", "could not write autoexec.cfg", 48);
    } else {
        hacker_loader_log("CFG", "autoexec.cfg bindings installed", 48);
    }

    /* eduke32 wants its data in the working directory; the frontend gives us a
     * ROM path but no chdir, so do it here. */
    SF2K_LOG("load_game: cfg done, before chdir");
    if (chdir(s_game_dir) != 0) {
        hacker_loader_log("VFS", "chdir failed, trying cwd", 45);
    }
    SF2K_LOG("load_game: cwd now %s", s_game_dir);

    hacker_loader_log("SDL", "video 320x200 RGB565 -> panel", 55);
    hacker_loader_log("SDL", "audio 44100Hz stereo", 62);
    hacker_loader_log("PAD", "D-pad + A/B/X/Y/L/R/Start/Select", 70);

    SF2K_LOG("load_game: before entry_start");
    hacker_loader_log("CO", "creating coroutine...", 72);
    s_game_co = (cothread_t)sf2k_entry_start(s_game_dir);
    if (!s_game_co) {
        hacker_loader_log("CORE", "entry point failed", 70);
        hacker_loader_stop();
        return false;
    }
    hacker_loader_log("CO", "coroutine ready, resuming...", 75);
    s_game_loaded = 1;
    s_co_running = 1;

    /* Run the game until it has drawn its first frame, still inside
     * retro_load_game().
     *
     * This is what devilutionx -- a core that works on this card -- does: it
     * switches to the game thread right after creating it, to run until the
     * first frame is ready.
     *
     * The multicore frontend's emulator loop will not call retro_run() until
     * the first video refresh has happened, and on this port it never got
     * that far: retro_load_game returned, the log stopped dead between
     * "retro_load_game returned 1" and "retro_load_game ok", and the hand-
     * written trace showed retro_run: frame 0 was never reached.
     *
     * One co_resume() is exactly one frame: sf2k_present() calls the video
     * callback and then yields straight back to us. */
    SF2K_LOG("load_game: resuming until first frame");
    sf2k_co_resume(s_game_co);
    hacker_loader_log("CO", "first frame drawn!", 99);
    SF2K_LOG("load_game: first frame drawn");

    hacker_loader_log("CORE", "running on libco coroutine", 85);
    SF2K_LOG("load_game: returning true");
    return true;
}

void retro_unload_game(void)
{
    if (s_co_running) {
        sf2k_entry_stop();
        s_co_running = 0;
    }
    s_game_loaded = 0;
}

void retro_deinit(void)
{
    retro_unload_game();
    hacker_loader_stop();
    s_inited = 0;
}

void retro_run(void)
{
    static unsigned run_frames;

    if (run_frames < 8)
        SF2K_LOG("retro_run: frame %u, loaded=%d co=%p", run_frames,
                 s_game_loaded, (void *)s_game_co);
    run_frames++;

    if (!s_game_loaded)
        return;

    if (s_env_cb) {
        bool b = false;
        if (s_env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &b) && b) {
            /* core options are applied on the next frame boundary */
        }
    }

    /* Sample the pad once per frame, exactly as the console's frontend does: ask
     * libretro for the current state, then translate it into the keys and mouse
     * buttons the game will read during the frame it is about to run. Doing it
     * here rather than from inside SDL's event pump keeps it once-per-frame and
     * keeps it out of the middle of the game draining its own event queue. */
    if (s_input_poll_cb)
        s_input_poll_cb();
    sf2k_input_poll(sf2k_read_pad());

    /* Resume the game coroutine. It yields back from sf2k_present() once per
     * frame, which is where control returns to us. */
    s_in_frame = 1;
    sf2k_co_resume(s_game_co);

    if (run_frames <= 8)
        SF2K_LOG("retro_run: frame %u resumed", run_frames - 1);

    pump_audio();
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{
    (void)type; (void)info; (void)count;
    return false;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port; (void)device;
}

void retro_set_memory_size(size_t size) { (void)size; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

void retro_get_perf_interface(struct retro_perf_callback *cb)
{
    (void)cb;
}

unsigned retro_get_region(void)
{
    /* The shareware release this port ships is NTSC; the timing code keys off
     * this, so it must not be left unset. */
    return RETRO_REGION_NTSC;
}

void retro_reset(void)
{
    /* The console frontend only calls this to honour a system reset. The game
     * keeps its own state machine for that, so tearing the coroutine down and
     * letting the next retro_load_game() rebuild it is the honest answer. */
    retro_unload_game();
    if (s_game_co)
    {
        sf2k_co_destroy(s_game_co);
        s_game_co = NULL;
    }
}