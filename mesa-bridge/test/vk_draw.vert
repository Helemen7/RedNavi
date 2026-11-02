#version 450
void main() {
    // upper-left half of the target: (-1,-1) (1,-1) (-1,1)
    vec2 p[3] = vec2[3](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
