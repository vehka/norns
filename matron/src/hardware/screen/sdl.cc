#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <cairo.h>

#include "hardware/io.h"
#include "hardware/screen/screens.h"

typedef struct _screen_sdl_priv {
    SDL_Window *window;
    SDL_Surface *window_surface;
    SDL_Surface *draw_surface;
    pthread_t render_thread;
    bool render_running;
    bool thread_started;
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

int screen_sdl_config(matron_io_t *io, lua_State *l) {
    (void)io;
    (void)l;
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

static void *screen_sdl_render_loop(void *data) {
    screen_sdl_priv_t *priv = (screen_sdl_priv_t *)data;

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
        SDL_PumpEvents();
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
