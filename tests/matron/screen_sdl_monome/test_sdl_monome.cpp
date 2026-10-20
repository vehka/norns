// tests for the virtual grid and arc of the desktop SDL backend,
// matron/src/hardware/screen/sdl_monome.cc: configuration, and the input
// logic that turns clicks, drags and the hold modifier into grid and arc
// events. Windows and drawing are not tested.
//
// The functions are `static` in sdl_monome.cc, so this test #includes the .cc
// (see tests/matron/screen_sdl for why the directory is not named after it).
// The event queue and the device constructors are faked in common/.
//
// Desktop-only: tests/wscript builds this unit only with --desktop.

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "event_types.h"
#include "events.h"

extern const std::vector<union event_data> &test_posted_events();
extern void test_clear_posted_events();

#include "hardware/screen/sdl_monome.cc"

#include <lauxlib.h>

// a stand-in for an open window: only compared against null and by id
static SDL_Window *const FAKE_WIN = (SDL_Window *)0x1;

// fresh state with a grid and an arc, laid out at a known size
static void setup(SDL_Keymod mod = KMOD_ALT) {
    vg = vgrid_t{};
    va = varc_t{};
    sdl_monome_conf_t conf;
    sdl_monome_conf_defaults(&conf);
    conf.arc = true;
    sdl_monome_init(&conf);
    hold_mod = mod;
    cur_mods = KMOD_NONE;
    vg.w.win = FAKE_WIN;
    vg.w.id = 7;
    va.w.win = FAKE_WIN;
    va.w.id = 8;
    vgrid_layout(660, 340); // pitch 40: grid from (0, 10) to (640, 330)
    varc_layout(640, 160);  // squares of 160 from (0, 0)
    test_clear_posted_events();
}

struct grid_key {
    int x, y, z;
    bool operator==(const grid_key &o) const { return x == o.x && y == o.y && z == o.z; }
};

static std::vector<grid_key> grid_keys() {
    std::vector<grid_key> out;
    for (const auto &e : test_posted_events()) {
        if (e.type == EVENT_GRID_KEY) {
            out.push_back({e.grid_key.x, e.grid_key.y, e.grid_key.state});
        }
    }
    test_clear_posted_events();
    return out;
}

// the centre of grid key (x, y) in the window
static int kx(int x) { return vg.ox + x * vg.pitch + vg.pitch / 2; }
static int ky(int y) { return vg.oy + y * vg.pitch + vg.pitch / 2; }

static SDL_Event mouse(Uint32 type, Uint32 win, int x, int y) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_MOUSEMOTION) {
        e.motion.windowID = win;
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.windowID = win;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    }
    return e;
}

static SDL_Event key(Uint32 type, SDL_Scancode sc, Uint16 mod) {
    SDL_Event e{};
    e.type = type;
    e.key.keysym.scancode = sc;
    e.key.keysym.mod = mod;
    return e;
}

// -----------------------------------------------------------------------------
// configuration

TEST_CASE("defaults: a 16x8 grid, no arc") {
    sdl_monome_conf_t c;
    sdl_monome_conf_defaults(&c);
    CHECK(c.grid);
    CHECK(c.grid_cols == 16);
    CHECK(c.grid_rows == 8);
    CHECK_FALSE(c.arc);
}

static sdl_monome_conf_t conf_from(const char *lua_table) {
    lua_State *l = luaL_newstate();
    std::string chunk = std::string("return ") + lua_table;
    REQUIRE(luaL_dostring(l, chunk.c_str()) == 0);
    // the options table sits at index 2 when screen_sdl_config reads it
    lua_pushnil(l);
    lua_insert(l, 1);
    sdl_monome_conf_t c;
    sdl_monome_conf_defaults(&c);
    sdl_monome_config(&c, l, 2);
    CHECK(lua_gettop(l) == 2); // the stack is left as it was
    lua_close(l);
    return c;
}

TEST_CASE("config: booleans turn the devices on and off") {
    sdl_monome_conf_t c = conf_from("{ grid = false, arc = true }");
    CHECK_FALSE(c.grid);
    CHECK(c.arc);
}

TEST_CASE("config: a table turns the device on and sets its fields") {
    sdl_monome_conf_t c = conf_from("{ grid = { cols = 8, rows = 16, size = 30 }, arc = { rings = 2, wheel = 4 } }");
    CHECK(c.grid);
    CHECK(c.grid_cols == 8);
    CHECK(c.grid_rows == 16);
    CHECK(c.grid_size == 30);
    CHECK(c.arc);
    CHECK(c.arc_rings == 2);
    CHECK(c.arc_wheel == 4);
}

TEST_CASE("config: out of range values keep the default") {
    sdl_monome_conf_t c = conf_from("{ grid = { cols = 32, rows = 0 }, arc = { rings = 5 } }");
    CHECK(c.grid_cols == 16);
    CHECK(c.grid_rows == 8);
    CHECK(c.arc_rings == 4);
}

TEST_CASE("init announces each device once") {
    vg = vgrid_t{};
    va = varc_t{};
    test_clear_posted_events();
    sdl_monome_conf_t c;
    sdl_monome_conf_defaults(&c);
    sdl_monome_init(&c);
    REQUIRE(test_posted_events().size() == 1);
    CHECK(test_posted_events()[0].type == EVENT_MONOME_ADD);
    CHECK(test_posted_events()[0].monome_add.dev == vg.dev);
    sdl_monome_init(&c);
    CHECK(test_posted_events().size() == 1);
    CHECK(va.dev == nullptr);
}

// -----------------------------------------------------------------------------
// grid

TEST_CASE("grid: layout and hit test") {
    setup();
    CHECK(vg.pitch == 40);
    CHECK(vgrid_key_at(kx(0), ky(0)) == 0);
    CHECK(vgrid_key_at(kx(15), ky(7)) == 7 * 16 + 15);
    CHECK(vgrid_key_at(vg.ox - 1, ky(0)) == -1);
    CHECK(vgrid_key_at(kx(0), vg.oy + 8 * vg.pitch) == -1);
}

TEST_CASE("grid: a click holds the key while the button is down") {
    setup();
    vgrid_press(kx(3), ky(2), false);
    CHECK(grid_keys() == std::vector<grid_key>{{3, 2, 1}});
    vgrid_release();
    CHECK(grid_keys() == std::vector<grid_key>{{3, 2, 0}});
}

TEST_CASE("grid: a drag slides the press from key to key") {
    setup();
    vgrid_press(kx(0), ky(0), false);
    vgrid_motion(kx(0) + 5, ky(0)); // same key: nothing
    vgrid_motion(kx(1), ky(0));
    vgrid_motion(-50, ky(0)); // off the grid
    vgrid_release();
    CHECK(grid_keys() == std::vector<grid_key>{{0, 0, 1}, {0, 0, 0}, {1, 0, 1}, {1, 0, 0}});
}

TEST_CASE("grid: held clicks stay down until the modifier comes up") {
    setup();
    vgrid_press(kx(1), ky(1), true);
    vgrid_release();
    vgrid_press(kx(2), ky(1), true);
    vgrid_release();
    CHECK(grid_keys() == std::vector<grid_key>{{1, 1, 1}, {2, 1, 1}});
    sdl_monome_check_hold(KMOD_LALT); // still down
    CHECK(grid_keys().empty());
    sdl_monome_check_hold(KMOD_NONE);
    CHECK(grid_keys() == std::vector<grid_key>{{1, 1, 0}, {2, 1, 0}});
}

TEST_CASE("grid: a held click on a held key lets it go") {
    setup();
    vgrid_press(kx(4), ky(4), true);
    vgrid_press(kx(4), ky(4), true);
    CHECK(grid_keys() == std::vector<grid_key>{{4, 4, 1}, {4, 4, 0}});
}

TEST_CASE("grid: a plain click on a held key does not let it go") {
    setup();
    vgrid_press(kx(5), ky(0), true);
    vgrid_press(kx(5), ky(0), false);
    vgrid_release();
    CHECK(grid_keys() == std::vector<grid_key>{{5, 0, 1}});
}

TEST_CASE("grid: events: alt down, click, alt up in one batch") {
    setup();
    // SDL_GetModState() would already say alt is up at the click; the
    // modifier must come from the key events in order
    SDL_Event ev[] = {
        key(SDL_KEYDOWN, SDL_SCANCODE_LALT, KMOD_LALT),
        mouse(SDL_MOUSEBUTTONDOWN, 7, kx(6), ky(3)),
        mouse(SDL_MOUSEBUTTONUP, 7, kx(6), ky(3)),
        key(SDL_KEYUP, SDL_SCANCODE_LALT, KMOD_NONE),
    };
    CHECK_FALSE(sdl_monome_handle_event(&ev[0])); // keys are never consumed
    CHECK(sdl_monome_handle_event(&ev[1]));
    CHECK(sdl_monome_handle_event(&ev[2]));
    CHECK(grid_keys() == std::vector<grid_key>{{6, 3, 1}}); // held past the button
    sdl_monome_handle_event(&ev[3]);
    CHECK(grid_keys() == std::vector<grid_key>{{6, 3, 0}});
}

TEST_CASE("grid: events for other windows are left alone") {
    setup();
    SDL_Event e = mouse(SDL_MOUSEBUTTONDOWN, 99, kx(0), ky(0));
    CHECK_FALSE(sdl_monome_handle_event(&e));
    CHECK(grid_keys().empty());
}

TEST_CASE("grid: no hold modifier, no holding") {
    setup(KMOD_NONE);
    SDL_Event ev[] = {
        key(SDL_KEYDOWN, SDL_SCANCODE_LALT, KMOD_LALT),
        mouse(SDL_MOUSEBUTTONDOWN, 7, kx(0), ky(0)),
        mouse(SDL_MOUSEBUTTONUP, 7, kx(0), ky(0)),
    };
    for (auto &e : ev) {
        sdl_monome_handle_event(&e);
    }
    CHECK(grid_keys() == std::vector<grid_key>{{0, 0, 1}, {0, 0, 0}});
}

TEST_CASE("grid: release all lets every key go") {
    setup();
    vgrid_press(kx(0), ky(0), true);
    vgrid_press(kx(1), ky(0), false);
    grid_keys();
    sdl_monome_release_all();
    CHECK(grid_keys() == std::vector<grid_key>{{1, 0, 0}, {0, 0, 0}});
}

TEST_CASE("grid: refresh hands the levels to the render thread") {
    setup();
    vg.dev->data[0][3] = 9;
    vgrid_refresh(vg.dev);
    CHECK(vg.led_dirty);
    CHECK(vg.led[3] == 9);
}

// -----------------------------------------------------------------------------
// arc

static int arc_ticks(int n) {
    int sum = 0;
    for (const auto &e : test_posted_events()) {
        if (e.type == EVENT_ARC_ENCODER_DELTA && e.arc_encoder_delta.number == n) {
            sum += e.arc_encoder_delta.delta;
        }
    }
    return sum;
}

static std::vector<int> arc_keys(int n) {
    std::vector<int> out;
    for (const auto &e : test_posted_events()) {
        if (e.type == EVENT_ARC_ENCODER_KEY && e.arc_encoder_key.number == n) {
            out.push_back(e.arc_encoder_key.state);
        }
    }
    return out;
}

TEST_CASE("arc: the disc in the middle is the ring's key") {
    setup();
    varc_press(80 + 160 * 2, 80, false);
    varc_release();
    CHECK(arc_keys(2) == std::vector<int>{1, 0});
}

TEST_CASE("arc: a held click on the key stays down until the modifier comes up") {
    setup();
    varc_press(80, 80, true);
    varc_release();
    CHECK(arc_keys(0) == std::vector<int>{1});
    sdl_monome_check_hold(KMOD_NONE);
    CHECK(arc_keys(0) == std::vector<int>{1, 0});
}

TEST_CASE("arc: a quarter turn clockwise is a quarter of the ticks") {
    setup();
    const float cx = 80 + 160, cy = 80, r = 60;
    varc_press(cx, cy - r, false); // top of ring 1
    for (int i = 1; i <= 30; i++) {
        float a = (float)M_PI / 2 * i / 30;
        varc_motion(cx + r * sinf(a), cy - r * cosf(a));
    }
    varc_release();
    CHECK(abs(arc_ticks(1) - ARC_TICKS / 4) <= 1);
    CHECK(arc_ticks(0) == 0);
    CHECK(arc_keys(1).empty());
}

TEST_CASE("arc: the ring turns wherever the drag goes") {
    setup();
    const float cx = 80, cy = 80;
    varc_press(cx, cy - 60, false);
    varc_motion(cx + 300, cy); // far right, past ring 1 and 2: still ring 0
    varc_release();
    CHECK(abs(arc_ticks(0) - ARC_TICKS / 4) <= 1);
    CHECK(arc_ticks(1) == 0);
}

TEST_CASE("arc: counterclockwise is negative") {
    setup();
    const float cx = 80, cy = 80;
    varc_press(cx, cy - 60, false);
    varc_motion(cx - 60, cy);
    CHECK(abs(arc_ticks(0) + ARC_TICKS / 4) <= 1);
}

TEST_CASE("arc: the wheel turns the ring under the pointer") {
    setup();
    varc_wheel(80 + 160 * 3, 20, 2);
    varc_wheel(80 + 160 * 3, 20, -1);
    CHECK(arc_ticks(3) == va.wheel);
}

TEST_CASE("arc: large turns are split into deltas that fit") {
    setup();
    varc_turn(0, 300);
    for (const auto &e : test_posted_events()) {
        CHECK(abs(e.arc_encoder_delta.delta) <= 127);
    }
    CHECK(arc_ticks(0) == 300);
}

TEST_CASE("arc: rings past the configured count are not there") {
    setup();
    va.rings = 2;
    varc_layout(320, 160);
    float dx, dy;
    CHECK(varc_ring_at(80 + 160, 80, &dx, &dy) == 1);
    CHECK(varc_ring_at(80 + 320, 80, &dx, &dy) == -1);
}
