// Virtual monome grid and arc for the desktop, each in an SDL window of its
// own next to the screen:sdl window. They are dev_monome devices with no
// libmonome handle (see dev_monome_new_virtual_grid), so scripts reach them
// through grid.connect() and arc.connect() like hardware.
//
// Grid: a click holds a key while the button is down, and dragging slides
// the press from key to key. A click with the hold modifier (the one that
// works the norns keys and encoders, alt by default) keeps the key down after
// the button comes up; more such clicks add keys, one on a held key lets it
// go, and letting go of the modifier releases them all.
//
// Arc: dragging around a ring turns it, ARC_TICKS to the turn, and so does
// the mouse wheel over it. The disc in the middle of a ring is its key, held
// the same way as a grid key.
//
// Threads: the refresh callbacks run on the lua thread and only copy the led
// levels under the lock. Everything else runs on the screen:sdl render thread,
// which owns the windows and posts the key and turn events.

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device/device_monome.h"
#include "event_types.h"
#include "events.h"
#include "hardware/screen/sdl_monome.h"

#define VG_MAX DEV_MONOME_VIRTUAL_MAX
#define ARC_RINGS 4
#define ARC_LEDS 64
// encoder ticks in one turn of a ring, as on the device
#define ARC_TICKS 1024
// shares of a ring's square: the key disc, and the middle part where a drag
// turns nothing (the angle jumps there)
#define ARC_KEY_R 0.22f
#define ARC_DEAD_R 0.08f
#define ARC_LED_R_IN 0.33f
#define ARC_LED_R_OUT 0.46f
// arc_map values that are not a led
#define ARC_MAP_KEY 254
#define ARC_MAP_NONE 255

// what holds a key down: it is down while either is set
enum { HELD_POINTER = 1, HELD_LATCH = 2 };

typedef struct {
    SDL_Window *win;
    uint32_t id;
    SDL_Surface *canvas; // drawn here, then blitted to the window
    bool redraw;
} sdl_monome_win_t;

typedef struct {
    struct dev_monome *dev;
    int cols, rows;
    uint8_t led[VG_MAX * VG_MAX]; // from the last refresh, under the lock
    bool led_dirty;
    // render thread only from here on
    sdl_monome_win_t w;
    uint8_t shown[VG_MAX * VG_MAX]; // levels being shown
    int pitch, ox, oy;              // key pitch and grid origin, px
    uint8_t held[VG_MAX * VG_MAX];  // HELD_* bits
    bool ptr_down;                  // a plain click is holding `ptr`
    int ptr;                        // key under it, -1 off the grid
} vgrid_t;

typedef struct {
    struct dev_monome *dev;
    int rings;
    uint8_t led[ARC_RINGS][ARC_LEDS]; // from the last refresh, under the lock
    bool led_dirty;
    // render thread only from here on
    sdl_monome_win_t w;
    uint8_t shown[ARC_RINGS][ARC_LEDS];
    int size, ox, oy; // side of a ring's square, origin of the first, px
    uint8_t *map;     // led (or ARC_MAP_*) under each pixel of a square
    int map_size;
    uint8_t held[ARC_RINGS];
    int key_ptr; // ring whose key a plain click holds, -1 for none
    int drag;    // ring being turned, -1 for none
    float drag_x, drag_y;
    float acc[ARC_RINGS]; // turn not yet sent, in ticks
    int wheel;
} varc_t;

static pthread_mutex_t sdl_monome_lock = PTHREAD_MUTEX_INITIALIZER;
static sdl_monome_conf_t sm_conf;
static vgrid_t vg;
static varc_t va;
static SDL_Keymod hold_mod = KMOD_NONE;
// the modifiers as of the event being handled. SDL_GetModState() is the
// state after all the events pumped so far, which can be ahead of a click.
static SDL_Keymod cur_mods = KMOD_NONE;

//--- configuration

void sdl_monome_conf_defaults(sdl_monome_conf_t *conf) {
    conf->grid = true;
    conf->grid_cols = 16;
    conf->grid_rows = 8;
    conf->grid_size = 40;
    conf->arc = false;
    conf->arc_rings = 4;
    conf->arc_size = 160;
    conf->arc_wheel = 16;
}

// an integer field of the table at `t`, kept as it is when missing or out of range
static void conf_int(lua_State *l, int t, const char *what, const char *key, int *val, int lo, int hi) {
    lua_getfield(l, t, key);
    if (lua_isnumber(l, -1)) {
        int v = (int)lua_tointeger(l, -1);
        if (v >= lo && v <= hi) {
            *val = v;
        } else {
            fprintf(stderr, "WARN (screen:sdl) %s.%s %d out of range [%d,%d], keeping %d\n", what, key, v, lo, hi,
                    *val);
        }
    }
    lua_pop(l, 1);
}

// `key` = true | false | { fields }; a table turns the device on
static int conf_device(lua_State *l, int opts, const char *key, bool *on) {
    lua_getfield(l, opts, key);
    if (lua_isboolean(l, -1)) {
        *on = lua_toboolean(l, -1);
    } else if (lua_istable(l, -1)) {
        *on = true;
        return lua_gettop(l);
    }
    lua_pop(l, 1);
    return 0;
}

//   grid = true | false | { cols = 16, rows = 8, size = 40 }
//   arc  = true | false | { rings = 4, size = 160, wheel = 16 }
void sdl_monome_config(sdl_monome_conf_t *conf, lua_State *l, int opts) {
    int t = conf_device(l, opts, "grid", &conf->grid);
    if (t) {
        conf_int(l, t, "grid", "cols", &conf->grid_cols, 1, VG_MAX);
        conf_int(l, t, "grid", "rows", &conf->grid_rows, 1, VG_MAX);
        conf_int(l, t, "grid", "size", &conf->grid_size, 8, 200);
        lua_pop(l, 1);
    }
    t = conf_device(l, opts, "arc", &conf->arc);
    if (t) {
        conf_int(l, t, "arc", "rings", &conf->arc_rings, 1, ARC_RINGS);
        conf_int(l, t, "arc", "size", &conf->arc_size, 48, 800);
        conf_int(l, t, "arc", "wheel", &conf->arc_wheel, 1, 256);
        lua_pop(l, 1);
    }
}

//--- devices (lua thread)

static void vgrid_refresh(struct dev_monome *md) {
    pthread_mutex_lock(&sdl_monome_lock);
    dev_monome_virtual_grid_levels(md, vg.led);
    vg.led_dirty = true;
    pthread_mutex_unlock(&sdl_monome_lock);
}

static void varc_refresh(struct dev_monome *md) {
    pthread_mutex_lock(&sdl_monome_lock);
    memcpy(va.led, md->data, sizeof(va.led));
    va.led_dirty = true;
    pthread_mutex_unlock(&sdl_monome_lock);
}

static void post_monome_add(struct dev_monome *md) {
    union event_data *ev = event_data_new(EVENT_MONOME_ADD);
    ev->monome_add.dev = md;
    event_post(ev);
}

// The devices live until matron exits: lua holds pointers to them.
void sdl_monome_init(const sdl_monome_conf_t *conf) {
    sm_conf = *conf;
    if (conf->grid && !vg.dev) {
        vg.cols = conf->grid_cols;
        vg.rows = conf->grid_rows;
        vg.ptr = -1;
        vg.dev = dev_monome_new_virtual_grid(vg.cols, vg.rows, "sdl", "virtual grid", &vgrid_refresh);
        if (vg.dev) {
            post_monome_add(vg.dev);
        } else {
            fprintf(stderr, "ERROR (screen:sdl) failed to create the virtual grid\n");
        }
    }
    if (conf->arc && !va.dev) {
        va.rings = conf->arc_rings;
        va.wheel = conf->arc_wheel;
        va.key_ptr = va.drag = -1;
        // lua takes a monome device for an arc when its name starts so
        va.dev = dev_monome_new_virtual_arc("sdl", "monome arc", &varc_refresh);
        if (va.dev) {
            post_monome_add(va.dev);
        } else {
            fprintf(stderr, "ERROR (screen:sdl) failed to create the virtual arc\n");
        }
    }
}

//--- input (render thread). No SDL calls here, so the tests can drive it.

static void post_grid_key(int x, int y, int z) {
    union event_data *ev = event_data_new(EVENT_GRID_KEY);
    ev->grid_key.id = vg.dev->dev.id;
    ev->grid_key.x = x;
    ev->grid_key.y = y;
    ev->grid_key.state = z;
    event_post(ev);
}

static void post_arc_key(int n, int z) {
    union event_data *ev = event_data_new(EVENT_ARC_ENCODER_KEY);
    ev->arc_encoder_key.id = va.dev->dev.id;
    ev->arc_encoder_key.number = n;
    ev->arc_encoder_key.state = z;
    event_post(ev);
}

static void post_arc_delta(int n, int d) {
    union event_data *ev = event_data_new(EVENT_ARC_ENCODER_DELTA);
    ev->arc_encoder_delta.id = va.dev->dev.id;
    ev->arc_encoder_delta.number = n;
    ev->arc_encoder_delta.delta = d;
    event_post(ev);
}

// Sets or clears hold bits of key i; the key goes down with the first bit
// and up with the last.
static void vgrid_hold(int i, uint8_t bits, bool on) {
    uint8_t was = vg.held[i];
    vg.held[i] = on ? (uint8_t)(was | bits) : (uint8_t)(was & ~bits);
    if ((was != 0) != (vg.held[i] != 0)) {
        post_grid_key(i % vg.cols, i / vg.cols, vg.held[i] != 0);
    }
    if (was != vg.held[i]) {
        vg.w.redraw = true;
    }
}

// the key at a window position, -1 for none. The gaps between keys belong to
// the key to their upper left, so a drag does not let go between keys.
static int vgrid_key_at(int px, int py) {
    if (vg.pitch <= 0 || px < vg.ox || py < vg.oy) {
        return -1;
    }
    int cx = (px - vg.ox) / vg.pitch, cy = (py - vg.oy) / vg.pitch;
    if (cx >= vg.cols || cy >= vg.rows) {
        return -1;
    }
    return cy * vg.cols + cx;
}

static void vgrid_press(int px, int py, bool latch) {
    int i = vgrid_key_at(px, py);
    if (latch) {
        vg.ptr_down = false;
        if (i >= 0) {
            vgrid_hold(i, HELD_LATCH, !(vg.held[i] & HELD_LATCH));
        }
        return;
    }
    vg.ptr_down = true;
    vg.ptr = i;
    if (i >= 0) {
        vgrid_hold(i, HELD_POINTER, true);
    }
}

static void vgrid_motion(int px, int py) {
    if (!vg.ptr_down) {
        return;
    }
    int i = vgrid_key_at(px, py);
    if (i == vg.ptr) {
        return;
    }
    if (vg.ptr >= 0) {
        vgrid_hold(vg.ptr, HELD_POINTER, false);
    }
    vg.ptr = i;
    if (i >= 0) {
        vgrid_hold(i, HELD_POINTER, true);
    }
}

static void vgrid_release(void) {
    if (vg.ptr_down && vg.ptr >= 0) {
        vgrid_hold(vg.ptr, HELD_POINTER, false);
    }
    vg.ptr_down = false;
    vg.ptr = -1;
}

static void vgrid_release_held(uint8_t bits) {
    for (int i = 0; i < vg.cols * vg.rows; i++) {
        if (vg.held[i] & bits) {
            vgrid_hold(i, bits, false);
        }
    }
}

static void varc_hold(int n, uint8_t bits, bool on) {
    uint8_t was = va.held[n];
    va.held[n] = on ? (uint8_t)(was | bits) : (uint8_t)(was & ~bits);
    if ((was != 0) != (va.held[n] != 0)) {
        post_arc_key(n, va.held[n] != 0);
    }
    if (was != va.held[n]) {
        va.w.redraw = true;
    }
}

// the ring at a window position, -1 for none, and the offset from its centre
static int varc_ring_at(float px, float py, float *dx, float *dy) {
    if (va.size <= 0) {
        return -1;
    }
    float x = px - va.ox, y = py - va.oy;
    if (x < 0 || y < 0 || y >= va.size) {
        return -1;
    }
    int n = (int)(x / va.size);
    if (n >= va.rings) {
        return -1;
    }
    *dx = x - (n + 0.5f) * va.size;
    *dy = y - 0.5f * va.size;
    return n;
}

static void varc_turn(int n, float ticks) {
    va.acc[n] += ticks;
    int steps = (int)va.acc[n];
    va.acc[n] -= steps;
    while (steps != 0) {
        int d = steps > 127 ? 127 : (steps < -127 ? -127 : steps);
        post_arc_delta(n, d);
        steps -= d;
    }
}

static void varc_press(float px, float py, bool latch) {
    float dx, dy;
    int n = varc_ring_at(px, py, &dx, &dy);
    if (n < 0) {
        return;
    }
    float r = ARC_KEY_R * va.size;
    if (dx * dx + dy * dy < r * r) {
        if (latch) {
            varc_hold(n, HELD_LATCH, !(va.held[n] & HELD_LATCH));
        } else {
            va.key_ptr = n;
            varc_hold(n, HELD_POINTER, true);
        }
        return;
    }
    va.drag = n;
    va.drag_x = px;
    va.drag_y = py;
}

// The dragged ring turns by the angle the pointer moves around its centre,
// wherever the pointer goes.
static void varc_motion(float px, float py) {
    if (va.drag < 0) {
        return;
    }
    float cx = va.ox + (va.drag + 0.5f) * va.size, cy = va.oy + 0.5f * va.size;
    float ax = va.drag_x - cx, ay = va.drag_y - cy;
    float bx = px - cx, by = py - cy;
    float dead = ARC_DEAD_R * va.size;
    if (ax * ax + ay * ay >= dead * dead && bx * bx + by * by >= dead * dead) {
        // y points down, so a positive angle is clockwise
        varc_turn(va.drag, atan2f(ax * by - ay * bx, ax * bx + ay * by) * (ARC_TICKS / (2.f * (float)M_PI)));
    }
    va.drag_x = px;
    va.drag_y = py;
}

static void varc_release(void) {
    if (va.key_ptr >= 0) {
        varc_hold(va.key_ptr, HELD_POINTER, false);
    }
    va.key_ptr = -1;
    va.drag = -1;
}

// a wheel step away from you turns clockwise
static void varc_wheel(float px, float py, int steps) {
    float dx, dy;
    int n = varc_ring_at(px, py, &dx, &dy);
    if (n >= 0) {
        varc_turn(n, (float)(steps * va.wheel));
    }
}

static void varc_release_held(uint8_t bits) {
    for (int n = 0; n < va.rings; n++) {
        if (va.held[n] & bits) {
            varc_hold(n, bits, false);
        }
    }
}

// keys held by the modifier go up once it is no longer down
static void sdl_monome_check_hold(SDL_Keymod mods) {
    if (hold_mod != KMOD_NONE && (mods & hold_mod)) {
        return;
    }
    if (vg.dev) {
        vgrid_release_held(HELD_LATCH);
    }
    if (va.dev) {
        varc_release_held(HELD_LATCH);
    }
}

static void sdl_monome_release_all(void) {
    if (vg.dev) {
        vgrid_release();
        vgrid_release_held(HELD_LATCH);
    }
    if (va.dev) {
        varc_release();
        varc_release_held(HELD_LATCH);
        memset(va.acc, 0, sizeof(va.acc));
    }
}

//--- layout and drawing (render thread)

// keys as large as the window allows, centred, with half a key of margin
static void vgrid_layout(int w, int h) {
    float pw = w / (vg.cols + 0.5f), ph = h / (vg.rows + 0.5f);
    vg.pitch = (int)(pw < ph ? pw : ph);
    if (vg.pitch < 1) {
        vg.pitch = 1;
    }
    vg.ox = (w - vg.pitch * vg.cols) / 2;
    vg.oy = (h - vg.pitch * vg.rows) / 2;
}

static void varc_layout(int w, int h) {
    int s = w / va.rings;
    va.size = s < h ? s : h;
    if (va.size < 1) {
        va.size = 1;
    }
    va.ox = (w - va.size * va.rings) / 2;
    va.oy = (h - va.size) / 2;
}

// level 0 to 15: unlit keys stay visible, lit ones go to a warm white
static uint32_t level_colour(const SDL_PixelFormat *f, int level) {
    float t = (level & 15) / 15.f;
    return SDL_MapRGB(f, (uint8_t)(38 + t * (255 - 38)), (uint8_t)(38 + t * (238 - 38)), (uint8_t)(38 + t * (205 - 38)));
}

static uint32_t hold_colour(const SDL_PixelFormat *f) {
    return SDL_MapRGB(f, 90, 170, 255);
}

// a rectangle with corners of radius r, clipped to the surface
static void fill_round(SDL_Surface *s, int x0, int y0, int w, int h, int r, uint32_t c) {
    if (2 * r > w) {
        r = w / 2;
    }
    if (2 * r > h) {
        r = h / 2;
    }
    for (int y = 0; y < h; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= s->h) {
            continue;
        }
        int in = 0;
        float d = y < r ? r - y - 0.5f : (y >= h - r ? y - (h - r) + 0.5f : 0.f);
        if (d > 0.f) {
            in = (int)(r - sqrtf((float)r * r - d * d) + 0.5f);
        }
        int xa = x0 + in, xb = x0 + w - in;
        if (xa < 0) {
            xa = 0;
        }
        if (xb > s->w) {
            xb = s->w;
        }
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)sy * s->pitch);
        for (int x = xa; x < xb; x++) {
            row[x] = c;
        }
    }
}

static void vgrid_draw(SDL_Surface *s) {
    const SDL_PixelFormat *f = s->format;
    SDL_FillRect(s, NULL, SDL_MapRGB(f, 16, 16, 16));
    int gap = vg.pitch / 8 > 2 ? vg.pitch / 8 : 2;
    int key = vg.pitch - gap;
    int r = key / 6;
    int ring = key / 14 > 2 ? key / 14 : 2;
    for (int cy = 0; cy < vg.rows; cy++) {
        for (int cx = 0; cx < vg.cols; cx++) {
            int i = cy * vg.cols + cx;
            int x = vg.ox + cx * vg.pitch + gap / 2, y = vg.oy + cy * vg.pitch + gap / 2;
            uint32_t c = level_colour(f, vg.shown[i]);
            if (vg.held[i]) {
                // the keys you hold, which the script may leave dark
                fill_round(s, x, y, key, key, r, hold_colour(f));
                fill_round(s, x + ring, y + ring, key - 2 * ring, key - 2 * ring, r > ring ? r - ring : 0, c);
            } else {
                fill_round(s, x, y, key, key, r, c);
            }
        }
    }
}

// which led of a ring each pixel of its square belongs to: led 0 at the top,
// then clockwise, with a gap between neighbours; the key disc in the middle
static bool varc_make_map(int px) {
    if (va.map && va.map_size == px) {
        return true;
    }
    free(va.map);
    va.map = (uint8_t *)malloc((size_t)px * px);
    if (!va.map) {
        va.map_size = 0;
        return false;
    }
    va.map_size = px;
    const float c = px / 2.f, r_out = px * ARC_LED_R_OUT, r_in = px * ARC_LED_R_IN, r_key = px * ARC_KEY_R;
    for (int y = 0; y < px; y++) {
        for (int x = 0; x < px; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c;
            float r = sqrtf(dx * dx + dy * dy);
            float t = atan2f(dx, -dy) * (ARC_LEDS / (2.f * (float)M_PI));
            if (t < 0) {
                t += ARC_LEDS;
            }
            int led = (int)(t + 0.5f);
            uint8_t v = ARC_MAP_NONE;
            if (r >= r_in && r <= r_out && fabsf(t - led) < 0.36f) {
                v = (uint8_t)(led % ARC_LEDS);
            } else if (r < r_key) {
                v = ARC_MAP_KEY;
            }
            va.map[y * px + x] = v;
        }
    }
    return true;
}

static void varc_draw(SDL_Surface *s) {
    const SDL_PixelFormat *f = s->format;
    SDL_FillRect(s, NULL, SDL_MapRGB(f, 16, 16, 16));
    if (!varc_make_map(va.size)) {
        return;
    }
    uint32_t colour[16];
    for (int i = 0; i < 16; i++) {
        colour[i] = level_colour(f, i);
    }
    uint32_t key_up = SDL_MapRGB(f, 46, 46, 46), key_down = hold_colour(f);
    for (int n = 0; n < va.rings; n++) {
        uint32_t key = va.held[n] ? key_down : key_up;
        for (int y = 0; y < va.size; y++) {
            int sy = va.oy + y;
            if (sy < 0 || sy >= s->h) {
                continue;
            }
            const uint8_t *map = va.map + (size_t)y * va.size;
            uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + (size_t)sy * s->pitch);
            int x0 = va.ox + n * va.size;
            for (int x = 0; x < va.size; x++) {
                if (x0 + x < 0 || x0 + x >= s->w || map[x] == ARC_MAP_NONE) {
                    continue;
                }
                row[x0 + x] = map[x] == ARC_MAP_KEY ? key : colour[va.shown[n][map[x]] & 15];
            }
        }
    }
}

//--- windows (render thread)

// opens below `above` when there is one (window managers that place windows
// themselves, and wayland, may not follow)
static bool win_open(sdl_monome_win_t *w, const char *title, int width, int height, SDL_Window *above) {
    int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
    if (above) {
        int ax, ay, aw, ah, top = 0, bottom = 0;
        SDL_GetWindowPosition(above, &ax, &ay);
        SDL_GetWindowSize(above, &aw, &ah);
        // the frame: our window's title bar sits above its position too
        SDL_GetWindowBordersSize(above, &top, NULL, &bottom, NULL);
        x = ax;
        y = ay + ah + bottom + top + 8;
        // beside it when there is no room below, if there is room there
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(above), &r) == 0 && y + height > r.y + r.h &&
            ax + aw + 8 + width <= r.x + r.w) {
            x = ax + aw + 8;
            y = ay;
        }
    }
    w->win = SDL_CreateWindow(title, x, y, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!w->win) {
        fprintf(stderr, "ERROR (screen:sdl) failed to open the %s window: %s\n", title, SDL_GetError());
        return false;
    }
    w->id = SDL_GetWindowID(w->win);
    w->redraw = true;
    return true;
}

static void win_close(sdl_monome_win_t *w) {
    if (w->canvas) {
        SDL_FreeSurface(w->canvas);
        w->canvas = NULL;
    }
    if (w->win) {
        SDL_DestroyWindow(w->win);
        w->win = NULL;
    }
    w->id = 0;
}

// draws into the canvas, sized to the window, and shows it
static void win_present(sdl_monome_win_t *w, void (*layout)(int, int), void (*draw)(SDL_Surface *)) {
    if (!w->win || !w->redraw || (SDL_GetWindowFlags(w->win) & SDL_WINDOW_HIDDEN)) {
        return;
    }
    // the window surface is replaced when the window changes size
    SDL_Surface *ws = SDL_GetWindowSurface(w->win);
    if (!ws) {
        return;
    }
    if (!w->canvas || w->canvas->w != ws->w || w->canvas->h != ws->h) {
        if (w->canvas) {
            SDL_FreeSurface(w->canvas);
        }
        w->canvas = SDL_CreateRGBSurfaceWithFormat(0, ws->w, ws->h, 32, SDL_PIXELFORMAT_RGB888);
        if (!w->canvas) {
            return;
        }
        layout(ws->w, ws->h);
    }
    draw(w->canvas);
    SDL_BlitSurface(w->canvas, NULL, ws, NULL);
    SDL_UpdateWindowSurface(w->win);
    w->redraw = false;
}

void sdl_monome_open(SDL_Keymod mod, SDL_Window *screen) {
    hold_mod = mod;
    cur_mods = SDL_GetModState();
    if (vg.dev && !vg.w.win) {
        int p = sm_conf.grid_size;
        if (win_open(&vg.w, "norns grid", (int)(p * (vg.cols + 0.5f)), (int)(p * (vg.rows + 0.5f)), screen)) {
            vgrid_layout((int)(p * (vg.cols + 0.5f)), (int)(p * (vg.rows + 0.5f)));
        }
    }
    if (va.dev && !va.w.win) {
        int s = sm_conf.arc_size;
        if (win_open(&va.w, "norns arc", s * va.rings, s, vg.w.win ? vg.w.win : screen)) {
            varc_layout(s * va.rings, s);
        }
    }
}

void sdl_monome_close(void) {
    sdl_monome_release_all();
    win_close(&vg.w);
    win_close(&va.w);
    free(va.map);
    va.map = NULL;
    va.map_size = 0;
}

static bool sdl_monome_window_event(const SDL_WindowEvent *we) {
    sdl_monome_win_t *w = we->windowID == vg.w.id && vg.w.win ? &vg.w : (we->windowID == va.w.id && va.w.win ? &va.w : NULL);
    switch (we->event) {
    case SDL_WINDOWEVENT_FOCUS_LOST:
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        // the modifier may have come up while another window had the keyboard
        cur_mods = SDL_GetModState();
        sdl_monome_check_hold(cur_mods);
        break;
    default:
        break;
    }
    if (!w) {
        return false;
    }
    switch (we->event) {
    case SDL_WINDOWEVENT_CLOSE:
        // hidden, not destroyed: the device stays connected, unlit and silent
        sdl_monome_release_all();
        SDL_HideWindow(w->win);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_EXPOSED:
    case SDL_WINDOWEVENT_SHOWN:
        w->redraw = true;
        break;
    default:
        break;
    }
    return true;
}

bool sdl_monome_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_KEYUP:
    case SDL_KEYDOWN:
        cur_mods = (SDL_Keymod)e->key.keysym.mod;
        sdl_monome_check_hold(cur_mods);
        return false;
    case SDL_WINDOWEVENT:
        return sdl_monome_window_event(&e->window);
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        const SDL_MouseButtonEvent *b = &e->button;
        if (b->button != SDL_BUTTON_LEFT) {
            return b->windowID == vg.w.id || b->windowID == va.w.id;
        }
        bool latch = e->type == SDL_MOUSEBUTTONDOWN && hold_mod != KMOD_NONE && (cur_mods & hold_mod);
        if (vg.w.win && b->windowID == vg.w.id) {
            if (e->type == SDL_MOUSEBUTTONDOWN) {
                vgrid_press(b->x, b->y, latch);
            } else {
                vgrid_release();
            }
            return true;
        }
        if (va.w.win && b->windowID == va.w.id) {
            if (e->type == SDL_MOUSEBUTTONDOWN) {
                varc_press((float)b->x, (float)b->y, latch);
            } else {
                varc_release();
            }
            return true;
        }
        return false;
    }
    case SDL_MOUSEMOTION:
        if (vg.w.win && e->motion.windowID == vg.w.id) {
            vgrid_motion(e->motion.x, e->motion.y);
            return true;
        }
        if (va.w.win && e->motion.windowID == va.w.id) {
            varc_motion((float)e->motion.x, (float)e->motion.y);
            return true;
        }
        return false;
    case SDL_MOUSEWHEEL:
        if (va.w.win && e->wheel.windowID == va.w.id) {
            int x, y;
            SDL_GetMouseState(&x, &y);
            int steps = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
            varc_wheel((float)x, (float)y, steps);
            return true;
        }
        return e->wheel.windowID == vg.w.id && vg.w.win;
    default:
        return false;
    }
}

void sdl_monome_present(void) {
    pthread_mutex_lock(&sdl_monome_lock);
    if (vg.led_dirty) {
        memcpy(vg.shown, vg.led, sizeof(vg.shown));
        vg.led_dirty = false;
        vg.w.redraw = true;
    }
    if (va.led_dirty) {
        memcpy(va.shown, va.led, sizeof(va.shown));
        va.led_dirty = false;
        va.w.redraw = true;
    }
    pthread_mutex_unlock(&sdl_monome_lock);
    win_present(&vg.w, &vgrid_layout, &vgrid_draw);
    win_present(&va.w, &varc_layout, &varc_draw);
}
