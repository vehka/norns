// test doubles for the virtual device constructors sdl_monome.cc uses, so the
// unit does not need libmonome or the device list.

#include <cstdlib>
#include <cstring>

#include "device/device_monome.h"

static uint32_t next_id = 0;

static struct dev_monome *fake(device_monome_type_t type, int cols, int rows, void (*refresh)(struct dev_monome *)) {
    struct dev_monome *md = (struct dev_monome *)calloc(1, sizeof(struct dev_monome));
    md->type = type;
    md->dev.id = next_id++;
    md->cols = cols;
    md->rows = rows;
    md->virtual_refresh = refresh;
    return md;
}

struct dev_monome *dev_monome_new_virtual_grid(int cols, int rows, const char *, const char *,
                                               void (*refresh)(struct dev_monome *md)) {
    if (cols < 1 || cols > DEV_MONOME_VIRTUAL_MAX || rows < 1 || rows > DEV_MONOME_VIRTUAL_MAX) {
        return NULL;
    }
    return fake(DEVICE_MONOME_TYPE_GRID, cols, rows, refresh);
}

struct dev_monome *dev_monome_new_virtual_arc(const char *, const char *, void (*refresh)(struct dev_monome *md)) {
    return fake(DEVICE_MONOME_TYPE_ARC, 0, 0, refresh);
}

// row-major from data[0], enough for the tests (the real one unpacks quads)
void dev_monome_virtual_grid_levels(struct dev_monome *md, uint8_t *out) {
    memcpy(out, md->data, (size_t)md->cols * md->rows);
}
