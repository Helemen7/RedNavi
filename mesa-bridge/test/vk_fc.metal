// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

#include <metal_stdlib>
using namespace metal;

// Function constants: an int, two bools and a float that may be left undefined, and a texture argument that exists only when a constant says so.
constant int mul [[function_constant(0)]];
constant bool use_add [[function_constant(1)]];
constant float scale [[function_constant(2)]];
constant float offset_c [[function_constant(3)]];
constant bool use_tex [[function_constant(4)]];
constant bool has_bias = is_function_constant_defined(offset_c);

kernel void fc_kernel(device float *out [[buffer(0)]], texture2d<float, access::read> tex [[texture(0), function_constant(use_tex)]], uint i [[thread_position_in_grid]])
{
    float v = float(i) * float(mul);
    if (use_add) v += scale;
    if (has_bias) v += offset_c;
    if (use_tex) v += tex.read(uint2(0, 0)).x;
    out[i] = v;
}
