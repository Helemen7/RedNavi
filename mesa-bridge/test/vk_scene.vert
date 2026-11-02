#version 450
// Three draws in one call: instance 0 a checkerboard-textured quad whose depth rises to the right, instance 1 a gradient quad whose depth rises to the left (they cross at x = 0),
// instance 2 a thin slanted triangle on the right edge, for the multisample resolve.
layout(location = 0) out vec2 uv;
layout(location = 1) flat out int inst;
void main() {
    vec2 q[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 t[6] = vec2[6](vec2(0.92, -0.9), vec2(0.98, 0.9), vec2(0.92, 0.9), vec2(0.92, 0.9), vec2(0.92, 0.9), vec2(0.92, 0.9));
    inst = gl_InstanceIndex;
    vec2 p = q[gl_VertexIndex] * 0.9;
    float z = inst == 0 ? 0.5 + 0.4 * (p.x / 0.9) : 0.5 - 0.4 * (p.x / 0.9);
    if (inst == 2) { p = t[gl_VertexIndex]; z = 0.1; }
    uv = p / 0.9 * 0.5 + 0.5;
    gl_Position = vec4(p, z, 1.0);
}
