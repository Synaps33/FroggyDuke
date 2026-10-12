/*
 * Controls for the Data Frog SF2000 / GB300.
 */

#include "sf2000_input.h"

#include <string.h>
#include <stdio.h>

#include "sdl2_shim.h"

extern int sf2k_is_in_menu(void);

static int pressed(unsigned mask, unsigned button)
{
    return (mask & button) != 0;
}

static int s_prev_in_menu = 1;

void sf2k_input_init(void)
{
    s_prev_in_menu = 1;
}

void sf2k_input_poll(unsigned buttons)
{
    int in_menu = sf2k_is_in_menu();

    if (in_menu != s_prev_in_menu) {
        sf2k_sdl_send_key(SDL_SCANCODE_RETURN, 0);
        sf2k_sdl_send_key(SDL_SCANCODE_ESCAPE, 0);
        sf2k_sdl_send_key(SDL_SCANCODE_E,      0);
        sf2k_sdl_send_key(SDL_SCANCODE_A,      0);
        s_prev_in_menu = in_menu;
    }

    /* D-pad: Up, Down, Left, Right */
    sf2k_sdl_send_key(SDL_SCANCODE_UP,    pressed(buttons, SF2K_PAD_UP));
    sf2k_sdl_send_key(SDL_SCANCODE_DOWN,  pressed(buttons, SF2K_PAD_DOWN));
    sf2k_sdl_send_key(SDL_SCANCODE_LEFT,  pressed(buttons, SF2K_PAD_LEFT));
    sf2k_sdl_send_key(SDL_SCANCODE_RIGHT, pressed(buttons, SF2K_PAD_RIGHT));

    /* L: Fire (Space in game, disabled in menu) */
    sf2k_sdl_send_key(SDL_SCANCODE_SPACE, in_menu ? 0 : pressed(buttons, SF2K_PAD_L));

    /* R: Strafe modifier (LAlt in game, disabled in menu) */
    sf2k_sdl_send_key(SDL_SCANCODE_LALT,  in_menu ? 0 : pressed(buttons, SF2K_PAD_R));

    /* A: In menu -> Return (Accept), In game -> E (Open) */
    if (in_menu) {
        sf2k_sdl_send_key(SDL_SCANCODE_RETURN, pressed(buttons, SF2K_PAD_A));
        sf2k_sdl_send_key(SDL_SCANCODE_E,      0);
    } else {
        sf2k_sdl_send_key(SDL_SCANCODE_E,      pressed(buttons, SF2K_PAD_A));
        sf2k_sdl_send_key(SDL_SCANCODE_RETURN, 0);
    }

    /* B: In menu -> Escape (Back), In game -> A (Jump) */
    if (in_menu) {
        sf2k_sdl_send_key(SDL_SCANCODE_ESCAPE, pressed(buttons, SF2K_PAD_B) || pressed(buttons, SF2K_PAD_START));
        sf2k_sdl_send_key(SDL_SCANCODE_A,      0);
    } else {
        sf2k_sdl_send_key(SDL_SCANCODE_A,      pressed(buttons, SF2K_PAD_B));
        sf2k_sdl_send_key(SDL_SCANCODE_ESCAPE, pressed(buttons, SF2K_PAD_START));
    }

    /* X: Crouch (C) */
    sf2k_sdl_send_key(SDL_SCANCODE_C, in_menu ? 0 : pressed(buttons, SF2K_PAD_X));

    /* Y: Next Weapon (Y) */
    sf2k_sdl_send_key(SDL_SCANCODE_Y, in_menu ? 0 : pressed(buttons, SF2K_PAD_Y));

    /* Select: Map (Tab) */
    sf2k_sdl_send_key(SDL_SCANCODE_TAB, in_menu ? 0 : pressed(buttons, SF2K_PAD_SELECT));
}

void sf2k_input_release_all(void)
{
    sf2k_sdl_send_key(SDL_SCANCODE_UP,     0);
    sf2k_sdl_send_key(SDL_SCANCODE_DOWN,   0);
    sf2k_sdl_send_key(SDL_SCANCODE_LEFT,   0);
    sf2k_sdl_send_key(SDL_SCANCODE_RIGHT,  0);
    sf2k_sdl_send_key(SDL_SCANCODE_SPACE,  0);
    sf2k_sdl_send_key(SDL_SCANCODE_LALT,   0);
    sf2k_sdl_send_key(SDL_SCANCODE_RETURN, 0);
    sf2k_sdl_send_key(SDL_SCANCODE_ESCAPE, 0);
    sf2k_sdl_send_key(SDL_SCANCODE_E,      0);
    sf2k_sdl_send_key(SDL_SCANCODE_A,      0);
    sf2k_sdl_send_key(SDL_SCANCODE_C,      0);
    sf2k_sdl_send_key(SDL_SCANCODE_Y,      0);
    sf2k_sdl_send_key(SDL_SCANCODE_TAB,    0);
    sf2k_input_init();
}

void sf2k_input_print_controls(const char *cfg_path)
{
    (void)cfg_path;
}
