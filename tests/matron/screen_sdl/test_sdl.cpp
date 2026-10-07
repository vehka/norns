// tests for the desktop (NORNS_DESKTOP) SDL window input logic in
// matron/src/hardware/screen/sdl.cc:
//   - sdl_scancode_to_evdev: SDL scancode -> linux evdev KEY_* mapping
//   - screen_sdl_input_defaults / screen_sdl_grid_lookup: the enc/key grid
//   - screen_sdl_handle_key: full SDL-key -> matron-event translation
//     (modifier gating, encoder-step bypass, keyboard passthrough, press/release
//     pairing)
//
// These functions are `static` inside sdl.cc and depend on a struct that is
// private to that translation unit, so this test #includes the .cc directly to
// reach them. Because sdl.cc defines the non-static `screen_sdl_ops` global, the
// directory is deliberately named `screen_sdl/` (not mirroring
// matron/src/hardware/screen/sdl) so the test runner's mirror-mapper does NOT
// also compile sdl.cc and produce a duplicate symbol. The two event-queue entry
// points sdl.cc calls are faked in common/event_stubs.cpp.
//
// Desktop-only: tests/wscript skips this unit unless built with --desktop, where
// it links SDL2/cairo/lua (sdl.cc's dependencies).

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "event_types.h"
#include "events.h"
#include <linux/input-event-codes.h>

// recorded by the capturing event_post stub (common/event_stubs.cpp)
extern const std::vector<union event_data> &test_posted_events();
extern void test_clear_posted_events();

// pull in the unit under test so its static functions are visible here.
#include "hardware/screen/sdl.cc"

// -----------------------------------------------------------------------------
// scancode -> linux evdev mapping

TEST_CASE("sdl_scancode_to_evdev maps representative keys") {
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_A) == KEY_A);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_Z) == KEY_Z);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_1) == KEY_1);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_0) == KEY_0);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_SPACE) == KEY_SPACE);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_RETURN) == KEY_ENTER);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_ESCAPE) == KEY_ESC);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_NONUSBACKSLASH) == KEY_102ND);
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_NONUSHASH) == KEY_BACKSLASH);
}

TEST_CASE("sdl_scancode_to_evdev returns 0 for unmapped scancodes") {
    CHECK(sdl_scancode_to_evdev(SDL_SCANCODE_AUDIOMUTE) == 0);
}

// -----------------------------------------------------------------------------
// config defaults

TEST_CASE("screen_sdl_input_defaults match the documented Alt grid") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);

    CHECK(priv.grid_mod == KMOD_ALT);
    CHECK(priv.enc_step == 1);
    CHECK(priv.key_sc[0] == SDL_SCANCODE_1);
    CHECK(priv.key_sc[1] == SDL_SCANCODE_2);
    CHECK(priv.key_sc[2] == SDL_SCANCODE_3);
    CHECK(priv.enc_dn[0] == SDL_SCANCODE_Q);
    CHECK(priv.enc_up[0] == SDL_SCANCODE_W);
    CHECK(priv.enc_dn[2] == SDL_SCANCODE_Z);
    CHECK(priv.enc_up[2] == SDL_SCANCODE_X);
}

// -----------------------------------------------------------------------------
// grid lookup

TEST_CASE("screen_sdl_grid_lookup resolves keys and encoders") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    bool is_key = false;
    int n = 0, delta = 0;

    SUBCASE("key 2") {
        REQUIRE(screen_sdl_grid_lookup(&priv, SDL_SCANCODE_2, &is_key, &n, &delta));
        CHECK(is_key);
        CHECK(n == 2);
    }
    SUBCASE("encoder 1 increment") {
        REQUIRE(screen_sdl_grid_lookup(&priv, SDL_SCANCODE_W, &is_key, &n, &delta));
        CHECK_FALSE(is_key);
        CHECK(n == 1);
        CHECK(delta == 1);
    }
    SUBCASE("encoder 3 decrement") {
        REQUIRE(screen_sdl_grid_lookup(&priv, SDL_SCANCODE_Z, &is_key, &n, &delta));
        CHECK_FALSE(is_key);
        CHECK(n == 3);
        CHECK(delta == -1);
    }
    SUBCASE("unmapped scancode misses") {
        CHECK_FALSE(screen_sdl_grid_lookup(&priv, SDL_SCANCODE_P, &is_key, &n, &delta));
    }
}

TEST_CASE("screen_sdl_grid_lookup: keys win over encoders on collision") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    priv.enc_dn[0] = SDL_SCANCODE_1; // collide enc1-down onto K1
    bool is_key = false;
    int n = 0, delta = 0;

    REQUIRE(screen_sdl_grid_lookup(&priv, SDL_SCANCODE_1, &is_key, &n, &delta));
    CHECK(is_key); // key wins
    CHECK(n == 1);
}

// -----------------------------------------------------------------------------
// full key-event translation

static SDL_KeyboardEvent make_key(SDL_Scancode sc, bool down, Uint16 mod, bool repeat) {
    SDL_KeyboardEvent ke;
    memset(&ke, 0, sizeof(ke));
    ke.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ke.repeat = repeat ? 1 : 0;
    ke.keysym.scancode = sc;
    ke.keysym.mod = mod;
    return ke;
}

TEST_CASE("screen_sdl_handle_key: modifier+key emits K-down then K-up") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    test_clear_posted_events();

    SDL_KeyboardEvent down = make_key(SDL_SCANCODE_1, true, KMOD_LALT, false);
    screen_sdl_handle_key(&priv, &down);
    SDL_KeyboardEvent up = make_key(SDL_SCANCODE_1, false, KMOD_LALT, false);
    screen_sdl_handle_key(&priv, &up);

    const auto &ev = test_posted_events();
    REQUIRE(ev.size() == 2);
    CHECK(ev[0].type == EVENT_KEY);
    CHECK(ev[0].key.n == 1);
    CHECK(ev[0].key.val == 1);
    CHECK(ev[1].type == EVENT_KEY);
    CHECK(ev[1].key.n == 1);
    CHECK(ev[1].key.val == 0);
}

TEST_CASE("screen_sdl_handle_key: modifier+encoder emits one step, scaled by enc_step") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    priv.enc_step = 4;
    test_clear_posted_events();

    SDL_KeyboardEvent down = make_key(SDL_SCANCODE_W, true, KMOD_LALT, false); // enc1 +
    screen_sdl_handle_key(&priv, &down);

    const auto &ev = test_posted_events();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == EVENT_SDL_ENC);
    CHECK(ev[0].enc.n == 1);
    CHECK(ev[0].enc.delta == 4);
}

TEST_CASE("screen_sdl_handle_key: without modifier, grid key passes through to keyboard") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    test_clear_posted_events();

    SDL_KeyboardEvent down = make_key(SDL_SCANCODE_1, true, KMOD_NONE, false);
    screen_sdl_handle_key(&priv, &down);

    const auto &ev = test_posted_events();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == EVENT_SDL_KEY);
    CHECK(ev[0].sdl_key.code == KEY_1);
    CHECK(ev[0].sdl_key.value == 1);
}

TEST_CASE("screen_sdl_handle_key: ordinary key passes through with repeat flag") {
    screen_sdl_priv_t priv;
    screen_sdl_input_defaults(&priv);
    test_clear_posted_events();

    SDL_KeyboardEvent rep = make_key(SDL_SCANCODE_A, true, KMOD_NONE, true);
    screen_sdl_handle_key(&priv, &rep);

    const auto &ev = test_posted_events();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].type == EVENT_SDL_KEY);
    CHECK(ev[0].sdl_key.code == KEY_A);
    CHECK(ev[0].sdl_key.value == 2); // 2 = repeat
}
