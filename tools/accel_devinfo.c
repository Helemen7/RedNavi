// Calls the device client (type 5) of the Intel accelerator on this Mac the way Metal does (selector 9 with "Metal", selector 2 with a 600-byte output) and dumps the reply.
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
int main(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOAccelerator"));
    if (!s) { puts("no accelerator"); return 1; }
    io_name_t cls; IOObjectGetClass(s, cls); printf("accelerator class %s\n", cls);
    io_connect_t c;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 5, &c);
    printf("open type 5: 0x%x\n", kr);
    if (kr) return 1;
    char name[16] = "Metal";
    size_t outSize = 0;
    kr = IOConnectCallStructMethod(c, 9, name, 16, NULL, &outSize);
    printf("selector 9: 0x%x\n", kr);
    unsigned char out[600]; outSize = 600; memset(out, 0xAA, sizeof(out));
    kr = IOConnectCallStructMethod(c, 2, NULL, 0, out, &outSize);
    printf("selector 2: 0x%x, %zu bytes\n", kr, outSize);
    for (int i = 0; i < 600; i += 16) {
        printf("%03x: ", i);
        for (int j = 0; j < 16; j++) printf("%02x ", out[i + j]);
        printf(" ");
        for (int j = 0; j < 16; j++) putchar(isprint(out[i + j]) ? out[i + j] : '.');
        putchar('\n');
    }
    // Memory the device client offers: IOConnectMapMemory64 for types 0 to 7, address, size and the first words (what IOAccelDeviceCreate maps in user space)
    for (uint32_t t = 0; t < 8; t++) {
        mach_vm_address_t addr = 0; mach_vm_size_t size = 0;
        kr = IOConnectMapMemory64(c, t, mach_task_self(), &addr, &size, kIOMapAnywhere);
        printf("map memory type %u: 0x%x", t, kr);
        if (!kr) { printf(" addr 0x%llx size 0x%llx first words", (unsigned long long)addr, (unsigned long long)size); for (int i = 0; i < 4 && (size_t)i * 4 < size; i++) printf(" %08x", ((uint32_t *)addr)[i]); }
        putchar('\n');
    }
    return 0;
}
