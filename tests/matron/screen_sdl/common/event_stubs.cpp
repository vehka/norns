// test doubles for the two event-queue entry points sdl.cc calls. Instead of
// posting to the real matron event queue, record each event so tests can assert
// on what screen_sdl_handle_key produced. Mirrors events.cc: event_data_new
// callocs and stamps the type; event_post takes ownership.

#include <cstdlib>
#include <vector>

#include "event_types.h"
#include "events.h"

static std::vector<union event_data> g_posted;

const std::vector<union event_data> &test_posted_events() { return g_posted; }
void test_clear_posted_events() { g_posted.clear(); }

union event_data *event_data_new(event_t type) {
    union event_data *ev = (union event_data *)calloc(1, sizeof(union event_data));
    ev->type = type;
    return ev;
}

void event_post(union event_data *ev) {
    g_posted.push_back(*ev);
    free(ev);
}
