#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 1) flat in int inst;
layout(location = 0) out vec4 color;
void main() {
    if (inst == 0) color = texture(tex, uv * 8.0);
    else if (inst == 1) color = vec4(uv.x, uv.y, 0.25, 1.0);
    else color = vec4(1.0);
}
