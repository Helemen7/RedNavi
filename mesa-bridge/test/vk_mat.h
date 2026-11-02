/* Small column-major 4x4 matrix helpers and a millisecond clock for the cube tests (Vulkan clip space: y down, depth 0 to 1). */
#ifndef VK_MAT_H
#define VK_MAT_H
#include <math.h>
#include <string.h>
#include <time.h>

typedef float mat4[16];                                              /* column-major: m[column * 4 + row] */
static inline void m_mul(mat4 r, const mat4 a, const mat4 b)
{
   mat4 t;
   for (int c = 0; c < 4; c++) for (int rw = 0; rw < 4; rw++) { float v = 0; for (int k = 0; k < 4; k++) v += a[k * 4 + rw] * b[c * 4 + k]; t[c * 4 + rw] = v; }
   memcpy(r, t, sizeof(t));
}
static inline void m_id(mat4 m) { memset(m, 0, sizeof(mat4)); m[0] = m[5] = m[10] = m[15] = 1; }
static inline void m_rot_y(mat4 m, float a) { m_id(m); m[0] = cosf(a); m[8] = sinf(a); m[2] = -sinf(a); m[10] = cosf(a); }
static inline void m_rot_x(mat4 m, float a) { m_id(m); m[5] = cosf(a); m[9] = -sinf(a); m[6] = sinf(a); m[10] = cosf(a); }
static inline void m_trans(mat4 m, float x, float y, float z) { m_id(m); m[12] = x; m[13] = y; m[14] = z; }
static inline void m_scale(mat4 m, float s) { m_id(m); m[0] = m[5] = m[10] = s; }
static inline void m_persp(mat4 m, float fovy, float aspect, float n, float f)    /* Vulkan clip space: y down, depth 0 to 1 */
{
   const float t = 1.0f / tanf(fovy / 2);
   memset(m, 0, sizeof(mat4));
   m[0] = t / aspect; m[5] = -t; m[10] = f / (n - f); m[11] = -1; m[14] = n * f / (n - f);
}
static inline double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }

#endif
