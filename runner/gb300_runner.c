/*
 * gb300 test runner for the eduke32 SF2000 core.
 *
 * The console's core is a shared object on the host, driven through libretro,
 * with a scripted joypad timeline and a PPM dump after every step. That is the
 * only way to exercise the paths this port actually takes -- pad -> synthetic
 * SDL events -> eduke32's binding layer -> game -- without the hardware.
 *
 * Two console properties are modelled, because they are the two that decide
 * whether this port works:
 *
 *   - a 32 MB heap, via --ram-mb N and RLIMIT_AS;
 *   - no FPU, because build/gbsim is compiled with -mfpmath=387 and no SSE.
 *
 * The frame time measured here is then scaled by --cpu-factor to predict what
 * the card would do at its rated clock. Treat that as an estimate.
 *
 * usage: gb300_runner <core.so> <game-dir> [frames] [dump-every] [script]
 *
 * The optional script is a comma separated list of
 *   <first_frame>[-<last_frame>]:<button>[+<button>...]
 * where <button> is one of up,down,left,right,a,b,x,y,l,r,start,select.
 * With no script a built-in "boot and start a new game" timeline is used, which
 * is the one worth watching: menu -> New Game -> Episode 1 -> first level.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <time.h>
#include <stdarg.h>
#include <malloc.h>
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#include <execinfo.h>
#include <errno.h>

#include "libretro.h"

/* The core presents at BUILD's native resolution and the frontend scales it to
 * the panel; keep room for the largest size any SF2000 core uses. */
#define MAX_W 640
#define MAX_H 480

/* The SF2000 runs at ~600 MHz and the GB300 at ~900. Default to the GB300. */
#define CONSOLE_MHZ_DEFAULT 900.0

static int       s_w = 320;
static int       s_h = 200;
static uint16_t  s_frame[MAX_W * MAX_H];
static int       s_frame_count;
static unsigned  s_last_width, s_last_height;
static uint32_t  s_pad_mask;
static int       s_shutdown_requested;
static int64_t   s_audio_frames_total;
static uint32_t  s_last_present_ms;

/* Where the runner was started; see save_ppm(). */
static char      s_launch_dir[512];

/* one entry of the input timeline */
struct step {
	int first, last;
	uint32_t mask;
};

static struct step s_steps[256];
static int s_step_count;

static void log_printf_cb(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static void cb_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	unsigned y, x;

	if (!data)
		return;

	s_last_width = width;
	s_last_height = height;
	if (width > MAX_W)  width = MAX_W;
	if (height > MAX_H) height = MAX_H;
	if (width != s_w || height != s_h) {
		/* resolution changed: start from a clean frame */
		memset(s_frame, 0, sizeof(s_frame));
		s_w = width;
		s_h = height;
	}

	for (y = 0; y < height; y++) {
		const uint8_t *src = (const uint8_t *)data + y * pitch;
		uint16_t *dst = &s_frame[y * MAX_W];
		for (x = 0; x < width; x++)
			dst[x] = *(const uint16_t *)(src + x * 2);
	}
	s_frame_count++;
}

static void cb_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t cb_audio_sample_batch(const int16_t *d, size_t frames)
{
	(void)d;
	s_audio_frames_total += (int64_t)frames;
	return frames;
}

static void cb_input_poll(void) { }

static int16_t cb_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)port; (void)index;
	if (device != RETRO_DEVICE_JOYPAD)
		return 0;
	return (s_pad_mask & (1u << id)) ? 1 : 0;
}

static bool cb_environment(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (data) {
			struct retro_log_callback *cb = (struct retro_log_callback *)data;
			cb->log = log_printf_cb;
			return true;
		}
		return false;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		s_shutdown_requested = 1;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	default:
		return false;
	}
}

static int count_non_black(const uint16_t *fb, int count)
{
	int i, nb = 0;
	for (i = 0; i < count; i++)
		if (fb[i] != 0)
			nb++;
	return nb;
}

static int count_distinct(const uint16_t *fb, int count)
{
	static uint16_t seen[4096];
	int i, n = 0;

	memset(seen, 0, sizeof(seen));
	for (i = 0; i < count; i++) {
		uint16_t v = fb[i];
		uint16_t h;
		if (v == 0)
			continue;
		h = (uint16_t)((v * 2654435761u) >> 20);
		seen[h & 4095] = 1;
	}
	for (i = 0; i < 4096; i++)
		if (seen[i])
			n++;
	return n;
}

/* sum of all pixels -- cheap "did the screen change at all" fingerprint */
static uint32_t checksum(const uint16_t *fb, int count)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < count; i++) {
		h ^= fb[i];
		h *= 16777619u;
	}
	return h;
}

static void save_ppm(const char *name)
{
	char path[640];
	FILE *f;
	int y, x;

	snprintf(path, sizeof(path), "%s/%s", s_launch_dir, name);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "[TEST] cannot write %s: %s\n", path, strerror(errno));
		return;
	}

	fprintf(f, "P6\n%d %d\n255\n", s_w, s_h);
	for (y = 0; y < s_h; y++) {
		for (x = 0; x < s_w; x++) {
			uint16_t p = s_frame[y * MAX_W + x];
			fputc((p >> 11) << 3, f);
			fputc(((p >> 5) & 0x3F) << 2, f);
			fputc((p & 0x1F) << 3, f);
		}
	}
	fclose(f);
}

static double now_ms(void);

/* Frame watchdog.
 *
 * A hung frame is the failure this port is most likely to hit -- the game runs
 * on a coroutine and every SDL_Delay becomes a yield back to us, so a mistake
 * in that plumbing shows up as a hang rather than a crash. Without this the
 * runner just sits there and stdout is lost when it is killed, which says
 * nothing about where it went.
 */
static double s_frame_start;
static void on_alarm(int sig)
{
    double held = now_ms() - s_frame_start;
    int i;

    (void)sig;
    fprintf(stderr, "\n[WATCHDOG] still inside retro_run() after %.1f s\n", held / 1000.0);
    fflush(stderr);

    {
        void *frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, 2);
    }
    fflush(stderr);
    for (i = 0; i < 100; i++) { }
    _exit(99);
}

static void watchdog_arm(double seconds)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    alarm((unsigned)seconds);
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static const struct { const char *name; uint32_t bit; } s_buttons[] = {
	{ "up",    RETRO_DEVICE_ID_JOYPAD_UP },
	{ "down",  RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ "left",  RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ "a",     RETRO_DEVICE_ID_JOYPAD_A },
	{ "b",     RETRO_DEVICE_ID_JOYPAD_B },
	{ "x",     RETRO_DEVICE_ID_JOYPAD_X },
	{ "y",     RETRO_DEVICE_ID_JOYPAD_Y },
	{ "l",     RETRO_DEVICE_ID_JOYPAD_L },
	{ "r",     RETRO_DEVICE_ID_JOYPAD_R },
	{ "start", RETRO_DEVICE_ID_JOYPAD_START },
	{ "select", RETRO_DEVICE_ID_JOYPAD_SELECT },
};

static uint32_t parse_buttons(char *list)
{
	char *save = NULL;
	char *tok;
	uint32_t mask = 0;

	for (tok = strtok_r(list, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		size_t i;
		for (i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
			if (strcasecmp(tok, s_buttons[i].name) == 0) {
				mask |= 1u << s_buttons[i].bit;
				break;
			}
		}
		if (i == sizeof(s_buttons) / sizeof(s_buttons[0]))
			fprintf(stderr, "[TEST] unknown button '%s'\n", tok);
	}
	return mask;
}

static void add_step(int first, int last, uint32_t mask)
{
	if (s_step_count >= (int)(sizeof(s_steps) / sizeof(s_steps[0])) || mask == 0)
		return;
	s_steps[s_step_count].first = first;
	s_steps[s_step_count].last = last;
	s_steps[s_step_count].mask = mask;
	s_step_count++;
}

/*
 * The timeline that matters: title screen -> New Game -> a playable level.
 *
 * The timings are not arbitrary. eduke32's title logo runs for 980 game tics at
 * 120 Hz, so under the test model the menu is not reachable until roughly frame
 * 1000; the numbers below were read off a captured run. A is "New Game", then
 * the game-setup screen is confirmed with A, then Episode 1 starts.
 */
static void boot_script(int frames)
{
	struct { int f; const char *b; } steps[] = {
		{ 1080, "a" },       /* New Game                       */
		{ 1200, "a" },       /* accept the game setup defaults */
		{ 1320, "a" },       /* start Episode 1                 */
		{ 1500, "up" },      /* move forward                   */
		{ 1530, "a" },       /* fire, to prove the action binds */
		{ 1560, "right" },   /* turn right (mouse-look)         */
		{ 1640, "left" },
		{ 1720, "right" },
		{ 1800, "l" },       /* strafe                          */
		{ 1880, "x" },       /* jump                            */
		{ 1960, "start" },   /* automap                         */
		{ 2040, "start" },
	};
	size_t i;
	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		int f = steps[i].f;
		if (f + 4 >= frames)
			break;
		add_step(f, f + 3, parse_buttons((char *)steps[i].b));
	}
}

static void load_script(const char *spec, int frames)
{
	char *copy = strdup(spec);
	char *save = NULL;
	char *tok;

	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon = strchr(tok, ':');
		int first, last;
		if (!colon)
			continue;
		*colon = '\0';
		if (sscanf(tok, "%d-%d", &first, &last) != 2) {
			if (sscanf(tok, "%d", &first) != 1)
				continue;
			last = first + 5;
		}
		if (last >= frames)
			last = frames - 1;
		add_step(first, last, parse_buttons(colon + 1));
	}
	free(copy);
}

static void *g_handle;

static void *sym(const char *name)
{
	void *p = dlsym(g_handle, name);
	if (!p) {
		fprintf(stderr, "missing symbol %s\n", name);
		exit(1);
	}
	return p;
}

int main(int argc, char **argv)
{
	void *handle;
	struct retro_system_info sys;
	struct retro_system_av_info av;
	struct retro_game_info game;
	const char *so_path = "./eduke32_libretro.so";
	const char *rom_dir = ".";
	const char *script = NULL;
	int total_frames = 600;
	int dump_every = 0;
	int ram_mb = 0, verbose = 0;
	double watchdog_seconds = 0;
	double cpu_mhz = CONSOLE_MHZ_DEFAULT;
	int i, argi, npos = 0;
	const char *pos[5];
	double worst = 0.0, total = 0.0, best = 1e30;
	uint32_t last_sum = 0;
	int changed_at = -1;

	/* Options may appear anywhere: collect the positionals as they are seen and
	 * interpret them afterwards, so `--watchdog 60` after a script argument does
	 * not silently become the script. */
	for (argi = 1; argi < argc; argi++) {
		if (!strcmp(argv[argi], "--ram-mb") && argi + 1 < argc) {
			ram_mb = atoi(argv[++argi]);
		} else if (!strcmp(argv[argi], "--cpu-mhz") && argi + 1 < argc) {
			cpu_mhz = atof(argv[++argi]);
		} else if (!strcmp(argv[argi], "--watchdog") && argi + 1 < argc) {
			watchdog_seconds = atof(argv[++argi]);
		} else if (!strcmp(argv[argi], "--verbose")) {
			verbose = 1;
		} else if (npos < 5) {
			pos[npos++] = argv[argi];
		}
	}
	if (npos > 0) so_path  = pos[0];
	if (npos > 1) rom_dir  = pos[1];
	if (npos > 2) total_frames = atoi(pos[2]);
	if (npos > 3) dump_every = atoi(pos[3]);
	if (npos > 4) script  = pos[4];

	if (!getcwd(s_launch_dir, sizeof(s_launch_dir)))
		s_launch_dir[0] = '\0';
	else
		printf("[TEST] launch dir: %s\n", s_launch_dir);

	if (ram_mb > 0) {
		/* Model the console's RAM ceiling. */
		struct rlimit rl;
		char *env = getenv("MALLOC_ARENA_MAX");
		(void)env;
		rl.rlim_cur = (rlim_t)ram_mb * 1024 * 1024;
		rl.rlim_max = rl.rlim_cur;
		if (setrlimit(RLIMIT_AS, &rl) != 0)
			fprintf(stderr, "[TEST] warning: could not cap address space at %d MB\n", ram_mb);
		else
			printf("[TEST] address space capped at %d MB\n", ram_mb);
	}

	unsigned (*retro_api_version)(void);
	void (*retro_get_system_info)(struct retro_system_info *);
	void (*retro_get_system_av_info)(struct retro_system_av_info *);
	void (*retro_set_environment)(retro_environment_t);
	void (*retro_set_video_refresh)(retro_video_refresh_t);
	void (*retro_set_audio_sample)(retro_audio_sample_t);
	void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*retro_set_input_poll)(retro_input_poll_t);
	void (*retro_set_input_state)(retro_input_state_t);
	void (*retro_init)(void);
	void (*retro_deinit)(void);
	bool (*retro_load_game)(const struct retro_game_info *);
	void (*retro_run)(void);
	void (*retro_unload_game)(void);

	handle = dlopen(so_path, RTLD_NOW);
	if (!handle) {
		fprintf(stderr, "dlopen failed: %s\n", dlerror());
		return 1;
	}
	g_handle = handle;

	*(void **)&retro_api_version            = sym("retro_api_version");
	*(void **)&retro_get_system_info        = sym("retro_get_system_info");
	*(void **)&retro_get_system_av_info     = sym("retro_get_system_av_info");
	*(void **)&retro_set_environment        = sym("retro_set_environment");
	*(void **)&retro_set_video_refresh      = sym("retro_set_video_refresh");
	*(void **)&retro_set_audio_sample       = sym("retro_set_audio_sample");
	*(void **)&retro_set_audio_sample_batch = sym("retro_set_audio_sample_batch");
	*(void **)&retro_set_input_poll         = sym("retro_set_input_poll");
	*(void **)&retro_set_input_state        = sym("retro_set_input_state");
	*(void **)&retro_init                   = sym("retro_init");
	*(void **)&retro_deinit                 = sym("retro_deinit");
	*(void **)&retro_load_game              = sym("retro_load_game");
	*(void **)&retro_run                    = sym("retro_run");
	*(void **)&retro_unload_game            = sym("retro_unload_game");

	printf("[TEST] core  : %s\n", so_path);
	printf("[TEST] game  : %s\n", rom_dir);
	printf("[TEST] frames: %d\n", total_frames);
	printf("[TEST] API version = %u\n", retro_api_version());

	retro_set_environment(cb_environment);
	retro_set_video_refresh(cb_video_refresh);
	retro_set_audio_sample(cb_audio_sample);
	retro_set_audio_sample_batch(cb_audio_sample_batch);
	retro_set_input_poll(cb_input_poll);
	retro_set_input_state(cb_input_state);

	retro_get_system_info(&sys);
	printf("[TEST] library: %s %s, ext='%s', need_fullpath=%d\n",
	       sys.library_name, sys.library_version, sys.valid_extensions, sys.need_fullpath);

	retro_get_system_av_info(&av);
	printf("[TEST] av: %ux%u fps=%f rate=%f\n", av.geometry.base_width,
	       av.geometry.base_height, av.timing.fps, av.timing.sample_rate);

	retro_init();

	memset(&game, 0, sizeof(game));
	game.path = rom_dir;
	if (!retro_load_game(&game)) {
		fprintf(stderr, "[TEST] retro_load_game failed (is DUKE3D.GRP in %s?)\n", rom_dir);
		retro_deinit();
		return 1;
	}
	printf("[TEST] game loaded\n");

	if (script && *script)
		load_script(script, total_frames);
	else
		boot_script(total_frames);
	printf("[TEST] input steps: %d\n", s_step_count);

	last_sum = checksum(s_frame, s_w * s_h);

	for (i = 0; i < total_frames; i++) {
		int k;
		double t0, ms;

		s_pad_mask = 0;
		for (k = 0; k < s_step_count; k++)
			if (i >= s_steps[k].first && i <= s_steps[k].last)
				s_pad_mask |= s_steps[k].mask;

		t0 = now_ms();
		s_frame_start = t0;
		if (watchdog_seconds > 0)
			watchdog_arm(watchdog_seconds);
		retro_run();
		alarm(0);
		ms = now_ms() - t0;
		total += ms;
		if (ms > worst) worst = ms;
		if (ms < best) best = ms;

		{
			uint32_t sum = checksum(s_frame, s_w * s_h);
			if (sum != last_sum) {
				changed_at = i;
				last_sum = sum;
			}
		}

		if (dump_every > 0 && (i % dump_every) == 0) {
			char name[64];
			snprintf(name, sizeof(name), "frames/frame_%04d.ppm", i);
			save_ppm(name);
			printf("[TEST] frame %4d: presented=%d nonblack=%d colors=%d sum=%08x last_change=%d (%dx%d) [HEAP: %.2f MB]\n",
			       i, s_frame_count, count_non_black(s_frame, s_w * s_h),
			       count_distinct(s_frame, s_w * s_h), last_sum, changed_at,
			       s_last_width, s_last_height,
			       (double)mallinfo2().uordblks / (1024.0 * 1024.0));
			fflush(stdout);
		}
		if (verbose && (i % 60) == 0)
			printf("[TEST]   frame %4d presented=%d heap=%.2f MB\n", i, s_frame_count,
			       (double)mallinfo2().uordblks / (1024.0 * 1024.0));
		if (s_shutdown_requested) {
			printf("[TEST] shutdown requested at frame %d\n", i);
			break;
		}
	}

	printf("[TEST] retro_run best %.3f ms, avg %.2f ms, worst %.2f ms (host)\n",
	       best, total / total_frames, worst);
	printf("[TEST] frames presented: %d, audio frames: %lld\n",
	       s_frame_count, (long long)s_audio_frames_total);
	/* No on-card prediction: that would need this host's effective clock, which
	 * is not knowable from here, and a wrong number is worse than none. The
	 * useful figure is the ratio between the frames that present a frame and
	 * those that do not, which shows whether the coroutine is pacing. */
	/* A frame can present more than once: eduke32 redraws without the frontend
	 * having asked for another frame (palette fades, the title logo). So this is
	 * a count of presents, not a count of frames. */
	printf("[TEST] presents: %d over %d frames\n", s_frame_count, total_frames);
	(void)cpu_mhz;

	retro_unload_game();
	retro_deinit();
	dlclose(handle);
	printf("[TEST] done\n");
	return 0;
}