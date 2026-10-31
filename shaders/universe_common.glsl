#ifndef UNIVERSE_COMMON_GLSL
#define UNIVERSE_COMMON_GLSL
// Cube-face tiling of directions, using OpenGL's cubemap face conventions so tiles line up with
// cubemap texels: face 0..5 = +X, -X, +Y, -Y, +Z, -Z and uv in [-1, 1].

void cubeFaceUV(vec3 d, out int face, out vec2 uv) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        face = d.x > 0.0 ? 0 : 1;
        uv = (d.x > 0.0 ? vec2(-d.z, -d.y) : vec2(d.z, -d.y)) / a.x;
    } else if (a.y >= a.z) {
        face = d.y > 0.0 ? 2 : 3;
        uv = (d.y > 0.0 ? vec2(d.x, d.z) : vec2(d.x, -d.z)) / a.y;
    } else {
        face = d.z > 0.0 ? 4 : 5;
        uv = (d.z > 0.0 ? vec2(d.x, -d.y) : vec2(-d.x, -d.y)) / a.z;
    }
}

// Inverse of cubeFaceUV (not normalised). uv may lie outside [-1, 1] (extended face plane).
vec3 cubeFaceDir(int face, vec2 uv) {
    if (face == 0) return vec3(1.0, -uv.y, -uv.x);
    if (face == 1) return vec3(-1.0, -uv.y, uv.x);
    if (face == 2) return vec3(uv.x, 1.0, uv.y);
    if (face == 3) return vec3(uv.x, -1.0, -uv.y);
    if (face == 4) return vec3(uv.x, -uv.y, 1.0);
    return vec3(-uv.x, -uv.y, -1.0);
}

int cubeTile(vec3 d, int T) {
    int f;
    vec2 uv;
    cubeFaceUV(d, f, uv);
    ivec2 ij = clamp(ivec2((uv * 0.5 + 0.5) * float(T)), ivec2(0), ivec2(T - 1));
    return (f * T + ij.y) * T + ij.x;
}
#endif
