#include <ctype.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <cairo.h>

#include "event_types.h"
#include "events.h"
#include "hardware/io.h"
#include "hardware/screen/screens.h"

typedef struct _screen_sdl_priv {
    SDL_Window *window;
    SDL_Surface *window_surface;
    SDL_Surface *draw_surface;
    pthread_t render_thread;
    bool render_running;
    bool thread_started;

    // input config (set from the add_io options `input` subtable; see
    // screen_sdl_config). grid_mod is the modifier that activates the enc/key
    // grid; KMOD_NONE means the grid is always active (no modifier needed).
    SDL_Keymod grid_mod;
    int enc_step;            // steps per encoder tap
    SDL_Scancode key_sc[3];  // K1..K3
    SDL_Scancode enc_dn[3];  // E1..E3 decrement
    SDL_Scancode enc_up[3];  // E1..E3 increment
} screen_sdl_priv_t;

// guards access to draw_surface, shared between the screen-events thread
// (which repaints it on screen.update via the cairo paint path) and the
// render thread below (which blits/presents it to the window).
static pthread_mutex_t sdl_lock = PTHREAD_MUTEX_INITIALIZER;

static int screen_sdl_config(matron_io_t *io, lua_State *l);
static int screen_sdl_setup(matron_io_t *io);
static void screen_sdl_destroy(matron_io_t *io);
static void screen_sdl_paint(matron_fb_t *fb);
static void screen_sdl_bind(matron_fb_t *fb, cairo_surface_t *surface);
static void *screen_sdl_render_loop(void *data);

static void screen_sdl_surface_destroy(void *priv);
static cairo_surface_t *screen_sdl_surface_create(screen_sdl_priv_t *priv);

static SDL_Rect windowSize = {
    0,   // .x
    0,   // .y
    512, // .w
    256, // .h
};

screen_ops_t screen_sdl_ops = {
    {
        // .io_ops
        "screen:sdl",               // .name
        IO_SCREEN,                  // .type
        sizeof(screen_sdl_priv_t),  // .data_size
        screen_sdl_config,          // .config
        screen_sdl_setup,           // .setup
        screen_sdl_destroy,         // .destroy
    },
    screen_sdl_paint, // .paint
    screen_sdl_bind,  // .bind
};

static void screen_sdl_input_defaults(screen_sdl_priv_t *priv) {
    priv->grid_mod = KMOD_ALT;
    priv->enc_step = 1;
    priv->key_sc[0] = SDL_SCANCODE_1;
    priv->key_sc[1] = SDL_SCANCODE_2;
    priv->key_sc[2] = SDL_SCANCODE_3;
    priv->enc_dn[0] = SDL_SCANCODE_Q;
    priv->enc_up[0] = SDL_SCANCODE_W;
    priv->enc_dn[1] = SDL_SCANCODE_A;
    priv->enc_up[1] = SDL_SCANCODE_S;
    priv->enc_dn[2] = SDL_SCANCODE_Z;
    priv->enc_up[2] = SDL_SCANCODE_X;
}

// Resolve a key name (e.g. "q", "1", "Left") to an SDL scancode. SDL expects
// names like "Q"/"Left"; accept lowercase letters too by retrying uppercased.
// Returns fallback (and warns) if the name is unknown.
static SDL_Scancode screen_sdl_scancode_from_name(const char *name, SDL_Scancode fallback) {
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        char up[64];
        size_t i = 0;
        for (; name[i] != '\0' && i < sizeof(up) - 1; i++) {
            up[i] = (char)toupper((unsigned char)name[i]);
        }
        up[i] = '\0';
        sc = SDL_GetScancodeFromName(up);
    }
    if (sc == SDL_SCANCODE_UNKNOWN) {
        fprintf(stderr, "WARN (screen:sdl) unknown key name '%s', keeping default\n", name);
        return fallback;
    }
    return sc;
}

// Optionally read `input = {...}` from the add_io options table (arg 2):
//   modifier = 'alt'|'ctrl'|'gui'|'shift'|'none'   (grid activator)
//   enc_step = <int>                               (steps per encoder tap)
//   keys     = {'1','2','3'}                        (K1..K3)
//   enc1/2/3 = {'<down>','<up>'}                    (encoder rebinds)
// Any field may be omitted; defaults match the hardware-like layout.
int screen_sdl_config(matron_io_t *io, lua_State *l) {
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)io->data;
    screen_sdl_input_defaults(priv);

    if (!lua_istable(l, 2)) {
        return 0;
    }
    lua_getfield(l, 2, "input");
    if (!lua_istable(l, -1)) {
        lua_pop(l, 1);
        return 0;
    }
    int in = lua_gettop(l);

    lua_getfield(l, in, "modifier");
    if (lua_isstring(l, -1)) {
        const char *m = lua_tostring(l, -1);
        if (strcmp(m, "alt") == 0) {
            priv->grid_mod = KMOD_ALT;
        } else if (strcmp(m, "ctrl") == 0) {
            priv->grid_mod = KMOD_CTRL;
        } else if (strcmp(m, "gui") == 0) {
            priv->grid_mod = KMOD_GUI;
        } else if (strcmp(m, "shift") == 0) {
            priv->grid_mod = KMOD_SHIFT;
        } else if (strcmp(m, "none") == 0) {
            priv->grid_mod = KMOD_NONE; // grid always active
        } else {
            fprintf(stderr, "WARN (screen:sdl) unknown modifier '%s', keeping 'alt'\n", m);
        }
    }
    lua_pop(l, 1);

    lua_getfield(l, in, "enc_step");
    if (lua_isnumber(l, -1)) {
        int s = (int)lua_tointeger(l, -1);
        if (s >= 1 && s <= 64) {
            priv->enc_step = s;
        } else {
            fprintf(stderr, "WARN (screen:sdl) enc_step %d out of range [1,64], keeping %d\n", s, priv->enc_step);
        }
    }
    lua_pop(l, 1);

    lua_getfield(l, in, "keys");
    if (lua_istable(l, -1)) {
        for (int i = 0; i < 3; i++) {
            lua_rawgeti(l, -1, i + 1);
            if (lua_isstring(l, -1)) {
                priv->key_sc[i] = screen_sdl_scancode_from_name(lua_tostring(l, -1), priv->key_sc[i]);
            }
            lua_pop(l, 1);
        }
    }
    lua_pop(l, 1);

    const char *enc_field[3] = {"enc1", "enc2", "enc3"};
    for (int i = 0; i < 3; i++) {
        lua_getfield(l, in, enc_field[i]);
        if (lua_istable(l, -1)) {
            lua_rawgeti(l, -1, 1);
            if (lua_isstring(l, -1)) {
                priv->enc_dn[i] = screen_sdl_scancode_from_name(lua_tostring(l, -1), priv->enc_dn[i]);
            }
            lua_pop(l, 1);
            lua_rawgeti(l, -1, 2);
            if (lua_isstring(l, -1)) {
                priv->enc_up[i] = screen_sdl_scancode_from_name(lua_tostring(l, -1), priv->enc_up[i]);
            }
            lua_pop(l, 1);
        }
        lua_pop(l, 1);
    }

    lua_pop(l, 1); // input table
    return 0;
}

int screen_sdl_setup(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)io->data;

    // Only the CPU-side draw surface (and the cairo context over it) is created
    // here; the matron screen code binds to fb->surface immediately after setup
    // returns. All SDL *video* calls (init, window, present) are confined to the
    // render thread below — SDL requires the thread that creates a window to be
    // the one that pumps its events and presents it, otherwise the window never
    // composites and stays blank on X11/Wayland.
    fb->surface = screen_sdl_surface_create(priv);
    if (!fb->surface) {
        fprintf(stderr, "ERROR (%s) failed to create surface\n", io->ops->name);
        return -1;
    }
    fb->cairo = cairo_create(fb->surface);

    priv->render_running = true;
    if (pthread_create(&priv->render_thread, NULL, &screen_sdl_render_loop, priv) != 0) {
        fprintf(stderr, "ERROR (%s) failed to start render thread\n", io->ops->name);
        priv->render_running = false;
    } else {
        priv->thread_started = true;
    }
    return 0;
}

static void screen_sdl_destroy(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)io->data;
    if (priv->thread_started) {
        priv->render_running = false;
        pthread_join(priv->render_thread, NULL);
        priv->thread_started = false;
    }
    cairo_destroy(fb->cairo);
    cairo_surface_destroy(fb->surface);
}

// Runs on the screen-events thread (via screen.update). Copy the bound matron
// surface into draw_surface's pixels; the render thread does the SDL present.
static void screen_sdl_paint(matron_fb_t *fb) {
    pthread_mutex_lock(&sdl_lock);
    cairo_paint(fb->cairo);
    cairo_surface_flush(fb->surface);
    pthread_mutex_unlock(&sdl_lock);
}

// Map an SDL scancode to a linux evdev KEY_* code for passthrough to the Lua
// `keyboard` module (which speaks evdev codes). Returns 0 for unmapped keys.
static uint16_t sdl_scancode_to_evdev(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_A: return KEY_A;
    case SDL_SCANCODE_B: return KEY_B;
    case SDL_SCANCODE_C: return KEY_C;
    case SDL_SCANCODE_D: return KEY_D;
    case SDL_SCANCODE_E: return KEY_E;
    case SDL_SCANCODE_F: return KEY_F;
    case SDL_SCANCODE_G: return KEY_G;
    case SDL_SCANCODE_H: return KEY_H;
    case SDL_SCANCODE_I: return KEY_I;
    case SDL_SCANCODE_J: return KEY_J;
    case SDL_SCANCODE_K: return KEY_K;
    case SDL_SCANCODE_L: return KEY_L;
    case SDL_SCANCODE_M: return KEY_M;
    case SDL_SCANCODE_N: return KEY_N;
    case SDL_SCANCODE_O: return KEY_O;
    case SDL_SCANCODE_P: return KEY_P;
    case SDL_SCANCODE_Q: return KEY_Q;
    case SDL_SCANCODE_R: return KEY_R;
    case SDL_SCANCODE_S: return KEY_S;
    case SDL_SCANCODE_T: return KEY_T;
    case SDL_SCANCODE_U: return KEY_U;
    case SDL_SCANCODE_V: return KEY_V;
    case SDL_SCANCODE_W: return KEY_W;
    case SDL_SCANCODE_X: return KEY_X;
    case SDL_SCANCODE_Y: return KEY_Y;
    case SDL_SCANCODE_Z: return KEY_Z;
    case SDL_SCANCODE_1: return KEY_1;
    case SDL_SCANCODE_2: return KEY_2;
    case SDL_SCANCODE_3: return KEY_3;
    case SDL_SCANCODE_4: return KEY_4;
    case SDL_SCANCODE_5: return KEY_5;
    case SDL_SCANCODE_6: return KEY_6;
    case SDL_SCANCODE_7: return KEY_7;
    case SDL_SCANCODE_8: return KEY_8;
    case SDL_SCANCODE_9: return KEY_9;
    case SDL_SCANCODE_0: return KEY_0;
    case SDL_SCANCODE_RETURN: return KEY_ENTER;
    case SDL_SCANCODE_ESCAPE: return KEY_ESC;
    case SDL_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
    case SDL_SCANCODE_TAB: return KEY_TAB;
    case SDL_SCANCODE_SPACE: return KEY_SPACE;
    case SDL_SCANCODE_MINUS: return KEY_MINUS;
    case SDL_SCANCODE_EQUALS: return KEY_EQUAL;
    case SDL_SCANCODE_LEFTBRACKET: return KEY_LEFTBRACE;
    case SDL_SCANCODE_RIGHTBRACKET: return KEY_RIGHTBRACE;
    case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
    case SDL_SCANCODE_NONUSHASH: return KEY_BACKSLASH;
    case SDL_SCANCODE_NONUSBACKSLASH: return KEY_102ND;
    case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
    case SDL_SCANCODE_GRAVE: return KEY_GRAVE;
    case SDL_SCANCODE_COMMA: return KEY_COMMA;
    case SDL_SCANCODE_PERIOD: return KEY_DOT;
    case SDL_SCANCODE_SLASH: return KEY_SLASH;
    case SDL_SCANCODE_CAPSLOCK: return KEY_CAPSLOCK;
    case SDL_SCANCODE_LSHIFT: return KEY_LEFTSHIFT;
    case SDL_SCANCODE_RSHIFT: return KEY_RIGHTSHIFT;
    case SDL_SCANCODE_LCTRL: return KEY_LEFTCTRL;
    case SDL_SCANCODE_RCTRL: return KEY_RIGHTCTRL;
    case SDL_SCANCODE_LALT: return KEY_LEFTALT;
    case SDL_SCANCODE_RALT: return KEY_RIGHTALT;
    case SDL_SCANCODE_LEFT: return KEY_LEFT;
    case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
    case SDL_SCANCODE_UP: return KEY_UP;
    case SDL_SCANCODE_DOWN: return KEY_DOWN;
    case SDL_SCANCODE_HOME: return KEY_HOME;
    case SDL_SCANCODE_END: return KEY_END;
    case SDL_SCANCODE_PAGEUP: return KEY_PAGEUP;
    case SDL_SCANCODE_PAGEDOWN: return KEY_PAGEDOWN;
    case SDL_SCANCODE_INSERT: return KEY_INSERT;
    case SDL_SCANCODE_DELETE: return KEY_DELETE;
    case SDL_SCANCODE_F1: return KEY_F1;
    case SDL_SCANCODE_F2: return KEY_F2;
    case SDL_SCANCODE_F3: return KEY_F3;
    case SDL_SCANCODE_F4: return KEY_F4;
    case SDL_SCANCODE_F5: return KEY_F5;
    case SDL_SCANCODE_F6: return KEY_F6;
    case SDL_SCANCODE_F7: return KEY_F7;
    case SDL_SCANCODE_F8: return KEY_F8;
    case SDL_SCANCODE_F9: return KEY_F9;
    case SDL_SCANCODE_F10: return KEY_F10;
    case SDL_SCANCODE_F11: return KEY_F11;
    case SDL_SCANCODE_F12: return KEY_F12;
    default: return 0;
    }
}

// Is this scancode part of the configured enc/key control grid? If so, report
// whether it's a key (vs encoder), the 1-based control number, and (encoders)
// the turn direction. Keys win over encoders if the user maps the same scancode
// to both.
static bool screen_sdl_grid_lookup(const screen_sdl_priv_t *priv, SDL_Scancode sc,
                                   bool *is_key, int *n, int *delta) {
    for (int i = 0; i < 3; i++) {
        if (sc == priv->key_sc[i]) {
            *is_key = true;
            *n = i + 1;
            return true;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (sc == priv->enc_dn[i]) {
            *is_key = false;
            *n = i + 1;
            *delta = -1;
            return true;
        }
        if (sc == priv->enc_up[i]) {
            *is_key = false;
            *n = i + 1;
            *delta = 1;
            return true;
        }
    }
    return false;
}

// Translate one SDL key event. With the grid modifier held (priv->grid_mod;
// KMOD_NONE = always active), the control grid drives the 3 encoders + 3 keys;
// everything else (and the grid without the modifier) passes through to the Lua
// keyboard module. grid_consumed[] ensures that if we ate a key's press, we also
// eat its release (delivering key-up, and never leaking a spurious release into
// the keyboard module).
static void screen_sdl_handle_key(const screen_sdl_priv_t *priv, const SDL_KeyboardEvent *ke) {
    static bool grid_consumed[SDL_NUM_SCANCODES] = {false};
    SDL_Scancode sc = ke->keysym.scancode;
    if (sc <= 0 || sc >= SDL_NUM_SCANCODES) {
        return;
    }
    bool down = (ke->type == SDL_KEYDOWN);
    bool repeat = ke->repeat != 0;
    bool mod_ok = (priv->grid_mod == KMOD_NONE) || (ke->keysym.mod & priv->grid_mod);

    bool is_key = false;
    int n = 0, delta = 0;
    if (screen_sdl_grid_lookup(priv, sc, &is_key, &n, &delta)) {
        if (down && mod_ok) {
            if (is_key) {
                if (!repeat) {
                    union event_data *ev = event_data_new(EVENT_KEY);
                    ev->key.n = n;
                    ev->key.val = 1;
                    event_post(ev);
                }
            } else {
                // Encoders fire one step per press (repeats included, so holding
                // scrolls at the OS key-repeat rate). EVENT_SDL_ENC dispatches
                // straight to encoders.callback, bypassing the sens/accel
                // accumulator — so +/-1 is exactly one step on every encoder,
                // including the menu's deliberately coarse E1 (sens 8).
                union event_data *ev = event_data_new(EVENT_SDL_ENC);
                ev->enc.n = n;
                ev->enc.delta = priv->enc_step * delta;
                event_post(ev);
            }
            grid_consumed[sc] = true;
            return;
        }
        if (!down && grid_consumed[sc]) {
            if (is_key) {
                union event_data *ev = event_data_new(EVENT_KEY);
                ev->key.n = n;
                ev->key.val = 0;
                event_post(ev);
            }
            grid_consumed[sc] = false;
            return;
        }
    }

    // passthrough: feed the keyboard module like an evdev HID keyboard
    uint16_t code = sdl_scancode_to_evdev(sc);
    if (code == 0) {
        return;
    }
    union event_data *ev = event_data_new(EVENT_SDL_KEY);
    ev->sdl_key.code = code;
    ev->sdl_key.value = down ? (repeat ? 2 : 1) : 0;
    event_post(ev);
}

static void *screen_sdl_render_loop(void *data) {
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)data;

    // SDL would otherwise install SIGINT/SIGTERM handlers that just queue an
    // SDL_QUIT event, which swallows the SIGTERM _norns.terminate() relies on
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "ERROR (screen:sdl) SDL_Init failed: %s\n", SDL_GetError());
        priv->render_running = false;
        return NULL;
    }
    priv->window = SDL_CreateWindow("matron",
                                    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                    windowSize.w, windowSize.h,
                                    SDL_WINDOW_SHOWN);
    if (!priv->window) {
        fprintf(stderr, "ERROR (screen:sdl) SDL_CreateWindow failed: %s\n", SDL_GetError());
        priv->render_running = false;
        return NULL;
    }
    priv->window_surface = SDL_GetWindowSurface(priv->window);
    if (!priv->window_surface) {
        fprintf(stderr, "ERROR (screen:sdl) SDL_GetWindowSurface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(priv->window);
        priv->window = NULL;
        priv->render_running = false;
        return NULL;
    }

    // Continuously pump SDL events and present the latest frame. Pumping keeps
    // the window mapped/composited; presenting every frame keeps the last drawn
    // content visible even when nothing calls screen.update (e.g. sitting in a
    // menu), mirroring how the hardware OLED is continuously refreshed.
    static const struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 16666666, // 60Hz
    };
    while (priv->render_running) {
        // SDL_PollEvent pumps the queue; events must be serviced on this thread
        // (the one that owns the window). Keyboard input is gated on window
        // focus by SDL automatically.
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                screen_sdl_handle_key(priv, &e.key);
            }
        }
        pthread_mutex_lock(&sdl_lock);
        SDL_BlitScaled(priv->draw_surface, NULL, priv->window_surface, NULL);
        SDL_UpdateWindowSurface(priv->window);
        pthread_mutex_unlock(&sdl_lock);
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }

    SDL_DestroyWindow(priv->window);
    priv->window = NULL;
    priv->window_surface = NULL;
    return NULL;
}

static void screen_sdl_bind(matron_fb_t *fb, cairo_surface_t *surface) {
    cairo_set_operator(fb->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(fb->cairo, surface, 0, 0);
}

static void screen_sdl_surface_destroy(void *data) {
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)data;

    if (priv == NULL) {
        return;
    }
    // The window is owned and destroyed by the render thread (joined before we
    // get here); only the CPU draw surface and priv remain to free.
    SDL_FreeSurface(priv->draw_surface);
    free(priv);
}

static cairo_surface_t *screen_sdl_surface_create(screen_sdl_priv_t *priv) {
    cairo_surface_t *surface;

    priv->window = NULL;
    priv->window_surface = NULL;
    priv->thread_started = false;

    // CPU-only surface; SDL_CreateRGBSurface does not require the video
    // subsystem to be initialized (that happens on the render thread).
    priv->draw_surface = SDL_CreateRGBSurface(0,
                                              128, 64,
                                              16, 0xf800, 0x000007e0, 0x0000001f,
                                              0);
    if (!priv->draw_surface) {
        fprintf(stderr, "ERROR (screen:sdl) failed to create draw surface: %s\n", SDL_GetError());
        return NULL;
    }
    surface = cairo_image_surface_create_for_data((unsigned char *)priv->draw_surface->pixels,
                                                  CAIRO_FORMAT_RGB16_565, priv->draw_surface->w, priv->draw_surface->h,
                                                  cairo_format_stride_for_width(CAIRO_FORMAT_RGB16_565, priv->draw_surface->w));
    cairo_surface_set_user_data(surface, NULL, priv, &screen_sdl_surface_destroy);

    return surface;
}
