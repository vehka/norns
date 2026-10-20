#pragma once

// Virtual monome grid and arc for the desktop (screen:sdl), each in a window
// of its own. Scripts see them as ordinary monome devices.

#include <stdbool.h>

#include <SDL2/SDL.h>

#include <lualib.h>

typedef struct {
    bool grid;      // offer the virtual grid
    int grid_cols;  // 1 to 16
    int grid_rows;  // 1 to 16
    int grid_size;  // key pitch in px, at the window's first size
    bool arc;       // offer the virtual arc
    int arc_rings;  // rings shown, 1 to 4
    int arc_size;   // side of a ring's square in px, at the first size
    int arc_wheel;  // encoder ticks per mouse wheel step
} sdl_monome_conf_t;

extern void sdl_monome_conf_defaults(sdl_monome_conf_t *conf);
// read `grid` and `arc` from the add_io options table at index `opts`
extern void sdl_monome_config(sdl_monome_conf_t *conf, lua_State *l, int opts);

// create the devices and announce them to lua (any thread, before the windows)
extern void sdl_monome_init(const sdl_monome_conf_t *conf);

// the rest runs on the thread that owns SDL video.
// hold_mod: the modifier that makes a click hold a key until it is released,
// KMOD_NONE for none. The windows open below `screen` (may be NULL)
extern void sdl_monome_open(SDL_Keymod hold_mod, SDL_Window *screen);
// look at an SDL event. true if it belonged to a grid or arc window and needs
// nothing more; key events are never consumed
extern bool sdl_monome_handle_event(const SDL_Event *e);
// redraw the windows whose contents changed
extern void sdl_monome_present(void);
extern void sdl_monome_close(void);
