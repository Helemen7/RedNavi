// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// The same kernel as vk_dispatch.comp in Metal Shading Language: v[i] = i * 3 + add. Build the library (needs the Metal Toolchain: xcodebuild -downloadComponent MetalToolchain):
//   xcrun -sdk macosx metal -c vk_dispatch.metal -o /tmp/vk_dispatch.air && xcrun -sdk macosx metallib /tmp/vk_dispatch.air -o /tmp/vk_dispatch.metallib
// and run tools/mtl_init_harness with RN_AIR_TEST=/tmp/vk_dispatch.metallib RN_AIR_FUNC=dispatch_add (RN_METAL_COMPUTE=1 RN_COMPUTE_TEST=1 as for the SPIR-V test).
#include <metal_stdlib>
using namespace metal;

kernel void dispatch_add(device uint *out [[buffer(0)]], constant uint &add [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    out[i] = i * 3u + add;
}
