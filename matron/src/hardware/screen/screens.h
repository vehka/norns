#pragma once

#include "hardware/io.h"

#ifdef NORNS_DESKTOP
extern screen_ops_t screen_sdl_ops;
#endif

#ifdef HAVE_TERMUXGUI
extern screen_ops_t screen_tgui_ops;
// (re)open the window after it has been closed
extern void screen_tgui_show(void);
#endif
