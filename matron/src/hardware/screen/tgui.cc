// screen:tgui -- norns screen and controls in a Termux:GUI activity (Android).
//
// The 128x64 screen is upscaled into a shared image buffer shown in an
// ImageView; below it are touch pads for the three keys and three encoders,
// laid out like the hardware:
//
//     +-----------------+
//     |     screen      |
//     +--------+--------+
//     |   K1   |   E1   |
//     +--------+--------+
//     |   E2   |   E3   |
//     +--------+--------+
//     |   K2   |   K3   |
//     +--------+--------+
//
// Keys follow touch down/up, so holding works. Encoders are drag pads: right
// or up is clockwise, one step per `enc_step_dp` of travel.
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

#include "event_types.h"
#include "events.h"
#include "hardware/io.h"
#include "hardware/screen/screens.h"

#define SCREEN_W 128
#define SCREEN_H 64

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
    int scale;

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
int screen_tgui_config(matron_io_t *io, lua_State *l) {
    screen_tgui_priv_t *priv = (screen_tgui_priv_t *)io->data;
    memset(priv, 0, sizeof(*priv));
    priv->enc_step_dp = 12.f;
    priv->keep_screen_on = true;

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
    return 0;
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

static void tgui_release_all(tgui_state_t *st) {
    for (int i = 0; i < 3; i++) {
        if (st->key_down[i]) {
            st->key_down[i] = false;
            tgui_post_key(i + 1, 0);
        }
        st->enc_ptr[i] = -1;
    }
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
        tgui_connection_destroy(st->c);
    }
    free(st->buf);
    st->buf = NULL;
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
    if (pthread_create(&st->evt_thread, NULL, &tgui_event_loop, st) != 0) {
        fprintf(stderr, "WARN (screen:tgui) failed to start event thread\n");
        return false;
    }
    st->evt_started = true;
    return true;
}

#define TGUI_TRY(call)                                                        \
    do {                                                                      \
        tgui_err e_ = (call);                                                 \
        if (e_ != TGUI_ERR_OK) {                                              \
            fprintf(stderr, "WARN (screen:tgui) %s failed (%d)\n", #call, (int)e_); \
            return false;                                                     \
        }                                                                     \
    } while (0)

static bool tgui_make_pad(tgui_state_t *st, tgui_view *v, const tgui_view *row, const char *label) {
    tgui_view_size fill = {TGUI_VIEW_MATCH_PARENT, {TGUI_UNIT_DP, 0}};
    TGUI_TRY(tgui_create_button(st->c, st->a, v, row, TGUI_VIS_VISIBLE, label));
    TGUI_TRY(tgui_set_height(st->c, st->a, *v, fill));
    TGUI_TRY(tgui_linear_params(st->c, st->a, *v, 1, 0));
    TGUI_TRY(tgui_text_size(st->c, st->a, *v, {TGUI_UNIT_SP, 24}));
    TGUI_TRY(tgui_send_touch_event(st->c, st->a, *v, true));
    return true;
}

static bool tgui_open_window(tgui_state_t *st) {
    st->a = -1;
    // intercept the back button: it hides the window instead of closing it
    st->task = -1;
    TGUI_TRY(tgui_activity_create(st->c, &st->a, TGUI_ACTIVITY_NORMAL, &st->task, true));
    tgui_activity_set_orientation(st->c, st->a, TGUI_ORIENTATION_PORTRAIT);
    tgui_activity_set_keep_screen_on(st->c, st->a, st->priv->keep_screen_on);

    tgui_activity_configuration conf;
    TGUI_TRY(tgui_activity_get_configuration(st->c, st->a, &conf));
    int width_dp = conf.screen_width < conf.screen_height ? conf.screen_width : conf.screen_height;
    st->enc_step_px = st->priv->enc_step_dp * (float)conf.density;

    if (!st->buf) {
        // largest integer scale that fits the display width
        int scale = (int)(width_dp * conf.density) / SCREEN_W;
        st->scale = scale < 2 ? 2 : (scale > 12 ? 12 : scale);
        tgui_buffer b = {-1, -1, (uint32_t)(SCREEN_W * st->scale), (uint32_t)(SCREEN_H * st->scale),
                         TGUI_BUFFER_FORMAT_ARGB8888, NULL};
        st->buf = (tgui_buffer *)malloc(sizeof(tgui_buffer));
        if (!st->buf) {
            return false;
        }
        memcpy(st->buf, &b, sizeof(b));
        tgui_err e = tgui_add_buffer(st->c, st->buf);
        if (e != TGUI_ERR_OK) {
            fprintf(stderr, "WARN (screen:tgui) tgui_add_buffer failed (%d)\n", (int)e);
            free(st->buf);
            st->buf = NULL;
            return false;
        }
    }

    tgui_view root, row[3];
    tgui_view_size img_h = {TGUI_VIEW_SIZE, {TGUI_UNIT_DP, width_dp / 2.f}};
    TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &root, NULL, TGUI_VIS_VISIBLE, false));
    TGUI_TRY(tgui_create_image_view(st->c, st->a, &st->img, &root, TGUI_VIS_VISIBLE, false));
    TGUI_TRY(tgui_set_height(st->c, st->a, st->img, img_h));
    TGUI_TRY(tgui_linear_params(st->c, st->a, st->img, 0, 0));
    TGUI_TRY(tgui_set_buffer(st->c, st->a, st->img, st->buf));
    for (int i = 0; i < 3; i++) {
        TGUI_TRY(tgui_create_linear_layout(st->c, st->a, &row[i], &root, TGUI_VIS_VISIBLE, true));
        TGUI_TRY(tgui_linear_params(st->c, st->a, row[i], 1, 0));
    }
    if (!tgui_make_pad(st, &st->key[0], &row[0], "K1") || !tgui_make_pad(st, &st->enc[0], &row[0], "E1") ||
        !tgui_make_pad(st, &st->enc[1], &row[1], "E2") || !tgui_make_pad(st, &st->enc[2], &row[1], "E3") ||
        !tgui_make_pad(st, &st->key[1], &row[2], "K2") || !tgui_make_pad(st, &st->key[2], &row[2], "K3")) {
        return false;
    }
    tgui_release_all(st);
    st->visible = true;
    st->priv->dirty = true;
    fprintf(stderr, "screen:tgui window open, %dx%d buffer\n", SCREEN_W * st->scale, SCREEN_H * st->scale);
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

static void tgui_handle_touch(tgui_state_t *st, const tgui_event *ev) {
    int id = ev->touch.id;
    tgui_touch_action action = ev->touch.action;
    if (st->priv->debug && action != TGUI_TOUCH_MOVE) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        fprintf(stderr, "screen:tgui touch view=%d action=%d events=%u pointers=%u t=%llu now=%lld\n", id,
                (int)action, ev->touch.events, ev->touch.num_pointers, (unsigned long long)ev->touch.time,
                (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000);
    }
    if (ev->touch.events == 0 || ev->touch.num_pointers == 0) {
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
            case TGUI_EVENT_BACK:
                tgui_activity_task_to_back(st->c, st->a);
                break;
            case TGUI_EVENT_DESTROY:
                fprintf(stderr, "screen:tgui window closed\n");
                tgui_release_all(st);
                st->a = -1;
                st->visible = false;
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

static bool tgui_present(tgui_state_t *st, tgui_activity a) {
    pthread_mutex_lock(&tgui_lock);
    st->priv->dirty = false;
    tgui_upscale(st);
    pthread_mutex_unlock(&tgui_lock);

    const char *what = "blit_buffer";
    tgui_err e = tgui_blit_buffer(st->c, st->buf);
    if (e == TGUI_ERR_OK) {
        what = "refresh_image_view";
        e = tgui_refresh_image_view(st->c, a, st->img);
    }
    if (e == TGUI_ERR_OK || e == TGUI_ERR_ACTIVITY_DESTROYED) {
        return true; // a closed window is reported by its DESTROY event
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
    while (priv->running) {
        bool ok = true;
        bool present = false;
        tgui_activity a = -1;

        pthread_mutex_lock(&st->lock);
        if (priv->want_show) {
            priv->want_show = false;
            ok = tgui_show(st);
        }
        if (ok && st->connected && st->a != -1 && st->visible && priv->dirty) {
            present = true;
            a = st->a;
        }
        pthread_mutex_unlock(&st->lock);

        if (present) {
            ok = tgui_present(st, a);
        }
        if (!ok || st->lost) {
            tgui_disconnect(st);
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
