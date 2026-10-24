#ifndef BLACKBODY_GLSL
#define BLACKBODY_GLSL
uniform sampler1D uBlackbodyLut;
const float BB_TMIN = 1000.0;
const float BB_TMAX = 40000.0;
// rgb: linear sRGB colour of a blackbody normalised to luminance 1
// a:   visible luminance relative to a 6500 K blackbody
vec4 blackbody(float T) {
    float Tc = clamp(T, BB_TMIN, BB_TMAX);
    float t = (log(Tc) - log(BB_TMIN)) / (log(BB_TMAX) - log(BB_TMIN));
    vec4 bb = texture(uBlackbodyLut, t);
    // Below the table the visible output collapses quickly (Wien tail).
    float under = clamp(T / BB_TMIN, 0.0, 1.0);
    bb.a *= under * under * under * under * under * under;
    return bb;
}
#endif
