#ifndef GAS_HASH_GLSL
#define GAS_HASH_GLSL
// Particle counts per cell on a ladder of cell sizes uCell0 * 2^level (gas_count.comp fills them,
// gas_moments.comp and gas_splat.comp read them): the local density of the particles at every scale.
// Each particle's own scale is the finest cell holding enough particles (gasLevel); there the second
// moments of the particles' positions (gas_moments.comp) give the shape of the gas around it, so that
// thin streams are drawn as thin ribbons. Both are hash tables with linear probing; a key is never 0
// (0 marks an empty slot).
#include "common.glsl"

const int HASH_LEVELS = 15;
const uint HASH_SIZE = 1u << 22;   // the fine levels hold one key per particle: ~2.5M keys at worst
const uint HASH_MASK = HASH_SIZE - 1u;
const int HASH_PROBES = 24;
const uint NEIGHBOURS = 12u;          // particles a cell must hold before its statistics are trusted
layout(std430, binding = 11) buffer HashKeys { uint hKey[]; };
layout(std430, binding = 12) buffer HashCounts { uint hCnt[]; };
uniform float uCell0;   // finest cell (M)

// Moments: count, sum of offsets from the cell centre (3), sum of their products (xx yy zz xy xz yz), in
// cell units, fixed point.
const uint MOM_SIZE = 1u << 19;
const uint MOM_MASK = MOM_SIZE - 1u;
const int MOM_WORDS = 10;
const float MOM_SCALE = 1048576.0;
layout(std430, binding = 9) buffer MomKeys { uint mKey[]; };
layout(std430, binding = 10) buffer MomVals { int mVal[]; };

float gasCellSize(int level) { return uCell0 * exp2(float(level)); }

uvec2 hashCell(vec3 p, int level) {
    ivec3 c = ivec3(floor(p / gasCellSize(level)));
    uvec3 h = pcg3d(uvec3(c) ^ uvec3(uint(level) * 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u));
    return uvec2(max(h.x, 1u), h.y);   // key, first slot (before masking)
}

void hashAddKey(uint key, uint firstSlot, uint n) {
    uint slot = firstSlot & HASH_MASK;
    for (int i = 0; i < HASH_PROBES; ++i) {
        uint prev = atomicCompSwap(hKey[slot], 0u, key);
        if (prev == 0u || prev == key) {
            atomicAdd(hCnt[slot], n);
            return;
        }
        slot = (slot + 1u) & HASH_MASK;
    }
}
void hashAdd(vec3 p, int level) {
    uvec2 ks = hashCell(p, level);
    hashAddKey(ks.x, ks.y, 1u);
}

uint hashCount(vec3 p, int level) {
    uvec2 ks = hashCell(p, level);
    uint slot = ks.y & HASH_MASK;
    for (int i = 0; i < HASH_PROBES; ++i) {
        uint k = hKey[slot];
        if (k == ks.x) return hCnt[slot];
        if (k == 0u) return 0u;
        slot = (slot + 1u) & HASH_MASK;
    }
    return 0u;
}

// The finest level whose cell around p holds NEIGHBOURS particles (the coarsest if none does).
int gasLevel(vec3 p, out uint count) {
    for (int lv = 0; lv < HASH_LEVELS; ++lv) {
        count = hashCount(p, lv);
        if (count >= NEIGHBOURS) return lv;
    }
    return HASH_LEVELS - 1;
}

// Moment slot of p's cell at a level: found, or claimed when `claim` (-1: none).
int momSlot(vec3 p, int level, bool claim) {
    uvec2 ks = hashCell(p, level);
    uint slot = (ks.y >> 7) & MOM_MASK;
    for (int i = 0; i < HASH_PROBES; ++i) {
        uint k = claim ? atomicCompSwap(mKey[slot], 0u, ks.x) : mKey[slot];
        if (k == ks.x || (claim && k == 0u)) return int(slot);
        if (k == 0u) return -1;
        slot = (slot + 1u) & MOM_MASK;
    }
    return -1;
}

// Offset of p from the centre of its cell at a level, in cell units (-0.5 .. 0.5).
vec3 cellOffset(vec3 p, int level) {
    float c = gasCellSize(level);
    return p / c - floor(p / c) - 0.5;
}

// Where a particle of the disruption is (frozen ones are stored relative to their remnant).
vec3 gasParticlePosition(vec4 pos, vec3 frozenCentre) {
    return int(pos.w) == 4 ? frozenCentre + pos.xyz : pos.xyz;
}
bool gasParticleDrawn(vec4 pos) {
    int s = int(pos.w);
    return s == 0 || s == 4;
}
#endif
