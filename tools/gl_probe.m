// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Helemen7 and RedNavi contributors

// OpenGL probe (docs/OPENGL_PLAN.md): lists the CGL renderers, opens a 3.2 core profile context on each of them (or only on the one whose
// renderer ID is given), compiles a vertex and a fragment shader, draws one triangle and one instanced quad pair into an off-screen
// framebuffer with alpha blending (what a terminal such as Kitty does), reads the pixels back and checks them.
//
//     clang -O1 -fobjc-arc -framework OpenGL -framework CoreFoundation tools/gl_probe.m -Wno-deprecated-declarations -o gl_probe
//     ./gl_probe            all renderers
//     ./gl_probe 0x...      only the renderer with this CGL renderer ID
//
// Exit status 0 when every context that could be created drew the right pixels. A renderer that cannot make a context is reported and
// skipped; it is not a failure of the others.
#import <Foundation/Foundation.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kVS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec4 col;\n"
    "layout(location=2) in vec2 off;\n"
    "out vec4 vcol;\n"
    "void main(){ vcol = col; gl_Position = vec4(pos + off, 0.0, 1.0); }\n";
static const char *kFS =
    "#version 330 core\n"
    "in vec4 vcol;\n"
    "out vec4 frag;\n"
    "void main(){ frag = vcol; }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("  shader compile failed: %s\n", log);
        return 0;
    }
    return s;
}

// Returns 0 when the pixels are right, 1 when not, 2 when the context could not be made.
static int probeOne(CGLRendererInfoObj info, int idx, int W, int H) {
    GLint rid = 0, accel = 0, vram = 0, online = 0;
    CGLDescribeRenderer(info, idx, kCGLRPRendererID, &rid);
    CGLDescribeRenderer(info, idx, kCGLRPAccelerated, &accel);
    CGLDescribeRenderer(info, idx, kCGLRPVideoMemoryMegabytes, &vram);
    CGLDescribeRenderer(info, idx, kCGLRPOnline, &online);
    printf("renderer %d: id 0x%08x accelerated %d online %d vram %d MB\n", idx, rid, accel, online, vram);

    CGLPixelFormatAttribute attrs[] = {kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core, kCGLPFARendererID, (CGLPixelFormatAttribute)rid,
                                       kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = NULL;
    GLint npf = 0;
    CGLError e = CGLChoosePixelFormat(attrs, &pf, &npf);
    if (e || !pf) { printf("  no 3.2 core pixel format: %s\n", CGLErrorString(e)); return 2; }
    CGLContextObj ctx = NULL;
    e = CGLCreateContext(pf, NULL, &ctx);
    CGLDestroyPixelFormat(pf);
    if (e || !ctx) { printf("  context failed: %s\n", CGLErrorString(e)); return 2; }
    CGLSetCurrentContext(ctx);
    printf("  GL_VENDOR   %s\n  GL_RENDERER %s\n  GL_VERSION  %s\n  GLSL        %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
           glGetString(GL_SHADING_LANGUAGE_VERSION));

    int result = 1;
    GLuint fbo = 0, tex = 0, vao = 0, vbo[2] = {0, 0}, prog = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fs != GL_FRAMEBUFFER_COMPLETE) { printf("  framebuffer incomplete: 0x%x\n", fs); goto done; }

    GLuint v = compile(GL_VERTEX_SHADER, kVS), f = compile(GL_FRAGMENT_SHADER, kFS);
    if (!v || !f) goto done;
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { printf("  link failed\n"); goto done; }

    // Quad of two triangles, 0.5 wide, centred on the instance offset; per-vertex colour is red with alpha 0.5, blended over a blue clear.
    // Instance 0 at (-0.5, 0), instance 1 at (0.5, 0).
    float pos[12] = {-0.25f, -0.5f, 0.25f, -0.5f, 0.25f, 0.5f, -0.25f, -0.5f, 0.25f, 0.5f, -0.25f, 0.5f};
    float col[24];
    for (int i = 0; i < 6; i++) { col[i * 4] = 1; col[i * 4 + 1] = 0; col[i * 4 + 2] = 0; col[i * 4 + 3] = 0.5f; }
    float off[4] = {-0.5f, 0.0f, 0.5f, 0.0f};
    GLuint vbos[3];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(3, vbos);
    glBindBuffer(GL_ARRAY_BUFFER, vbos[0]); glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbos[1]); glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbos[2]); glBufferData(GL_ARRAY_BUFFER, sizeof(off), off, GL_STATIC_DRAW);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, 0); glVertexAttribDivisor(2, 1);

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, 2);
    glFinish();

    uint8_t *px = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum ge = glGetError();
    if (ge) printf("  glGetError 0x%x\n", ge);
    // Inside a quad: red 0.5 over blue = (128, 0, 128). Outside: (0, 0, 255). Quad 0 covers x in [0.25, 0.75] of the width, quad 1 [0.75, 1.0] and beyond? No:
    // the quads are 0.5 wide in NDC, centred at -0.5 and 0.5, so they cover NDC x in [-0.75, -0.25] and [0.25, 0.75].
    struct { float x, y; int r, g, b; } want[] = {
        {-0.5f, 0.0f, 128, 0, 128}, {0.5f, 0.0f, 128, 0, 128}, {0.0f, 0.0f, 0, 0, 255}, {-0.9f, 0.0f, 0, 0, 255}, {0.5f, 0.8f, 0, 0, 255}};
    int bad = 0;
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        int x = (int)((want[i].x * 0.5f + 0.5f) * W), y = (int)((want[i].y * 0.5f + 0.5f) * H);
        uint8_t *p = px + ((size_t)y * W + x) * 4;
        int d = abs(p[0] - want[i].r) + abs(p[1] - want[i].g) + abs(p[2] - want[i].b);
        printf("  pixel (%3d,%3d) = %3d %3d %3d, want %3d %3d %3d%s\n", x, y, p[0], p[1], p[2], want[i].r, want[i].g, want[i].b, d > 6 ? "  BAD" : "");
        if (d > 6) bad++;
    }
    free(px);
    result = bad ? 1 : 0;
    printf("  %s\n", result ? "FAIL" : "PASS");
done:
    CGLSetCurrentContext(NULL);
    CGLDestroyContext(ctx);
    return result;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        GLint want = argc > 1 ? (GLint)strtoul(argv[1], NULL, 0) : 0;
        CGLRendererInfoObj info = NULL;
        GLint n = 0;
        CGLError e = CGLQueryRendererInfo(0xffffffff, &info, &n);
        if (e) { printf("CGLQueryRendererInfo: %s\n", CGLErrorString(e)); return 3; }
        printf("%d renderer(s)\n", n);
        int fail = 0, made = 0;
        for (int i = 0; i < n; i++) {
            GLint rid = 0;
            CGLDescribeRenderer(info, i, kCGLRPRendererID, &rid);
            if (want && rid != want) continue;
            int r = probeOne(info, i, 64, 64);
            if (r == 1) fail++;
            if (r != 2) made++;
        }
        CGLDestroyRendererInfo(info);
        printf("%d context(s) made, %d failed\n", made, fail);
        return (fail || !made) ? 1 : 0;
    }
}
