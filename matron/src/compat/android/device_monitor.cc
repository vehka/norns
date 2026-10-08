// Android replacement for device/device_monitor.cc.
//
// There is no udev on Android and an unrooted app cannot open /dev/input,
// /dev/snd or USB serial nodes, so there is nothing to scan or watch.
// Devices that don't come from udev (OSC, network MIDI) are unaffected.

#include <stdio.h>

#include "device_monitor.h"

void dev_monitor_init(void) {
    fprintf(stderr, "dev_monitor: no udev on android, hardware device scanning disabled\n");
}

void dev_monitor_deinit(void) {
}

void dev_monitor_scan(void) {
}
