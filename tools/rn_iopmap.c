// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Prints whether IOPresentment has a map (the table CoreDisplay's InitializeDisplayPipes builds from the display pipes it finds). NULL: no pipe was found.
// Build: clang -o rn_iopmap tools/rn_iopmap.c -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/IOPresentment.framework/IOPresentment", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    CFTypeRef (*getMap)(void) = (CFTypeRef (*)(void))dlsym(h, "IOPresentmentGetMap");
    if (!getMap) { printf("IOPresentmentGetMap not found\n"); return 1; }
    CFTypeRef m = getMap();
    if (!m) { printf("MAP NULL\n"); return 0; }
    CFStringRef d = CFCopyDescription(m);
    char buf[4096];
    CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
    printf("map present (type id %lu):\n%s\n", CFGetTypeID(m), buf);
    return 0;
}
