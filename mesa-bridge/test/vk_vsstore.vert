#version 450
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

layout(std430, binding = 0) buffer Out { uint v[]; } o;
void main() {
    o.v[gl_VertexIndex] = 1000u + uint(gl_VertexIndex) * 3u;
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
