/*
 * eduke32 entry point on the SF2000 / GB300.
 *
 * eduke32 is a hosted program: its main() parses argv, calls initsystem() and
 * then never returns -- it owns the thread. The console's multicore frontend
 * wants the opposite, calling retro_run() once per frame from its own loop.
 *
 * The join is libco. sf2k_entry_start() spawns a coroutine that calls
 * eduke32's real main() with a fixed, empty argv, and everything eduke32 does
 * that would block -- SDL_Flip, SDL_Delay -- is intercepted by the SDL2 shim and
 * turned into a co_yield(). One co_resume() is therefore one frame.
 *
 * eduke32's main() is renamed to eduke32_sf2000_main() at build time (see
 * -Dmain=eduke32_sf2000_main in Makefile.sf2000) so that the archive can still
 * provide a real main() when it is linked into a standalone executable for
 * debugging on the host.
 */

#include "eduke32_entry.h"
#include "libco/co_api.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hacker_loader.h"
#include "compat/sf2k_log.h"

/* eduke32's real main(), renamed by the build with -Dmain=eduke32_sf2000_main.
 *
 * This has to be C++ linkage: main() is defined in sdlayer.cpp, which is C++, so
 * -Dmain=... renames it to a function with C++ mangling
 * (_Z19eduke32_sf2000_mainiPPc). Declaring it extern "C" from a C translation
 * unit would look for an unmangled symbol and fail to link. */
extern int eduke32_sf2000_main(int argc, char *argv[]);

static cothread_t s_co;

static char s_arg0[] = "eduke32";
static char *s_argv[2];

static void entry_trampoline(void)
{
    s_argv[0] = s_arg0;
    s_argv[1] = NULL;

    hacker_loader_log("CO", "inside trampoline!", 76);
    SF2K_LOG("trampoline: entering eduke32 main");

    /* Single-frame kick to get the engine running. No window, no resolution
     * juggling, no startup window, no config write unless the player asks. */
    eduke32_sf2000_main(1, s_argv);

    hacker_loader_log("CO", "eduke32 main exited!", 100);
    SF2K_LOG("trampoline: eduke32 main returned");

    /* main() is not supposed to return. If it does -- quit via the console menu,
     * which calls Bexit() -- park the coroutine forever rather than letting it
     * fall off the end and run whatever is next on the stack. */
    hacker_loader_log("CORE", "game loop exited, parking core", 100);
    for (;;)
        sf2k_co_yield();
}

void *sf2k_entry_start(const char *dir)
{
    SF2K_LOG("entry_start: calling sf2k_co_init");
    sf2k_co_init();
    SF2K_LOG("entry_start: calling sf2k_co_create(256KB)");
    s_co = sf2k_co_create(256 * 1024, entry_trampoline);
    SF2K_LOG("entry_start: sf2k_co_create returned %p", (void*)s_co);
    if (!s_co) {
        fprintf(stderr, "[sf2000] co_create failed\n");
        return NULL;
    }
    return s_co;
}

void sf2k_entry_stop(void)
{
    if (s_co) {
        sf2k_co_destroy(s_co);
        s_co = NULL;
    }
}

/* The bindings the pad needs, embedded rather than shipped as a separate file so
 * that a core dropped on the card as a single binary still gets them. The .inc
 * is generated from default_autoexec.cfg, so there is one source of truth. */
#include "default_autoexec.cfg.inc"

/* Writes autoexec.cfg next to the game data if it is not already there. Returns 0
 * on success, -1 on failure; an existing file is left untouched. */
int sf2k_install_default_config(const char *dir)
{
    char path[512];
    FILE *dst;
    size_t n;

    if (!dir || !*dir)
        dir = ".";
    snprintf(path, sizeof(path), "%s/autoexec.cfg", dir);

    dst = fopen(path, "wb");
    if (!dst)
        return -1;

    n = fwrite(kDefaultAutoexec, 1, sizeof(kDefaultAutoexec) - 1, dst);
    fclose(dst);
    return n == sizeof(kDefaultAutoexec) - 1 ? 0 : -1;
}

int sf2k_game_data_present(const char *dir)
{
    char path[512];

    if (!dir || !*dir)
        dir = ".";
    snprintf(path, sizeof(path), "%s/DUKE3D.GRP", dir);
    {
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            return 0;
        }
    }
    return -1;
}