/*
 * Controls for the Data Frog SF2000 / GB300 -- shared declarations.
 *
 * SF2K_PAD_* is a mask of libretro joypad button IDs. It is spelled out here
 * rather than pulled from libretro.h so that this header can be included by the
 * build tooling without dragging in the whole libretro API.
 */
#ifndef SF2000_INPUT_H
#define SF2000_INPUT_H

#include <stdint.h>

#define SF2K_PAD_NONE   0x0000u
#define SF2K_PAD_UP     0x0001u
#define SF2K_PAD_DOWN   0x0002u
#define SF2K_PAD_LEFT   0x0004u
#define SF2K_PAD_RIGHT  0x0008u
#define SF2K_PAD_A      0x0010u
#define SF2K_PAD_B      0x0020u
#define SF2K_PAD_X      0x0040u
#define SF2K_PAD_Y      0x0080u
#define SF2K_PAD_L      0x0100u
#define SF2K_PAD_R      0x0200u
#define SF2K_PAD_START  0x0400u
#define SF2K_PAD_SELECT 0x0800u

#ifdef __cplusplus
extern "C" {
#endif

void sf2k_input_init(void);

/* Applies an SF2K_PAD_* mask for one frame. Callbacks for the mouse follow the
 * same frame boundary, so input and rendering stay in step. */
void sf2k_input_poll(unsigned buttons);

/* Drops every key. Used when the game is torn down so nothing stays latched. */
void sf2k_input_release_all(void);

/* Prints the pad->key mapping alongside the cfg_path bindings file. */
void sf2k_input_print_controls(const char *cfg_path);

#ifdef __cplusplus
}
#endif

#endif /* SF2000_INPUT_H */