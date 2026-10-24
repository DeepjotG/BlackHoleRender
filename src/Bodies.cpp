#include "Bodies.h"

#include <algorithm>
#include <cmath>

namespace bhs {

std::vector<BodyGpu> packBodies(const std::vector<BodyDesc>& bodies, const glm::dvec3& camPos,
                                double minAngularRadius, size_t maxBodies) {
    struct Candidate {
        const BodyDesc* b;
        double angular;
    };
    std::vector<Candidate> cands;
    cands.reserve(bodies.size());
    for (const BodyDesc& b : bodies) {
        const double d = glm::length(b.pos - camPos);
        const double extent = std::max(b.radius, b.ringOuter);
        const double ang = d > extent ? std::asin(std::min(1.0, extent / d)) : 1.0;
        // Stars stay visible as glowing discs a little longer than their geometric size suggests.
        const double eff = b.kind == BodyKind::Star ? ang * 4.0 : ang;
        if (eff < minAngularRadius) continue;
        cands.push_back({&b, eff});
    }
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& c) { return a.angular > c.angular; });
    if (cands.size() > maxBodies) cands.resize(maxBodies);

    std::vector<BodyGpu> out;
    out.reserve(cands.size());
    for (const Candidate& c : cands) {
        const BodyDesc& b = *c.b;
        BodyGpu g{};
        g.posRadius = glm::vec4(glm::vec3(b.pos), static_cast<float>(b.radius));
        g.relPos = glm::vec4(glm::vec3(b.pos - camPos), b.kind == BodyKind::Star ? 1.0f : 0.0f);
        g.look = glm::vec4(static_cast<float>(static_cast<int>(b.look)), static_cast<float>(b.seed % 4096u), b.atmosphere, b.rotation);
        g.tint = glm::vec4(b.tint, 0.0f);
        const float ll = glm::length(b.lightDir);
        g.light = glm::vec4(ll > 0.0f ? b.lightDir / ll : glm::vec3(0, 1, 0), b.lightIntensity);
        g.lightColor = glm::vec4(b.lightColor, b.teff);
        const float al = glm::length(b.axis);
        g.axis = glm::vec4(al > 0.0f ? b.axis / al : glm::vec3(0, 1, 0), 0.0f);
        g.ring = glm::vec4(static_cast<float>(b.ringInner), static_cast<float>(b.ringOuter), 0.0f, 0.0f);
        out.push_back(g);
    }
    return out;
}

}  // namespace bhs
