// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// Reader for the container format of .metallib files (what the Metal compiler tools write and -[MTLDevice newLibraryWithData:error:] takes). The layout below was read off
// the metallib files that ship in macOS (e.g. /System/Library/ScreenCaptureKitMetal/.../default.metallib), not from a specification, so it is checked against every function
// it finds: the bitcode must start with LLVM's magic. Header (little endian): "MTLB", then at 0x10 the file size, at 0x18 offset and at 0x20 size of the function list, at 0x28/0x30 public
// metadata, at 0x38/0x40 private metadata, at 0x48/0x50 the bitcode section. The function list is a u32 count and that many entries (the size at 0x20 counts the bytes after the count), each a u32 size (counted from the size
// field itself) and tags: 4 characters, u16 length, data; "ENDT" ends the entry with no length. Tags used: NAME (C string), TYPE (u8: 0 vertex, 1 fragment, 2 kernel, checked against the function names of the system libraries),
// OFFT (three u64: offsets into the public metadata, private metadata and bitcode sections), MDSZ (u64: size of the function's bitcode).
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[128];
    uint8_t type;                    // 0 vertex, 1 fragment, 2 kernel (MTLFunctionType is this plus 1)
    uint64_t bitcode_off;            // from the start of the file
    uint64_t bitcode_size;
} rn_mtl_func;

// 0 on success; count receives the number of functions found (at most max are stored).
int rn_metallib_parse(const uint8_t *d, size_t n, rn_mtl_func *out, unsigned max, unsigned *count);

#ifdef __cplusplus
}
#endif
