// sdl.cc calls into the virtual grid and arc (sdl_monome.cc), which this unit
// does not test; tests/matron/screen_sdl_monome does.

#include "hardware/screen/sdl_monome.h"

void sdl_monome_conf_defaults(sdl_monome_conf_t *conf) { *conf = sdl_monome_conf_t{}; }
void sdl_monome_config(sdl_monome_conf_t *, lua_State *, int) {}
void sdl_monome_init(const sdl_monome_conf_t *) {}
void sdl_monome_open(SDL_Keymod, SDL_Window *) {}
bool sdl_monome_handle_event(const SDL_Event *) { return false; }
void sdl_monome_present(void) {}
void sdl_monome_close(void) {}
