// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Sets the answers RedNaviFB gives to framebuffer attributes, for experiments, through the probe's user client (RN_UC_FB_OVERRIDE; setting a property of the framebuffer from user space
// needs an entitlement). WindowServer asks again when it restarts. usage: rn_fbknob clear | rn_fbknob KIND SELECTOR RESULT VALUE [KIND SELECTOR RESULT VALUE ...]
//   KIND: set, get, setconn, getconn; SELECTOR: the four characters as printed by tools/rn_fbtable.py (for example wsrv); RESULT and VALUE: numbers (RESULT 0 is success, 0xe00002c7 unsupported).
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define RN_UC_FB_OVERRIDE 20
int main(int argc, char **argv) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedNaviProbe"));
    if (!s) { fprintf(stderr, "no RedNaviProbe\n"); return 1; }
    io_connect_t c;
    if (IOServiceOpen(s, mach_task_self(), 0, &c) != KERN_SUCCESS) { fprintf(stderr, "cannot open the probe\n"); return 1; }
    uint8_t buf[24 * 16]; size_t n = 0;
    for (int i = 1; i + 3 < argc && n < 16; i += 4, n++) {
        uint32_t kind = !strcmp(argv[i], "set") ? 1 : !strcmp(argv[i], "get") ? 2 : !strcmp(argv[i], "setconn") ? 3 : 4;
        uint32_t sel = 0; for (int k = 0; k < 4; k++) sel = (sel << 8) | (uint8_t)(strlen(argv[i + 1]) > (size_t)k ? argv[i + 1][k] : ' ');
        uint32_t res = (uint32_t)strtoul(argv[i + 2], NULL, 0), pad = 0; uint64_t val = strtoull(argv[i + 3], NULL, 0);
        memcpy(buf + 24 * n, &kind, 4); memcpy(buf + 24 * n + 4, &sel, 4); memcpy(buf + 24 * n + 8, &res, 4); memcpy(buf + 24 * n + 12, &pad, 4); memcpy(buf + 24 * n + 16, &val, 8);
    }
    kern_return_t kr = IOConnectCallStructMethod(c, RN_UC_FB_OVERRIDE, buf, 24 * n, NULL, NULL);
    printf("%zu override(s): %s (0x%x)\n", n, kr == KERN_SUCCESS ? "set" : "refused", kr);
    return kr != KERN_SUCCESS;
}
