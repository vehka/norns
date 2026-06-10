#include <SDL2/SDL.h>

#include "event_types.h"
#include "events.h"
#include "hardware/input.h"
#include "hardware/input/inputs.h"
#include "hardware/io.h"

typedef struct _input_sdl_priv {
} input_sdl_priv_t;

static int input_sdl_config(matron_io_t *io, lua_State *l);
static int input_sdl_setup(matron_io_t *io);
static void input_sdl_destroy(matron_io_t *io);
static void *input_sdl_poll(void *data);

input_ops_t input_sdl_ops = {
    {
        // .io_ops
        "input:sdl",               // .name
        IO_INPUT,                  // .type
        sizeof(input_sdl_priv_t),  // .data_size
        input_sdl_config,          // .config
        input_sdl_setup,           // .setup
        input_sdl_destroy,         // .destroy
    },
    input_sdl_poll, // .poll
};

int input_sdl_config(matron_io_t *io, lua_State *l) {
    (void)io;
    (void)l;
    return 0;
}

int input_sdl_setup(matron_io_t *io) {
    return input_setup(io);
}

void input_sdl_destroy(matron_io_t *io) {
    input_destroy(io);
}

static void *input_sdl_poll(void *data) {
    (void)data;
    // No-op: SDL keyboard events must be pumped on the thread that owns the SDL
    // window, so input is handled in the screen render loop instead
    // (matron/src/hardware/screen/sdl.cc, screen_sdl_handle_key). This IO is
    // kept only so `_boot.add_io('input:sdl', {})` still resolves.
    return NULL;
}
