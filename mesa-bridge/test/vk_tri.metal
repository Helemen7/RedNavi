#include <metal_stdlib>
using namespace metal;

// The first render pipeline of the Metal plug-in: positions and colours from a buffer indexed by vertex_id (no stage_in), the colour interpolated to the fragment stage.
struct VOut { float4 position [[position]]; float4 color; };

vertex VOut tri_vertex(uint vid [[vertex_id]], constant float4 *pos [[buffer(0)]], constant float4 *col [[buffer(1)]])
{
    VOut o;
    o.position = pos[vid];
    o.color = col[vid];
    return o;
}

fragment float4 tri_fragment(VOut in [[stage_in]], constant float4 &tint [[buffer(0)]])
{
    return in.color * tint;
}
