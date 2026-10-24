#pragma once
// The 3D universe around the black hole: owns the UniverseContent (real or invented), the
// procedural star field, the GPU star list binned by direction, the diffuse sky cubemap baked from
// the viewpoint, and converts between the galactic frame (meters) and the renderer frame (M).
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Bodies.h"
#include "Shader.h"
#include "universe/StarField.h"
#include "universe/Universe.h"

namespace bhs {

struct Settings;

struct UniverseTarget {
    std::string name;
    glm::dvec3 pos{0.0};      // renderer frame (M)
    double radius = 0.0;      // M, sets the viewing distance
    int kind = 0;             // 0 star, 1 galaxy, 2 place, 3 planet
    std::string info;
    float absMag = 0.0f;      // stars: absolute visual magnitude
    glm::dvec3 axis{0.0};     // galaxies: disk normal (renderer frame), so visits arrive at a flattering angle
};

class UniverseScene {
public:
    static constexpr int kTileGrid = 128;      // star tiles per cube-face edge
    static constexpr int kBakeChunkRows = 256; // rows of one cube face baked per frame

    bool init();
    void shutdown();

    // Builds the content for a mode (1 invented, 2 real). Returns false (and an error) if the
    // content is unavailable; mode 0 (backdrop) clears it.
    bool configure(int mode, uint32_t seed, int placement, const std::string& assetDir, std::string* error);
    bool active() const { return active_; }
    int mode() const { return mode_; }
    const universe::UniverseContent& content() const { return content_; }

    // Per frame: maps the camera into the galaxy, refreshes the stars (CPU gather, GPU projection
    // and binning), advances the progressive sky bake, and appends nearby stars and solar-system
    // planets to `bodies`.
    void update(const glm::dvec3& camPosM, double simSeconds, const Settings& s, std::vector<BodyDesc>& bodies);
    // After the camera moved this frame: projects the stars for the final camera position and
    // advances the progressive sky bake.
    void prepareRender(const glm::dvec3& camPosM);

    glm::dvec3 toRenderer(const glm::dvec3& galacticMeters) const;
    glm::dvec3 toGalactic(const glm::dvec3& rendererM) const;
    glm::dvec3 dirToRenderer(const glm::dvec3& galacticDir) const;

    // GPU resources for the tracer.
    GLuint diffuseCube() const { return cube_; }
    GLuint starBuffer() const { return sortedBuf_; }
    GLuint tileBuffer() const { return offsetBuf_; }
    float starFluxScale() const { return fluxScale_; }

    const std::vector<UniverseTarget>& targets() const { return targets_; }
    double nearestSurfaceM() const { return nearestSurfaceM_; }   // to stars / galaxy edges, M
    // Exposure for the diffuse sky: 1 normally, lower inside very bright places (a galaxy's centre), so
    // the sky keeps its structure and the black hole stays the brightest thing in view.
    float skyExposure() const { return static_cast<float>(skyExposure_); }
    double julianDate(double simSeconds) const { return jd0_ + simSeconds / 86400.0; }
    std::string nearestName() const { return nearestName_; }
    // Nearest star other than the black hole (renderer frame), for the "Nearest star" button.
    bool nearestStar(glm::dvec3& pos, double& radius, std::string& name) const {
        if (nearestStarRadius_ <= 0.0) return false;
        pos = nearestStarPos_;
        radius = nearestStarRadius_;
        name = nearestStarName_;
        return true;
    }
    size_t starCount() const { return starCount_; }
    double lastGatherMs() const { return gatherMs_; }
    const std::string& status() const { return status_; }

private:
    void rebuildMapping(double bhMassSolar);
    void uploadStars(const glm::dvec3& originGal);
    void projectStars(const glm::dvec3& camGal, float limitingMag);
    void bakeChunk(int step);   // step 0 .. 6 * chunks - 1
    void buildGalaxyLists(const glm::dvec3& camGal);
    void buildTargets(double simSeconds);
    void appendPlanetarySystem(const universe::StarRecord& st, const std::string& starName, double simSeconds,
                               std::vector<BodyDesc>& bodies);

    bool active_ = false;
    int mode_ = 0;
    universe::UniverseContent content_;
    universe::StarField field_;
    uint32_t seed_ = 0;

    // galactic <-> renderer
    glm::dmat3 R_{1.0};          // galactic -> renderer rotation
    double metersPerM_ = 1.0;
    double massSolar_ = 0.0;

    // stars
    std::vector<universe::StarRecord> stars_;   // catalogue + procedural, galactic frame
    glm::dvec3 starOrigin_{0.0};                 // upload origin (galactic, meters)
    glm::dvec3 lastProjectCam_{1e300};
    size_t starCount_ = 0;
    double gatherMs_ = 0.0;
    GLuint starBuf_ = 0, sortedBuf_ = 0, countBuf_ = 0, offsetBuf_ = 0;
    size_t starCapacity_ = 0;
    ShaderProgram countProg_, scanProg_, scatterProg_;
    float fluxScale_ = 1.0f;
    double nearestSurfaceM_ = 1e300;
    glm::dvec3 nearestStarPos_{0.0};
    double nearestStarRadius_ = 0.0;
    std::string nearestStarName_;
    std::string nearestName_;

    // diffuse sky
    GLuint cube_ = 0;
    ShaderProgram bakeProg_;
    GLuint nearGalBuf_ = 0, spriteBuf_ = 0, spriteTileBuf_ = 0, spriteIndexBuf_ = 0;
    int nearGalCount_ = 0;
    glm::dvec3 bakeCam_{1e300};
    int bakeRes_ = 512;          // diffuse cubemap face size (BHS_BAKE_RES overrides)
    int bakeStep_ = 1 << 30;     // next chunk to bake; >= bakeSteps() = idle
    int bakeSteps() const { return 6 * std::max(1, bakeRes_ / kBakeChunkRows); }
    bool bakeOk_ = false;
    bool bakeDirty_ = true;
    float deepSky_ = 12.0f;      // long-exposure boost for distant galaxies (Settings::deepSky)
    bool targetsBuilt_ = false;
    bool holeShown_ = false;     // the black hole is in the star list as a point of light
    bool starsChanged_ = true;   // star set changed since the last upload
    bool remapped_ = false;      // mass (mapping) changed since the last prepareRender
    std::vector<universe::StarRecord> proc_;   // procedural stars from the last gather

    std::vector<UniverseTarget> targets_;
    // Distances to the nearest galaxy surface / centre (galactic frame, m), refreshed after the camera
    // moves 1% of them: a scan of every galaxy is too slow to repeat each frame.
    void refreshGalaxyDistances(const glm::dvec3& camGal);
    glm::dvec3 galDistAt_{1e300};
    std::vector<size_t> nearIdx_;         // candidates for the nearest stars (indices into stars_)
    glm::dvec3 nearAt_{1e300};
    double nearReach_ = 0.0;              // distance to the eighth nearest at the last full scan
    double galSurfaceM_ = 1e300, galCentreM_ = 1e300;
    std::string status_;
    double jd0_ = 0.0;           // Julian date when the simulation started (real universe)
    double skyMean_ = 0.0;       // mean luminance of the last complete bake
    double skyExposure_ = 1.0, skyExposureTarget_ = 1.0;
};

}  // namespace bhs
