#version 450
// A uniform buffer, rewritten every frame, tints the colour.
layout(set = 0, binding = 0) uniform U { vec4 tint; } u;
layout(location = 0) in vec3 vcol;
layout(location = 0) out vec4 color;
void main() { color = vec4(vcol * u.tint.rgb, 1.0); }
