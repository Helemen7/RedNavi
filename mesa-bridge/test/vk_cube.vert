#version 450
// Vertex and index buffers in, a model-view-projection matrix in a push constant.
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location = 0) out vec3 vcol;
void main() {
    gl_Position = pc.mvp * vec4(pos, 1.0);
    vcol = col;
}
