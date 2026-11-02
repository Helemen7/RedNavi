// Loads the plug-in bundle in this process and sends it the messages Metal sends, to see that it does not crash before it is installed anywhere. usage: bundle_test BUNDLE
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        NSBundle *b = [NSBundle bundleWithPath:[NSString stringWithUTF8String:argv[1]]];
        NSError *e = nil;
        printf("load: %d\n", [b loadAndReturnError:&e]);
        Class k = b.principalClass;
        printf("principal class: %s\n", k ? class_getName(k) : "nil");
        NSBundle *answer = [k performSelector:NSSelectorFromString(@"bundleForClass")];
        printf("bundleForClass: %s\n", answer.bundlePath.UTF8String);
        printf("[NSBundle bundleForClass:]: %s\n", [NSBundle bundleForClass:k].bundlePath.UTF8String);
    }
    return 0;
}
