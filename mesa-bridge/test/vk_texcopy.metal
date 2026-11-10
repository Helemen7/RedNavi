// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include <metal_stdlib>
using namespace metal;

// Reads a texel from src and writes it, plus one in the red channel, to dst: the first texture kernel for the Metal plug-in.
kernel void texture_copy(texture2d<float, access::read> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float4 c = src.read(gid);
    c.r = min(c.r + 1.0 / 255.0, 1.0);
    dst.write(c, gid);
}

// The same through a sampler (nearest, unnormalized coordinates).
constexpr sampler nearest_s(coord::pixel, filter::nearest, address::clamp_to_edge);
kernel void texture_sample(texture2d<float> src [[texture(0)]],
                           texture2d<float, access::write> dst [[texture(1)]],
                           uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    dst.write(src.sample(nearest_s, float2(gid) + 0.5), gid);
}
