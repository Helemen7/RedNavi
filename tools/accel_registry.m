// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Read-only: the registry entry IDs involved in how CoreDisplay finds the Metal device of a display: every Metal device's registryID, the IOAccelerator services, the IOFramebuffer
// services, their parents and the properties that mention an accelerator. (docs/M6_WINDOWSERVER_STUDY.md)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>

static void print_entry(io_registry_entry_t e, const char *indent) {
    uint64_t id = 0; IORegistryEntryGetRegistryEntryID(e, &id);
    io_name_t name, cls; IORegistryEntryGetName(e, name); IOObjectGetClass(e, cls);
    printf("%s%s (%s) id 0x%llx\n", indent, name, cls, id);
}
static void print_props(io_registry_entry_t e, const char *indent) {
    CFMutableDictionaryRef p = NULL;
    if (IORegistryEntryCreateCFProperties(e, &p, kCFAllocatorDefault, 0) != KERN_SUCCESS || !p) return;
    NSDictionary *d = (__bridge_transfer NSDictionary *)p;
    for (NSString *k in [d.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        if ([k rangeOfString:@"Accel" options:NSCaseInsensitiveSearch].location == NSNotFound && ![k hasPrefix:@"IOFB"] && ![k hasPrefix:@"IOGraphics"] && ![k hasPrefix:@"MetalPlugin"] && ![k isEqualToString:@"IOName"]) continue;
        NSString *v = [[d[k] description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
        if (v.length > 100) v = [[v substringToIndex:100] stringByAppendingString:@"..."];
        printf("%s    %s = %s\n", indent, k.UTF8String, v.UTF8String);
    }
}
int main(void) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        printf("%lu Metal device(s)\n", (unsigned long)devs.count);
        for (id<MTLDevice> d in devs) printf("  %s: registryID 0x%llx, low power %d, headless %d, removable %d\n", d.name.UTF8String, d.registryID, d.isLowPower, d.isHeadless, d.isRemovable);
        const char *classes[] = {"IOAccelerator", "IOFramebuffer"};
        for (int c = 0; c < 2; c++) {
            printf("\n%s services:\n", classes[c]);
            io_iterator_t it;
            if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(classes[c]), &it) != KERN_SUCCESS) continue;
            io_registry_entry_t e;
            while ((e = IOIteratorNext(it))) {
                print_entry(e, "  ");
                print_props(e, "  ");
                io_registry_entry_t parent;
                if (IORegistryEntryGetParentEntry(e, kIOServicePlane, &parent) == KERN_SUCCESS) { print_entry(parent, "    parent: "); IOObjectRelease(parent); }
                IOObjectRelease(e);
            }
            IOObjectRelease(it);
        }
    }
    return 0;
}
