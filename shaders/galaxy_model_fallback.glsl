#ifndef GALAXY_MODEL_FALLBACK_GLSL
#define GALAXY_MODEL_FALLBACK_GLSL
// Minimal stand-in for galaxy_model.glsl, matching src/stubs/GalaxyModelStub.cpp's packing:
// exponential disk with logarithmic arms, exponential bulge, exponential dust layer.
//   data[0] = centre (kpc, relative to the viewpoint), type
//   data[1] = normal, disk scale length (kpc)     data[2] = major axis, disk scale height (kpc)
//   data[3] = luminosity (L_sun), bulge fraction, bulge radius (kpc), bounding radius (kpc)
//   data[4] = disk colour, arm count              data[5] = bulge colour, arm pitch (rad)
//   data[6] = dust tau per pc at R = h_R, dust scale height (kpc), arm contrast, arm ref radius (kpc)
//   data[7] = arm colour, seed
struct GalaxyGpu { vec4 data[16]; };

vec3 galaxyCenterKpc(in GalaxyGpu g) { return g.data[0].xyz; }
float galaxyBoundRadiusKpc(in GalaxyGpu g) { return g.data[3].w; }

void galaxySample(in GalaxyGpu g, vec3 pKpc, out vec3 emissivity, out float extinction) {
    vec3 d = pKpc - g.data[0].xyz;
    vec3 n = g.data[1].xyz, m = g.data[2].xyz;
    float y = dot(d, n), x = dot(d, m), z = dot(d, cross(n, m));
    float R = sqrt(x * x + z * z);
    float hR = max(g.data[1].w, 1e-3), hz = max(g.data[2].w, 1e-4);
    float L = g.data[3].x, bf = g.data[3].y, rb = max(g.data[3].z, 1e-3);
    // Densities in L_sun / pc^3 (lengths converted from kpc).
    float hRpc = hR * 1000.0, hzpc = hz * 1000.0, rbpc = rb * 1000.0;
    float diskNorm = L * (1.0 - bf) / (4.0 * 3.14159265 * hRpc * hRpc * hzpc);
    float bulgeNorm = L * bf / (8.0 * 3.14159265 * rbpc * rbpc * rbpc);
    float theta = atan(z, x);
    float pitch = max(g.data[5].w, 0.05);
    float arms = g.data[4].w;
    float psi = arms * (theta - log(max(R, 1e-3) / max(g.data[6].w, 1e-3)) / tan(pitch));
    float armMod = mix(1.0, g.data[6].z, pow(0.5 + 0.5 * cos(psi), 4.0)) / (0.5 + 0.5 * g.data[6].z);
    float disk = diskNorm * exp(-R / hR - abs(y) / hz);
    float bulge = bulgeNorm * exp(-length(d) / rb);
    vec3 armCol = g.data[7].rgb;
    emissivity = g.data[4].rgb * disk * armMod * 0.7 + armCol * disk * 0.3 * armMod * armMod + g.data[5].rgb * bulge;
    float hd = max(g.data[6].y, 1e-4);
    extinction = g.data[6].x * exp(1.0 - R / hR - abs(y) / hd) * armMod;
}
vec3 galaxyNormal(in GalaxyGpu g) { return g.data[1].xyz; }
float galaxyThinKpc(in GalaxyGpu g) { return max(g.data[6].y, 1e-4); }
void galaxySampleLod(in GalaxyGpu g, vec3 pKpc, float lodKpc, out vec3 starLight, out vec3 nebula, out float tau) {
    galaxySample(g, pKpc, starLight, tau);
    nebula = vec3(0.0);
}
#endif
