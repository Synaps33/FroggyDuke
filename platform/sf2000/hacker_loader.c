/*
 * Hackerlog boot screen for eduke32 on the SF2000 / GB300.
 *
 * Same component the other SF2000 ports use (see brew-sf2000/src/hacker_loader.c
 * and re3-sf2000/libretro/hacker_loader.c): a matrix-green log panel drawn with
 * an 8x8 bitmap font while the core is still coming up, showing what the port
 * did at each step. It matters here more than usual, because the failure modes
 * this port has are all silent ones -- a missing DUKE3D.GRP, a chdir to the
 * wrong place, a frame that never gets presented -- and on a handheld with no
 * debug output the log is the only place any of that shows up.
 *
 * The whole panel is one 320x240 RGB565 buffer, which is the same buffer
 * geometry the game itself presents at, so no scaling happens on the way out.
 */
#include "hacker_loader.h"
#include "font8x8.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#define SCREEN_W        320
#define SCREEN_H        240
#define HACKER_MAX_LOGS 16
#define HACKER_LINE_MAX 46

#define COLOR_BLACK        0x0000
#define COLOR_SCANLINE     0x0842
#define COLOR_MATRIX_GREEN 0x07E0
#define COLOR_AMBER        0xFFE0
#define COLOR_WHITE        0xFFFF
#define COLOR_CYAN         0x07FF

static uint16_t s_loader_fb[SCREEN_W * SCREEN_H];
static retro_video_refresh_t s_video_cb = NULL;

static char s_hacker_logs[HACKER_MAX_LOGS][HACKER_LINE_MAX];
static int  s_hacker_log_count = 0;
static char s_rom_name[48];
static uint32_t s_boot_start_ms = 0;
static int  s_boot_active = 0;
static int  s_hacker_progress = 0;

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0)
        return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    return 0;
}

void hacker_loader_set_video_cb(retro_video_refresh_t cb)
{
    s_video_cb = cb;
}

static void draw_char8x8(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph;
    int row, col;

    if (c < 32 || c > 126)
        c = ' ';
    glyph = font8x8_basic[c - 32];

    for (row = 0; row < 8; row++) {
        int py = y + row;
        uint16_t *dst;
        uint8_t line;

        if (py < 0 || py >= SCREEN_H)
            continue;
        dst = &s_loader_fb[py * SCREEN_W + x];
        line = glyph[row];
        for (col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= SCREEN_W)
                continue;
            if (line & (0x80 >> col))
                dst[col] = fg;
            else if (bg != 0)
                dst[col] = bg;
        }
    }
}

static void draw_text(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    int cur_x = x;

    if (!str)
        return;
    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            y += 10;
        } else {
            draw_char8x8(cur_x, y, *str, fg, bg);
            cur_x += 8;
            if (cur_x + 8 > SCREEN_W)
                break;
        }
        str++;
    }
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int row;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (row = 0; row < h; row++) {
        uint16_t *line = &s_loader_fb[(y + row) * SCREEN_W + x];
        int col;
        for (col = 0; col < w; col++)
            line[col] = color;
    }
}

static void hacker_render_screen(void)
{
    char line[HACKER_LINE_MAX];
    int log_start_y = 40;
    int i;

    for (i = 0; i < SCREEN_H; i++) {
        uint16_t line_color = (i & 3) ? COLOR_BLACK : COLOR_SCANLINE;
        uint16_t *row = &s_loader_fb[i * SCREEN_W];
        int x;
        for (x = 0; x < SCREEN_W; x++)
            row[x] = line_color;
    }

    snprintf(line, sizeof(line), "ROM:  %.32s", s_rom_name[0] ? s_rom_name : "-");
    draw_text(8, 6, line, COLOR_CYAN, 0);

    draw_text(8, 18, "EDuke32 1.0 (SF2000 / GB300)", COLOR_AMBER, 0);
    fill_rect(8, 29, SCREEN_W - 16, 1, COLOR_SCANLINE);

    for (i = 0; i < s_hacker_log_count && i < HACKER_MAX_LOGS; i++) {
        int text_y = log_start_y + i * 9;
        uint16_t c = (i == s_hacker_log_count - 1) ? COLOR_WHITE : COLOR_MATRIX_GREEN;
        draw_text(8, text_y, s_hacker_logs[i], c, 0);
    }

    fill_rect(8, 196, SCREEN_W - 16, 1, COLOR_SCANLINE);

    snprintf(line, sizeof(line), "BOOTING: %3d%%", s_hacker_progress);
    draw_text(8, 200, line, COLOR_CYAN, 0);

    {
        int bar_x = 8, bar_y = 214, bar_w = SCREEN_W - 16, bar_h = 11;
        int max_fill = bar_w - 4;
        int fill_w = (s_hacker_progress * max_fill) / 100;

        fill_rect(bar_x, bar_y, bar_w, 1, COLOR_SCANLINE);
        fill_rect(bar_x, bar_y + bar_h - 1, bar_w, 1, COLOR_SCANLINE);
        fill_rect(bar_x, bar_y, 1, bar_h, COLOR_SCANLINE);
        fill_rect(bar_x + bar_w - 1, bar_y, 1, bar_h, COLOR_SCANLINE);
        if (fill_w > max_fill) fill_w = max_fill;
        if (fill_w > 0)
            fill_rect(bar_x + 2, bar_y + 2, fill_w, bar_h - 4, COLOR_MATRIX_GREEN);
    }
}

static void hacker_flush(void)
{
    hacker_render_screen();
    if (s_video_cb)
        s_video_cb(s_loader_fb, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint16_t));
}

void hacker_loader_init(const char *pak_path)
{
    s_boot_active = 1;
    s_hacker_log_count = 0;
    s_hacker_progress = 5;
    s_boot_start_ms = get_time_ms();
    s_rom_name[0] = '\0';

    if (pak_path && *pak_path) {
        const char *b = strrchr(pak_path, '/');
        b = b ? b + 1 : pak_path;
        snprintf(s_rom_name, sizeof(s_rom_name), "%.36s", b);
    } else {
        strcpy(s_rom_name, "ROM_STREAM");
    }

    hacker_loader_log("SYS", "JZ4775 MIPS32r2 core up", 5);
    hacker_loader_log("VFS", "MOUNT /dev/sdcard ... [OK]", 8);
}

void hacker_loader_log(const char *tag, const char *msg, int pct)
{
    char formatted[HACKER_LINE_MAX];
    uint32_t now, elapsed_ms, sec, msec;

    if (!s_boot_active)
        return;

    if (pct > s_hacker_progress) {
        s_hacker_progress = pct;
        if (s_hacker_progress > 100)
            s_hacker_progress = 100;
    }

    if (s_boot_start_ms == 0)
        s_boot_start_ms = get_time_ms();
    now = get_time_ms();
    elapsed_ms = (now >= s_boot_start_ms) ? (now - s_boot_start_ms) : 0;
    sec  = elapsed_ms / 1000;
    msec = (elapsed_ms % 1000) / 10;

    snprintf(formatted, sizeof(formatted), "[%u.%02us] [%-4.4s] %s",
             (unsigned)sec, (unsigned)msec, tag ? tag : "SYS", msg ? msg : "");

    if (s_hacker_log_count < HACKER_MAX_LOGS) {
        strncpy(s_hacker_logs[s_hacker_log_count], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[s_hacker_log_count][HACKER_LINE_MAX - 1] = '\0';
        s_hacker_log_count++;
    } else {
        memmove(&s_hacker_logs[0], &s_hacker_logs[1],
                (HACKER_MAX_LOGS - 1) * HACKER_LINE_MAX);
        strncpy(s_hacker_logs[HACKER_MAX_LOGS - 1], formatted, HACKER_LINE_MAX - 1);
        s_hacker_logs[HACKER_MAX_LOGS - 1][HACKER_LINE_MAX - 1] = '\0';
    }

    hacker_flush();
}

void hacker_loader_stop(void)
{
    if (!s_boot_active)
        return;
    s_hacker_progress = 100;
    hacker_flush();
    s_boot_active = 0;
}