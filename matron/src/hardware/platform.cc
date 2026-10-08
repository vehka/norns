#include <stdlib.h>
#include <unistd.h>

#include "platform.h"
#include <stdio.h>
#include <string.h>

static platform_t p = PLATFORM_UNKNOWN;

void init_platform() {
    if (access("/sys/firmware/devicetree/base/model", F_OK) != -1) {

        // the node can exist but be unreadable (android)
        FILE *fptr = fopen("/sys/firmware/devicetree/base/model", "r");
        if (fptr == NULL) {
            return;
        }
        char modelString[100] = "";
        if (fgets(modelString, 100, fptr) == NULL) {
            modelString[0] = '\0';
        }
        fclose(fptr);

        if (strstr(modelString, "Compute Module 3")) {
            p = PLATFORM_CM3;
        } else if (strstr(modelString, "Compute Module 4S")) {
            p = PLATFORM_CM4S;
        } else if (strstr(modelString, "Compute Module 4")) {
            p = PLATFORM_CM4;
        } else if (strstr(modelString, "Raspberry Pi 3")) {
            p = PLATFORM_PI3;
        } else if (strstr(modelString, "Raspberry Pi 4")) {
            p = PLATFORM_PI4;
        } else {
            p = PLATFORM_OTHER;
        }
    }
}

platform_t platform() {
    return p;
}

const char *platform_name() {
    switch (platform()) {
    case PLATFORM_CM3:
        return "cm3";
    case PLATFORM_CM4:
        return "cm4";
    case PLATFORM_CM4S:
        return "cm4s";
    case PLATFORM_PI3:
        return "pi3";
    case PLATFORM_PI4:
        return "pi4";
    case PLATFORM_OTHER:
        return "other";
    default:
        break;
    }

    return "unknown";
}

bool platform_factory() {
    switch (platform()) {
    case PLATFORM_CM3:
    case PLATFORM_CM4:
    case PLATFORM_CM4S:
        return true;
    default:
        break;
    }
    return false;
}

bool platform_shield() {
    return !platform_factory();
}