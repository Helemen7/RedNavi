// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Asks Metal for its devices, the way any Metal program does, so that the plug-in stub is loaded and logs. Run on the machine with the accelerator kext.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
int main(void) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        printf("%lu Metal devices\n", (unsigned long)all.count);
        for (id<MTLDevice> d in all) printf("  %s (registryID 0x%llx, lowPower %d, removable %d, headless %d)\n", d.name.UTF8String, d.registryID, d.isLowPower, d.isRemovable, d.isHeadless);
        id<MTLDevice> def = MTLCreateSystemDefaultDevice();
        printf("default device: %s\n", def ? def.name.UTF8String : "none");
    }
    return 0;
}
