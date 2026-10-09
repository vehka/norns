#pragma once

#include "device.h"
#include <lualib.h>

extern void dev_list_init(void);
extern void dev_list_add(device_t type, const char *node, const char *name, lua_State *l);
extern void dev_list_remove(device_t type, const char *node);
// an id for a device that is created outside the list
extern uint32_t dev_list_new_id(void);
