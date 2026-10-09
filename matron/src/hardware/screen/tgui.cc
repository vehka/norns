// screen:tgui -- norns screen and controls in a Termux:GUI activity (Android).
//
// The 128x64 screen is upscaled into a shared image buffer shown in an
// ImageView. In portrait the touch pads for the three keys and three encoders
// are below it; in landscape the screen fills the window and the pads are
// invisible zones over its left and right ends:
//
//     +-----------------+      +----+-----------------+----+
//     |     screen      |      | K1 |                 | E1 |
//     +--------+--------+      +----+                 +----+
//     |   K1   |   E1   |      | K2 |     screen      | E2 |
//     +--------+--------+      +----+                 +----+
//     |   K2   |   E2   |      | K3 |                 | E3 |
//     +--------+--------+      +----+-----------------+----+
//     |   K3   |   E3   |
//     +--------+--------+
//
// In landscape there is a second page with a virtual 16x8 monome grid, which
// scripts see as an ordinary grid device. A two-finger sideways swipe (on the
// grid, or on the middle of the screen page) switches pages; back returns
// from the grid to the screen.
//
// The layout follows the rotation of the device unless `orientation` fixes
// it. Keys follow touch down/up, so holding works. Encoders are drag pads:
// right or up is clockwise, one step per `enc_step_dp` of travel.
//
// Two threads talk to the plugin: screen_tgui_loop opens the window and
// presents frames, tgui_event_loop blocks on the event socket. (The library's
// non-blocking tgui_poll_event() only looks at the socket, not at what it has
// already buffered, so a touch release read together with its press would sit
// there until the next event.) The window may be closed by the user or by
// Android at any time; matron keeps running and screen_tgui_show()
// (_norns.screen_tgui_show() in Lua) opens it again.

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cairo.h>
#include <termuxgui/termuxgui.h>

#include "device/device_monome.h"
#include "event_types.h"
#include "events.h"
#include "hardware/io.h"
#include "hardware/screen/screens.h"

#define SCREEN_W 128
#define SCREEN_H 64

#define GRID_COLS 16
#define GRID_ROWS 8
#define TGUI_MAX_PTR 10
#define TGUI_SWIPE_DP 80

enum { TGUI_PAGE_SCREEN = 0, TGUI_PAGE_GRID };

// the fingers on one view
typedef struct {
    struct {
        int id; // pointer id, -1 when free
        int sx, sy, x, y;
        int cx, cy; // grid cell held, -1 for none
    } p[TGUI_MAX_PTR];
    bool swiped; // the gesture was a page swipe; ignore it until all fingers lift
} tgui_touches_t;

enum { TGUI_LAYOUT_AUTO = 0, TGUI_LAYOUT_PORTRAIT, TGUI_LAYOUT_LANDSCAPE };

typedef struct _screen_tgui_priv {
    pthread_t thread;
    bool thread_started;
    volatile bool running;
    volatile bool connecting; // inside tgui_connection_create(), which can block

    // set by paint (screen-events thread), cleared by the tgui thread
    volatile bool dirty;
    // set by screen_tgui_show (lua thread), cleared by the tgui thread
    volatile bool want_show;

    // options
    float enc_step_dp;
    bool keep_screen_on;
    bool debug; // log lifecycle and touch events
    int orientation;  // TGUI_LAYOUT_*
    float zone_width; // landscape: width of each touch zone, fraction of the window
    bool grid;        // offer the virtual grid
} screen_tgui_priv_t;

// state shared by the two tgui threads, guarded by `lock`
typedef struct {
    pthread_mutex_t lock;
    pthread_t evt_thread;
    bool evt_started;
    volatile bool evt_stop; // asks the event thread to exit
    volatile bool evt_done; // the event thread has left its loop
    volatile bool lost;     // the event thread saw the connection drop

    screen_tgui_priv_t *priv;
    const uint32_t *src; // pixels of the matron_fb surface (ARGB32, 128x64)

    tgui_connection c;
    bool connected;
    tgui_activity a; // -1 when there is no window
    tgui_task task;
    bool visible;
    tgui_buffer *buf;
    tgui_buffer *grid_buf;
    int present_failed; // frames in a row that the plugin declined
    int scale;

    // window size in dp and density, from the last configuration seen
    int conf_w, conf_h;
    float density;
    bool landscape;            // the layout that is built
    int built_w, built_h;      // and the window size it was built for
    volatile bool want_layout; // the event thread saw the orientation change

    tgui_view root;
    bool has_root;
    bool bars_hidden; // the system bars of this window are hidden

    int page;                // TGUI_PAGE_*, the one shown
    volatile int want_page;  // set by the event thread
    tgui_view page_view[2];
    tgui_view gap;           // middle of the screen page, takes page swipes
    tgui_view grid_img;
    float grid_cell_px;
    tgui_touches_t gap_touch, grid_touch;
    uint8_t grid_held[GRID_ROWS][GRID_COLS]; // fingers on each cell
    tgui_view img;
    tgui_view key[3];
    tgui_view enc[3];
    bool key_down[3];
    int enc_ptr[3]; // tracked pointer id, -1 when not touched
    int enc_x[3], enc_y[3];
    float enc_acc[3];
    float enc_step_px;
} tgui_state_t;

// guards the fb surface pixels, shared between paint and the tgui thread
static pthread_mutex_t tgui_lock = PTHREAD_MUTEX_INITIALIZER;

static screen_tgui_priv_t *tgui_instance = NULL;

// the virtual grid: its device, and the led levels of the last refresh
// (guarded by tgui_lock)
static struct dev_monome *tgui_grid_dev = NULL;
static uint8_t tgui_grid_led[GRID_ROWS][GRID_COLS];
static volatile bool tgui_grid_dirty = false;

static int screen_tgui_config(matron_io_t *io, lua_State *l);
static int screen_tgui_setup(matron_io_t *io);
static void screen_tgui_destroy(matron_io_t *io);
static void screen_tgui_paint(matron_fb_t *fb);
static void screen_tgui_bind(matron_fb_t *fb, cairo_surface_t *surface);
static void *screen_tgui_loop(void *data);

screen_ops_t screen_tgui_ops = {
    {
        // .io_ops
        "screen:tgui",               // .name
        IO_SCREEN,                   // .type
        sizeof(screen_tgui_priv_t),  // .data_size
        screen_tgui_config,          // .config
        screen_tgui_setup,           // .setup
        screen_tgui_destroy,         // .destroy
    },
    screen_tgui_paint, // .paint
    screen_tgui_bind,  // .bind
};

// Optional fields of the add_io options table (arg 2):
//   enc_step_dp    = <number>   drag distance per encoder step (default 12)
//   keep_screen_on = <boolean>  keep the display awake while shown (default true)
//   debug          = <boolean>  log window and touch events (default false)
//   orientation    = <string>   "auto" (follow the device, default),
//                               "portrait" or "landscape"
//   zone_width     = <number>   landscape: width of the key and encoder zones
//                               as a fraction of the window (default 0.25)
//   grid           = <boolean>  virtual 16x8 grid on a second page (default true)
int screen_tgui_config(matron_io_t *io, lua_State *l) {
    screen_tgui_priv_t *priv = (screen_tgui_priv_t *)io->data;
    memset(priv, 0, sizeof(*priv));
    priv->enc_step_dp = 12.f;
    priv->keep_screen_on = true;
    priv->zone_width = 0.25f;
    priv->grid = true;

    if (!lua_istable(l, 2)) {
        return 0;
    }
    lua_getfield(l, 2, "enc_step_dp");
    if (lua_isnumber(l, -1) && lua_tonumber(l, -1) >= 1) {
        priv->enc_step_dp = (float)lua_tonumber(l, -1);
    }
    lua_pop(l, 1);
    lua_getfield(l, 2, "keep_screen_on");
    if (lua_isboolean(l, -1)) {
        priv->keep_screen_on = lua_toboolean(l, -1);
    }
    lua_pop(l, 1);
    lua_getfield(l, 2, "debug");
    priv->debug = lua_toboolean(l, -1);
    lua_pop(l, 1);
    lua_getfield(l, 2, "orientation");
    if (lua_type(l, -1) == LUA_TSTRING) {
        const char *o = lua_tostring(l, -1);
        if (strcmp(o, "portrait") == 0) {
            priv->orientation = TGUI_LAYOUT_PORTRAIT;
        } else if (strcmp(o, "landscape") == 0) {
            priv->orientation = TGUI_LAYOUT_LANDSCAPE;
        } else if (strcmp(o, "auto") != 0) {
            fprintf(stderr, "WARN (%s) unknown orientation '%s'\n", io->ops->name, o);
        }
    }
    lua_pop(l, 1);
    lua_getfield(l, 2, "grid");
    if (lua_isboolean(l, -1)) {
        priv->grid = lua_toboolean(l, -1);
    }
    lua_pop(l, 1);
    lua_getfield(l, 2, "zone_width");
    if (lua_isnumber(l, -1) && lua_tonumber(l, -1) >= 0.05 && lua_tonumber(l, -1) <= 0.5) {
        priv->zone_width = (float)lua_tonumber(l, -1);
    }
    lua_pop(l, 1);
    return 0;
}

// dev_monome_refresh() of the virtual grid, on the lua thread
static void tgui_grid_refresh(struct dev_monome *md) {
    pthread_mutex_lock(&tgui_lock);
    for (int q = 0; q < 2; q++) {
        for (int y = 0; y < 8; y++) {
            memcpy(&tgui_grid_led[y][q * 8], &md->data[q][y * 8], 8);
        }
    }
    tgui_grid_dirty = true;
    pthread_mutex_unlock(&tgui_lock);
}

int screen_tgui_setup(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_tgui_priv_t *priv = (screen_tgui_priv_t *)io->data;

    fb->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SCREEN_W, SCREEN_H);
    if (cairo_surface_status(fb->surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "ERROR (%s) failed to create surface\n", io->ops->name);
        return -1;
    }
    fb->cairo = cairo_create(fb->surface);

    // A missing or unresponsive plugin is not fatal: the thread logs it and
    // matron carries on without a window.
    tgui_state_t *st = (tgui_state_t *)calloc(1, sizeof(tgui_state_t));
    if (!st) {
        return -1;
    }
    st->priv = priv;
    st->src = (const uint32_t *)cairo_image_surface_get_data(fb->surface);
    st->a = -1;
    pthread_mutex_init(&st->lock, NULL);

    priv->want_show = true;
    priv->running = true;
    if (pthread_create(&priv->thread, NULL, &screen_tgui_loop, st) != 0) {
        fprintf(stderr, "ERROR (%s) failed to start thread\n", io->ops->name);
        priv->running = false;
        free(st);
    } else {
        priv->thread_started = true;
        tgui_instance = priv;
        if (priv->grid && !tgui_grid_dev) {
            tgui_grid_dev = dev_monome_new_virtual_grid(GRID_COLS, GRID_ROWS, "tgui", "tgui grid", &tgui_grid_refresh);
            if (tgui_grid_dev) {
                union event_data *ev = event_data_new(EVENT_MONOME_ADD);
                ev->monome_add.dev = tgui_grid_dev;
                event_post(ev);
            }
        }
    }
    return 0;
}

static void screen_tgui_destroy(matron_io_t *io) {
    matron_fb_t *fb = (matron_fb_t *)io;
    screen_tgui_priv_t *priv = (screen_tgui_priv_t *)io->data;

    tgui_instance = NULL;
    if (priv->thread_started) {
        priv->running = false;
        if (priv->connecting) {
            // stuck waiting for the plugin; do not hang shutdown on it
            pthread_detach(priv->thread);
        } else {
            pthread_join(priv->thread, NULL);
        }
        priv->thread_started = false;
    }
    cairo_destroy(fb->cairo);
    cairo_surface_destroy(fb->surface);
}

// Runs on the screen-events thread (via screen.update): copy the bound matron
// surface into ours; the tgui thread upscales and presents it.
static void screen_tgui_paint(matron_fb_t *fb) {
    screen_tgui_priv_t *priv = (screen_tgui_priv_t *)fb->io.data;
    pthread_mutex_lock(&tgui_lock);
    cairo_paint(fb->cairo);
    cairo_surface_flush(fb->surface);
    priv->dirty = true;
    pthread_mutex_unlock(&tgui_lock);
}

static void screen_tgui_bind(matron_fb_t *fb, cairo_surface_t *surface) {
    cairo_set_operator(fb->cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(fb->cairo, surface, 0, 0);
}

void screen_tgui_show(void) {
    screen_tgui_priv_t *priv = tgui_instance;
    if (priv) {
        priv->want_show = true;
    }
}

//-------------------------
//--- tgui thread

static void tgui_post_key(int n, int val) {
    union event_data *ev = event_data_new(EVENT_KEY);
    ev->key.n = n;
    ev->key.val = val;
    event_post(ev);
}

// EVENT_SDL_ENC bypasses the sens/accel accumulator, so one step here is
// exactly one step on every encoder.
static void tgui_post_enc(int n, int delta) {
    union event_data *ev = event_data_new(EVENT_SDL_ENC);
    ev->enc.n = n;
    ev->enc.delta = delta;
    event_post(ev);
}

static void tgui_post_grid_key(int x, int y, int state) {
    union event_data *ev = event_data_new(EVENT_GRID_KEY);
    ev->grid_key.id = tgui_grid_dev->dev.id;
    ev->grid_key.x = x;
    ev->grid_key.y = y;
    ev->grid_key.state = state;
    event_post(ev);
}

static void tgui_touches_reset(tgui_touches_t *t) {
    for (int i = 0; i < TGUI_MAX_PTR; i++) {
        t->p[i].id = -1;
    }
    t->swiped = false;
}

// the slot of pointer `id`; with id -1, a free slot. -1 if there is none
static int tgui_touches_find(const tgui_touches_t *t, int id) {
    for (int i = 0; i < TGUI_MAX_PTR; i++) {
        if (t->p[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int tgui_touches_add(tgui_touches_t *t, const tgui_touch_pointer *p) {
    int i = tgui_touches_find(t, p->id);
    if (i < 0) {
        i = tgui_touches_find(t, -1);
    }
    if (i >= 0) {
        t->p[i].id = p->id;
        t->p[i].sx = t->p[i].x = p->x;
        t->p[i].sy = t->p[i].y = p->y;
        t->p[i].cx = t->p[i].cy = -1;
    }
    return i;
}

// Takes the positions of a move event. True, once per gesture, when exactly
// two fingers have both travelled sideways in the same direction.
static bool tgui_touches_move(tgui_touches_t *t, const tgui_event *ev, float swipe_px) {
    int n = 0, right = 0, left = 0;
    // (later entries of a pointer are newer)
    for (uint32_t e = 0; e < ev->touch.events; e++) {
        for (uint32_t k = 0; k < ev->touch.num_pointers; k++) {
            const tgui_touch_pointer *p = &ev->touch.pointers[e][k];
            int i = tgui_touches_find(t, p->id);
            if (i >= 0) {
                t->p[i].x = p->x;
                t->p[i].y = p->y;
            }
        }
    }
    for (int i = 0; i < TGUI_MAX_PTR; i++) {
        if (t->p[i].id == -1) {
            continue;
        }
        int dx = t->p[i].x - t->p[i].sx;
        int dy = abs(t->p[i].y - t->p[i].sy);
        n++;
        if (dx >= swipe_px && dy < dx) {
            right++;
        } else if (-dx >= swipe_px && dy < -dx) {
            left++;
        }
    }
    if (t->swiped || n != 2 || (right != 2 && left != 2)) {
        return false;
    }
    t->swiped = true;
    return true;
}

static void tgui_grid_lift(tgui_state_t *st, tgui_touches_t *t, int i) {
    int cx = t->p[i].cx, cy = t->p[i].cy;
    if (cx >= 0 && st->grid_held[cy][cx] > 0 && --st->grid_held[cy][cx] == 0) {
        tgui_post_grid_key(cx, cy, 0);
    }
    t->p[i].cx = t->p[i].cy = -1;
}

static void tgui_grid_lift_all(tgui_state_t *st) {
    for (int i = 0; i < TGUI_MAX_PTR; i++) {
        if (st->grid_touch.p[i].id != -1) {
            tgui_grid_lift(st, &st->grid_touch, i);
        }
    }
}

static void tgui_release_all(tgui_state_t *st) {
    for (int i = 0; i < 3; i++) {
        if (st->key_down[i]) {
            st->key_down[i] = false;
            tgui_post_key(i + 1, 0);
        }
        st->enc_ptr[i] = -1;
    }
    tgui_grid_lift_all(st);
    tgui_touches_reset(&st->grid_touch);
    tgui_touches_reset(&st->gap_touch);
}

static void *tgui_event_loop(void *data);

// Must be called without st->lock held.
static void tgui_disconnect(tgui_state_t *st) {
    if (st->evt_started) {
        // The thread is blocked in read(); pthread_cancel() (the android shim)
        // interrupts it with a signal. Repeat until it has noticed, in case
        // the signal lands just before it blocks.
        static const struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
        st->evt_stop = true;
        while (!st->evt_done) {
            pthread_cancel(st->evt_thread);
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
        }
        pthread_join(st->evt_thread, NULL);
        st->evt_started = false;
    }
    pthread_mutex_lock(&st->lock);
    tgui_release_all(st);
    if (st->connected) {
        if (st->buf) {
            tgui_delete_buffer(st->c, st->buf);
        }
        if (st->grid_buf) {
            tgui_delete_buffer(st->c, st->grid_buf);
        }
        tgui_connection_destroy(st->c);
    }
    free(st->buf);
    free(st->grid_buf);
    st->buf = st->grid_buf = NULL;
    st->connected = false;
    st->a = -1;
    st->visible = false;
    pthread_mutex_unlock(&st->lock);
}

static bool tgui_connect(tgui_state_t *st) {
    st->priv->connecting = true;
    tgui_err e = tgui_connection_create(&st->c);
    st->priv->connecting = false;
    if (e != TGUI_ERR_OK) {
        fprintf(stderr, "WARN (screen:tgui) cannot connect to Termux:GUI (%d); no window\n", (int)e);
        return false;
    }
    st->connected = true;
    st->evt_stop = st->evt_done = st->lost = false;
    st->present_failed = 0;
    if (pthread_create(&st->evt_thread, NULL, &tgui_event_loop, st) != 0) {
        fprintf(stderr, "WARN (screen:tgui) failed to start event thread\n");
        return false;
    }
    st->evt_started = true;
    return true;
}

// TGUI_ERR_MESSAGE is also what the library returns when the plugin answers
// that it did not carry out a call. That happens now and then while a window
// settles (seen after the system bars were shown or hidden) and the same call
// works a moment later, so it is repeated before giving up.
#define TGUI_TRY(call)                                                        \
    do {                                                                      \
        static const struct timespec ts_ = {.tv_sec = 0, .tv_nsec = 30000000}; \
        tgui_err e_ = (call);                                                 \
        for (int n_ = 0; e_ == TGUI_ERR_MESSAGE && n_ < 10; n_++) {           \
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts_, NULL);                  \
            e_ = (call);                                                      \
        }                                                                     \
        if (e_ != TGUI_ERR_OK) {                                              \
            fprintf(stderr, "WARN (screen:tgui) %s failed (%d)\n", #call, (int)e_); \
            return false;                                                     \
        }                                                                     \
    } while (0)

static const tgui_view_size TGUI_FILL = {TGUI_VIEW_MATCH_PARENT, {TGUI_UNIT_DP, 0}};
static const tgui_view_size TGUI_ZERO = {TGUI_VIEW_SIZE, {TGUI_UNIT_DP, 0}};

// A child of a linear layout that takes `weight` of the layout's length and
// all of its breadth.
static bool tgui_weigh(tgui_state_t *st, tgui_view v, float weight, bool in_row) {
    TGUI_TRY(tgui_linear_params(st->c, st->a, v, weight, 0));
    TGUI_TRY(tgui_set_width(st->c, st->a, v, in_row ? TGUI_ZERO : TGUI_FILL));
    TGUI_TRY(tgui_set_height(st->c, st->a, v, in_row ? TGUI_FILL : TGUI_ZERO));
    return true;
}

static bool tgui_make_pad(tgui_state_t *st, tgui_view *v, const tgui_view *row, const char *label) {
    TGUI_TRY(tgui_create_button(st->c, st->a, v, row, TGUI_VIS_VISIBLE, label));
    if (!tgui_weigh(st, *v, 1, true)) {
        return false;
    }
    TGUI_TRY(tgui_text_size(st->c, st->a, *v, {TGUI_UNIT_SP, 24}));
    TGUI_TRY(tgui_send_touch_event(st->c, st->a, *v, true));
    return true;
}

// The screen on top, a row per key and encoder pair below it.
static bool tgui_build_portrait(tgui_state_t *st) {
    tgui_view row[3];
    tgui_view_size img_h = {TGUI_VIEW_SIZE, {TGUI_UNIT_DP, st->conf_w / 2.f}};
    TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &st->root, NULL, TGUI_VIS_VISIBLE, false));
    st->has_root = true;
    TGUI_TRY(tgui_create_image_view(st->c, st->a, &st->img, &st->root, TGUI_VIS_VISIBLE, false));
    TGUI_TRY(tgui_set_height(st->c, st->a, st->img, img_h));
    TGUI_TRY(tgui_linear_params(st->c, st->a, st->img, 0, 0));
    for (int i = 0; i < 3; i++) {
        static const char *const key_label[3] = {"K1", "K2", "K3"};
        static const char *const enc_label[3] = {"E1", "E2", "E3"};
        TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &row[i], &st->root, TGUI_VIS_VISIBLE, true));
        TGUI_TRY(tgui_linear_params(st->c, st->a, row[i], 1, 0));
        if (!tgui_make_pad(st, &st->key[i], &row[i], key_label[i]) ||
            !tgui_make_pad(st, &st->enc[i], &row[i], enc_label[i])) {
            return false;
        }
    }
    return true;
}

// Three stacked zones that take touches and draw nothing.
static bool tgui_make_zones(tgui_state_t *st, tgui_view *v, const tgui_view *parent, float weight) {
    tgui_view col;
    TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &col, parent, TGUI_VIS_VISIBLE, false));
    if (!tgui_weigh(st, col, weight, true)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        TGUI_TRY(tgui_create_text_view(st->c, st->a, &v[i], &col, TGUI_VIS_VISIBLE, "", false, false));
        TGUI_TRY(tgui_send_touch_event(st->c, st->a, v[i], true));
        if (!tgui_weigh(st, v[i], 1, false)) {
            return false;
        }
    }
    return true;
}

static bool tgui_has_grid(const tgui_state_t *st) {
    return st->landscape && tgui_grid_dev != NULL;
}

// The grid page: one image, 2:1 like the grid, as large as the window allows
// and centred.
static bool tgui_build_grid_page(tgui_state_t *st) {
    tgui_view *page = &st->page_view[TGUI_PAGE_GRID];
    float h_dp = st->conf_h < st->conf_w / 2.f ? st->conf_h : st->conf_w / 2.f;
    float left_dp = (st->conf_w - 2 * h_dp) / 2;
    tgui_view_size w = {TGUI_VIEW_SIZE, {TGUI_UNIT_DP, 2 * h_dp}};
    tgui_view_size h = {TGUI_VIEW_SIZE, {TGUI_UNIT_DP, h_dp}};
    TGUI_TRY(tgui_create_frame_layout(st->c, st->a, page, &st->root,
                                      st->page == TGUI_PAGE_GRID ? TGUI_VIS_VISIBLE : TGUI_VIS_GONE));
    TGUI_TRY(tgui_set_width(st->c, st->a, *page, TGUI_FILL));
    TGUI_TRY(tgui_set_height(st->c, st->a, *page, TGUI_FILL));
    TGUI_TRY(tgui_create_image_view(st->c, st->a, &st->grid_img, page, TGUI_VIS_VISIBLE, false));
    TGUI_TRY(tgui_set_width(st->c, st->a, st->grid_img, w));
    TGUI_TRY(tgui_set_height(st->c, st->a, st->grid_img, h));
    TGUI_TRY(tgui_set_margin(st->c, st->a, st->grid_img, {TGUI_UNIT_DP, left_dp}, TGUI_DIR_LEFT));
    TGUI_TRY(tgui_set_buffer(st->c, st->a, st->grid_img, st->grid_buf));
    TGUI_TRY(tgui_send_touch_event(st->c, st->a, st->grid_img, true));
    // touches on an image view come in pixels of its buffer, not of the view
    st->grid_cell_px = 8 * st->scale;
    tgui_grid_dirty = true;
    return true;
}

// The screen over the whole window, the zones in a layer on top of it.
static bool tgui_build_landscape(tgui_state_t *st) {
    tgui_view over;
    tgui_view *page = &st->page_view[TGUI_PAGE_SCREEN];
    const float zone = st->priv->zone_width;
    TGUI_TRY(tgui_create_frame_layout(st->c, st->a, &st->root, NULL, TGUI_VIS_VISIBLE));
    st->has_root = true;
    TGUI_TRY(tgui_background_color(st->c, st->a, st->root, 0xff000000));
    TGUI_TRY(tgui_create_frame_layout(st->c, st->a, page, &st->root,
                                      st->page == TGUI_PAGE_SCREEN ? TGUI_VIS_VISIBLE : TGUI_VIS_GONE));
    TGUI_TRY(tgui_set_width(st->c, st->a, *page, TGUI_FILL));
    TGUI_TRY(tgui_set_height(st->c, st->a, *page, TGUI_FILL));
    TGUI_TRY(tgui_create_image_view(st->c, st->a, &st->img, page, TGUI_VIS_VISIBLE, false));
    TGUI_TRY(tgui_set_width(st->c, st->a, st->img, TGUI_FILL));
    TGUI_TRY(tgui_set_height(st->c, st->a, st->img, TGUI_FILL));
    TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &over, page, TGUI_VIS_VISIBLE, true));
    TGUI_TRY(tgui_set_width(st->c, st->a, over, TGUI_FILL));
    TGUI_TRY(tgui_set_height(st->c, st->a, over, TGUI_FILL));
    if (!tgui_make_zones(st, st->key, &over, zone)) {
        return false;
    }
    TGUI_TRY(tgui_create_text_view(st->c, st->a, &st->gap, &over, TGUI_VIS_VISIBLE, "", false, false));
    if (tgui_has_grid(st)) {
        TGUI_TRY(tgui_send_touch_event(st->c, st->a, st->gap, true));
    }
    if (!tgui_weigh(st, st->gap, 1.f - 2.f * zone, true) || !tgui_make_zones(st, st->enc, &over, zone)) {
        return false;
    }
    return !tgui_has_grid(st) || tgui_build_grid_page(st);
}

static bool tgui_is_landscape(const tgui_state_t *st) {
    switch (st->priv->orientation) {
    case TGUI_LAYOUT_PORTRAIT:
        return false;
    case TGUI_LAYOUT_LANDSCAPE:
        return true;
    default:
        return st->conf_w > st->conf_h;
    }
}

static void tgui_set_configuration(tgui_state_t *st, const tgui_activity_configuration *conf) {
    st->conf_w = conf->screen_width;
    st->conf_h = conf->screen_height;
    // not filled in by every CONFIG event
    if (conf->density > 0) {
        st->density = (float)conf->density;
    }
}

// (Re)builds the views for the current orientation. Called with st->lock held.
static bool tgui_build_layout(tgui_state_t *st) {
    tgui_release_all(st);
    if (st->has_root) {
        st->has_root = false;
        TGUI_TRY(tgui_delete_view(st->c, st->a, st->root));
    }
    st->landscape = tgui_is_landscape(st);
    st->built_w = st->conf_w;
    st->built_h = st->conf_h;
    st->gap = st->grid_img = -1;
    if (!tgui_has_grid(st)) {
        st->page = TGUI_PAGE_SCREEN;
    }
    st->want_page = st->page;
    st->enc_step_px = st->priv->enc_step_dp * st->density;

    // landscape is full screen; the bars come back on a swipe from the edge
    if (st->landscape != st->bars_hidden) {
        tgui_activity_configure_insets(st->c, st->a, st->landscape ? TGUI_INSET_NONE : TGUI_INSET_BOTH,
                                       st->landscape ? TGUI_INSET_BEHAVIOUR_TRANSIENT : TGUI_INSET_BEHAVIOUR_DEFAULT);
        st->bars_hidden = st->landscape;
    }
    if (!(st->landscape ? tgui_build_landscape(st) : tgui_build_portrait(st))) {
        return false;
    }
    TGUI_TRY(tgui_set_buffer(st->c, st->a, st->img, st->buf));
    st->priv->dirty = true;
    if (st->priv->debug) {
        fprintf(stderr, "screen:tgui layout %s, window %dx%d dp, density %.2f\n",
                st->landscape ? "landscape" : "portrait", st->conf_w, st->conf_h, st->density);
    }
    return true;
}

// A shared buffer of the upscaled screen size; the grid picture uses the same.
static tgui_buffer *tgui_new_buffer(tgui_state_t *st) {
    tgui_buffer b = {-1, -1, (uint32_t)(SCREEN_W * st->scale), (uint32_t)(SCREEN_H * st->scale),
                     TGUI_BUFFER_FORMAT_ARGB8888, NULL};
    tgui_buffer *buf = (tgui_buffer *)malloc(sizeof(tgui_buffer));
    if (!buf) {
        return NULL;
    }
    memcpy(buf, &b, sizeof(b));
    tgui_err e = tgui_add_buffer(st->c, buf);
    if (e != TGUI_ERR_OK) {
        fprintf(stderr, "WARN (screen:tgui) tgui_add_buffer failed (%d)\n", (int)e);
        free(buf);
        return NULL;
    }
    return buf;
}

static bool tgui_open_window(tgui_state_t *st) {
    static const tgui_orientation orientation[] = {
        TGUI_ORIENTATION_SENSOR,           // TGUI_LAYOUT_AUTO
        TGUI_ORIENTATION_PORTRAIT,         // TGUI_LAYOUT_PORTRAIT
        TGUI_ORIENTATION_SENSOR_LANDSCAPE, // TGUI_LAYOUT_LANDSCAPE
    };
    st->a = -1;
    st->has_root = false;
    st->want_layout = false;
    st->bars_hidden = false;
    // intercept the back button: it hides the window instead of closing it
    st->task = -1;
    TGUI_TRY(tgui_activity_create(st->c, &st->a, TGUI_ACTIVITY_NORMAL, &st->task, true));
    tgui_activity_set_orientation(st->c, st->a, orientation[st->priv->orientation]);
    tgui_activity_set_keep_screen_on(st->c, st->a, st->priv->keep_screen_on);

    tgui_activity_configuration conf;
    TGUI_TRY(tgui_activity_get_configuration(st->c, st->a, &conf));
    tgui_set_configuration(st, &conf);

    if (!st->buf) {
        // integer scale nearest to the short side of the display (the size
        // reported in landscape leaves out the system bars)
        int short_dp = st->conf_w < st->conf_h ? st->conf_w : st->conf_h;
        int scale = (int)(short_dp * st->density / SCREEN_W + 0.5f);
        st->scale = scale < 2 ? 2 : (scale > 12 ? 12 : scale);
        st->buf = tgui_new_buffer(st);
        if (!st->buf) {
            return false;
        }
    }
    if (tgui_grid_dev && !st->grid_buf) {
        st->grid_buf = tgui_new_buffer(st);
        if (!st->grid_buf) {
            return false;
        }
    }

    if (!tgui_build_layout(st)) {
        return false;
    }
    st->visible = true;
    fprintf(stderr, "screen:tgui window open, %dx%d buffer\n", SCREEN_W * st->scale, SCREEN_H * st->scale);
    return true;
}

// Shows the page the event thread asked for. Called with st->lock held.
static bool tgui_turn_page(tgui_state_t *st) {
    int page = st->want_page;
    if (!tgui_has_grid(st) || page == st->page) {
        st->want_page = st->page;
        return true;
    }
    tgui_release_all(st);
    TGUI_TRY(tgui_visibility(st->c, st->a, st->page_view[st->page], TGUI_VIS_GONE));
    TGUI_TRY(tgui_visibility(st->c, st->a, st->page_view[page], TGUI_VIS_VISIBLE));
    st->page = page;
    st->priv->dirty = true;
    tgui_grid_dirty = true;
    if (st->priv->debug) {
        fprintf(stderr, "screen:tgui page %d\n", page);
    }
    return true;
}

// Called with st->lock held. Returns false if the connection should be dropped.
static bool tgui_show(tgui_state_t *st) {
    if (st->connected && st->a != -1) {
        // already open: raise it. Android only honours this while the plugin
        // may start activities from the background ("Appear on top")
        if (!st->visible && st->task != -1) {
            tgui_task_to_front(st->c, st->task);
        }
        return true;
    }
    if (!st->connected && !tgui_connect(st)) {
        return false;
    }
    return tgui_open_window(st);
}

// The pointer a down or pointer-up event is about. With several fingers down
// the plugin sends one row per finger (`events` rows of one pointer), not the
// rows of `num_pointers` fingers that the header describes; counting through
// the rows covers both.
static const tgui_touch_pointer *tgui_event_pointer(const tgui_event *ev) {
    uint32_t i = ev->touch.action == TGUI_TOUCH_DOWN ? 0 : ev->touch.index;
    uint32_t n = ev->touch.num_pointers;
    return i < ev->touch.events * n ? &ev->touch.pointers[i / n][i % n] : NULL;
}

// All fingers on the grid arrive here. A finger holds the cell it landed on
// until it lifts; sliding does not move it to the next cell.
static void tgui_handle_grid_touch(tgui_state_t *st, const tgui_event *ev) {
    tgui_touches_t *t = &st->grid_touch;
    const tgui_touch_pointer *p = tgui_event_pointer(ev);
    int i;
    switch (ev->touch.action) {
    case TGUI_TOUCH_DOWN:
        tgui_grid_lift_all(st);
        tgui_touches_reset(t);
        // fall through
    case TGUI_TOUCH_POINTER_DOWN:
        if (p && !t->swiped && (i = tgui_touches_add(t, p)) >= 0) {
            int cx = (int)(p->x / st->grid_cell_px), cy = (int)(p->y / st->grid_cell_px);
            if (p->x >= 0 && p->y >= 0 && cx < GRID_COLS && cy < GRID_ROWS) {
                t->p[i].cx = cx;
                t->p[i].cy = cy;
                if (st->grid_held[cy][cx]++ == 0) {
                    tgui_post_grid_key(cx, cy, 1);
                }
                if (st->priv->debug) {
                    fprintf(stderr, "screen:tgui grid key %d,%d\n", cx, cy);
                }
            }
        }
        break;
    case TGUI_TOUCH_POINTER_UP:
        if (p && (i = tgui_touches_find(t, p->id)) >= 0) {
            tgui_grid_lift(st, t, i);
            t->p[i].id = -1;
        }
        break;
    case TGUI_TOUCH_UP:
    case TGUI_TOUCH_CANCEL:
        tgui_grid_lift_all(st);
        tgui_touches_reset(t);
        break;
    case TGUI_TOUCH_MOVE:
        if (tgui_touches_move(t, ev, TGUI_SWIPE_DP * st->density)) {
            tgui_grid_lift_all(st);
            st->want_page = TGUI_PAGE_SCREEN;
        }
        break;
    }
}

// The middle of the screen page only watches for the page swipe.
static void tgui_handle_gap_touch(tgui_state_t *st, const tgui_event *ev) {
    tgui_touches_t *t = &st->gap_touch;
    const tgui_touch_pointer *p = tgui_event_pointer(ev);
    int i;
    switch (ev->touch.action) {
    case TGUI_TOUCH_DOWN:
        tgui_touches_reset(t);
        // fall through
    case TGUI_TOUCH_POINTER_DOWN:
        if (p) {
            tgui_touches_add(t, p);
        }
        break;
    case TGUI_TOUCH_POINTER_UP:
        if (p && (i = tgui_touches_find(t, p->id)) >= 0) {
            t->p[i].id = -1;
        }
        break;
    case TGUI_TOUCH_UP:
    case TGUI_TOUCH_CANCEL:
        tgui_touches_reset(t);
        break;
    case TGUI_TOUCH_MOVE:
        if (tgui_touches_move(t, ev, TGUI_SWIPE_DP * st->density)) {
            st->want_page = TGUI_PAGE_GRID;
        }
        break;
    }
}

static void tgui_handle_touch(tgui_state_t *st, const tgui_event *ev) {
    int id = ev->touch.id;
    tgui_touch_action action = ev->touch.action;
    if (st->priv->debug && action != TGUI_TOUCH_MOVE) {
        fprintf(stderr, "screen:tgui touch view=%d%s action=%d index=%u events=%u pointers=%u:", id,
                id == st->grid_img ? " (grid)" : (id == st->gap ? " (gap)" : ""), (int)action, ev->touch.index,
                ev->touch.events, ev->touch.num_pointers);
        for (uint32_t k = 0; k < ev->touch.events * ev->touch.num_pointers; k++) {
            const tgui_touch_pointer *p = &ev->touch.pointers[k / ev->touch.num_pointers][k % ev->touch.num_pointers];
            fprintf(stderr, " #%d %d,%d", p->id, p->x, p->y);
        }
        const tgui_touches_t *t = id == st->grid_img ? &st->grid_touch : (id == st->gap ? &st->gap_touch : NULL);
        for (int i = 0; t && i < TGUI_MAX_PTR; i++) {
            if (t->p[i].id != -1) {
                fprintf(stderr, " [#%d moved %d,%d]", t->p[i].id, t->p[i].x - t->p[i].sx, t->p[i].y - t->p[i].sy);
            }
        }
        fprintf(stderr, "\n");
    }
    if (ev->touch.events == 0 || ev->touch.num_pointers == 0) {
        return;
    }
    if (id == st->grid_img) {
        tgui_handle_grid_touch(st, ev);
        return;
    }
    if (id == st->gap) {
        tgui_handle_gap_touch(st, ev);
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (id == st->key[i]) {
            bool down = st->key_down[i];
            if (action == TGUI_TOUCH_DOWN) {
                down = true;
            } else if (action == TGUI_TOUCH_UP || action == TGUI_TOUCH_CANCEL) {
                down = false;
            }
            if (down != st->key_down[i]) {
                st->key_down[i] = down;
                tgui_post_key(i + 1, down ? 1 : 0);
            }
            return;
        }
        if (id == st->enc[i]) {
            if (action == TGUI_TOUCH_DOWN) {
                const tgui_touch_pointer *p = &ev->touch.pointers[0][0];
                st->enc_ptr[i] = p->id;
                st->enc_x[i] = p->x;
                st->enc_y[i] = p->y;
                st->enc_acc[i] = 0;
            } else if (action == TGUI_TOUCH_UP || action == TGUI_TOUCH_CANCEL) {
                st->enc_ptr[i] = -1;
            } else if (action == TGUI_TOUCH_MOVE && st->enc_ptr[i] != -1) {
                for (uint32_t e = 0; e < ev->touch.events; e++) {
                    for (uint32_t k = 0; k < ev->touch.num_pointers; k++) {
                        const tgui_touch_pointer *p = &ev->touch.pointers[e][k];
                        if (p->id != st->enc_ptr[i]) {
                            continue;
                        }
                        // right or up is clockwise
                        st->enc_acc[i] += (float)((p->x - st->enc_x[i]) - (p->y - st->enc_y[i]));
                        st->enc_x[i] = p->x;
                        st->enc_y[i] = p->y;
                    }
                }
                int steps = (int)(st->enc_acc[i] / st->enc_step_px);
                if (steps != 0) {
                    st->enc_acc[i] -= steps * st->enc_step_px;
                    steps = steps > 127 ? 127 : (steps < -127 ? -127 : steps);
                    tgui_post_enc(i + 1, steps);
                }
            }
            return;
        }
    }
}

static void *tgui_event_loop(void *data) {
    tgui_state_t *st = (tgui_state_t *)data;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while (!st->evt_stop) {
        tgui_event ev;
        tgui_err e = tgui_wait_event(st->c, &ev);
        if (e != TGUI_ERR_OK) {
            if (!st->evt_stop) {
                fprintf(stderr, "WARN (screen:tgui) lost connection to Termux:GUI: wait_event (%d)\n", (int)e);
                st->lost = true;
            }
            break;
        }
        if (st->priv->debug && ev.type != TGUI_EVENT_TOUCH) {
            fprintf(stderr, "screen:tgui event type=%d activity=%d\n", (int)ev.type, (int)ev.activity);
        }
        pthread_mutex_lock(&st->lock);
        if (ev.activity == st->a && st->a != -1) {
            switch (ev.type) {
            case TGUI_EVENT_TOUCH:
                tgui_handle_touch(st, &ev);
                break;
            case TGUI_EVENT_START:
            case TGUI_EVENT_RESUME:
                st->visible = true;
                st->priv->dirty = true;
                break;
            case TGUI_EVENT_STOP:
                st->visible = false;
                tgui_release_all(st);
                break;
            case TGUI_EVENT_CONFIG:
                if (st->priv->debug) {
                    fprintf(stderr, "screen:tgui config %dx%d dp, density %.2f\n", ev.configuration.screen_width,
                            ev.configuration.screen_height, ev.configuration.density);
                }
                tgui_set_configuration(st, &ev.configuration);
                // the size also changes when the system bars go away
                if (tgui_is_landscape(st) != st->landscape ||
                    (tgui_has_grid(st) && (st->conf_w != st->built_w || st->conf_h != st->built_h))) {
                    st->want_layout = true;
                }
                break;
            case TGUI_EVENT_BACK:
                if (st->page == TGUI_PAGE_GRID) {
                    st->want_page = TGUI_PAGE_SCREEN;
                    break;
                }
                tgui_activity_task_to_back(st->c, st->a);
                break;
            case TGUI_EVENT_DESTROY:
                fprintf(stderr, "screen:tgui window closed\n");
                tgui_release_all(st);
                st->a = -1;
                st->visible = false;
                st->want_layout = false;
                break;
            default:
                break;
            }
        }
        pthread_mutex_unlock(&st->lock);
        tgui_event_destroy(&ev);
    }
    st->evt_done = true;
    return NULL;
}

// Nearest-neighbour upscale of the 128x64 surface into the shared buffer.
// cairo ARGB32 is B,G,R,A in memory, the Android bitmap is R,G,B,A.
static void tgui_upscale(tgui_state_t *st) {
    const int s = st->scale;
    const int dst_w = SCREEN_W * s;
    uint32_t *dst = (uint32_t *)st->buf->data;
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *line = dst + (size_t)y * s * dst_w;
        uint32_t *out = line;
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t p = st->src[y * SCREEN_W + x];
            uint32_t q = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff);
            for (int k = 0; k < s; k++) {
                *out++ = q;
            }
        }
        for (int k = 1; k < s; k++) {
            memcpy(line + (size_t)k * dst_w, line, (size_t)dst_w * sizeof(uint32_t));
        }
    }
}

// Draws the grid leds into its buffer: a square per cell, brighter with level.
static void tgui_draw_grid(tgui_state_t *st) {
    const int cell = 8 * st->scale;
    const int dst_w = SCREEN_W * st->scale;
    const int inset = cell / 10 + 1;
    uint32_t *dst = (uint32_t *)st->grid_buf->data;
    memset(dst, 0, (size_t)dst_w * SCREEN_H * st->scale * sizeof(uint32_t));
    for (int cy = 0; cy < GRID_ROWS; cy++) {
        for (int cx = 0; cx < GRID_COLS; cx++) {
            // warm white, with the unlit pads still visible
            uint32_t v = 34 + (tgui_grid_led[cy][cx] & 15) * 14;
            uint32_t q = 0xff000000u | ((v * 5 / 8) << 16) | ((v * 7 / 8) << 8) | v;
            for (int y = cy * cell + inset; y < (cy + 1) * cell - inset; y++) {
                uint32_t *out = dst + (size_t)y * dst_w + cx * cell + inset;
                for (int x = 0; x < cell - 2 * inset; x++) {
                    *out++ = q;
                }
            }
        }
    }
}

static bool tgui_present(tgui_state_t *st, tgui_activity a, int page) {
    tgui_buffer *buf = page == TGUI_PAGE_GRID ? st->grid_buf : st->buf;
    pthread_mutex_lock(&tgui_lock);
    if (page == TGUI_PAGE_GRID) {
        tgui_grid_dirty = false;
        tgui_draw_grid(st);
    } else {
        st->priv->dirty = false;
        tgui_upscale(st);
    }
    pthread_mutex_unlock(&tgui_lock);

    const char *what = "blit_buffer";
    tgui_err e = tgui_blit_buffer(st->c, buf);
    if (e == TGUI_ERR_OK) {
        what = "refresh_image_view";
        e = tgui_refresh_image_view(st->c, a, page == TGUI_PAGE_GRID ? st->grid_img : st->img);
    }
    if (e == TGUI_ERR_OK || e == TGUI_ERR_ACTIVITY_DESTROYED) {
        st->present_failed = 0;
        return true; // a closed window is reported by its DESTROY event
    }
    if (e == TGUI_ERR_MESSAGE && ++st->present_failed < 120) {
        // the plugin declined (see TGUI_TRY); draw again on the next frame
        if (page == TGUI_PAGE_GRID) {
            tgui_grid_dirty = true;
        } else {
            st->priv->dirty = true;
        }
        return true;
    }
    fprintf(stderr, "WARN (screen:tgui) lost connection to Termux:GUI: %s (%d)\n", what, (int)e);
    return false;
}

static void *screen_tgui_loop(void *data) {
    tgui_state_t *st = (tgui_state_t *)data;
    screen_tgui_priv_t *priv = st->priv;

    // a dead plugin must surface as an error return, not kill matron
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    static const struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 16666666, // 60Hz
    };
    int show_retries = 2;
    while (priv->running) {
        bool ok = true;
        bool present = false;
        bool opening = false;
        int page = TGUI_PAGE_SCREEN;
        tgui_activity a = -1;

        pthread_mutex_lock(&st->lock);
        if (priv->want_show) {
            priv->want_show = false;
            opening = true;
            ok = tgui_show(st);
        }
        if (ok && st->want_layout) {
            st->want_layout = false;
            if (st->connected && st->a != -1) {
                ok = tgui_build_layout(st);
            }
        }
        if (ok && st->connected && st->a != -1 && st->has_root) {
            if (st->want_page != st->page) {
                ok = tgui_turn_page(st);
            }
            page = st->page;
            if (ok && st->visible && (page == TGUI_PAGE_GRID ? tgui_grid_dirty : priv->dirty)) {
                present = true;
                a = st->a;
            }
        }
        pthread_mutex_unlock(&st->lock);

        if (present) {
            ok = tgui_present(st, a, page);
        }
        if (!ok || st->lost) {
            tgui_disconnect(st);
            // building the window now and then fails on a fresh connection
            // (TGUI_ERR_MESSAGE from an arbitrary call); try that again
            if (opening && show_retries > 0) {
                show_retries--;
                priv->want_show = true;
            }
        } else if (opening) {
            show_retries = 2;
        }
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }

    if (st->connected && st->a != -1) {
        tgui_activity_finish(st->c, st->a);
    }
    tgui_disconnect(st);
    pthread_mutex_destroy(&st->lock);
    free(st);
    return NULL;
}
