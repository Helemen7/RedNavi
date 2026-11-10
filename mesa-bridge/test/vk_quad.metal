// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include <metal_stdlib>
using namespace metal;

// A vertex function with [[stage_in]] (interleaved position, texture coordinate and colour from one vertex buffer, set up by an MTLVertexDescriptor) and a fragment function that
// samples a texture with a runtime sampler and multiplies the vertex colour: the shape of a 2D compositor's draw.
struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
struct VOut { float4 position [[position]]; float2 uv; float4 color; };

vertex VOut quad_vertex(VIn in [[stage_in]])
{
    VOut o;
    o.position = float4(in.pos, 0.0, 1.0);
    o.uv = in.uv;
    o.color = in.color;
    return o;
}

fragment float4 quad_fragment(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    return tex.sample(smp, in.uv) * in.color;
}
