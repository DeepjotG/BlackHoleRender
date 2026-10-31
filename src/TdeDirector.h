#pragma once
#include <glm/glm.hpp>

#include <algorithm>

namespace bhs {

struct GasParticleStats;

// Directs a star disruption like a short film. Its stages run on wildly different time scales (the star
// is torn apart in hours, its debris falls back over weeks), so the director sets the pace of the event
// clock for each stage: slow motion while the star is torn, a time-lapse while the stream is away (it
// predicts the return from the particles' orbits and slows down in time), slow again when the gas
// comes back. Unless the user flies the camera, it also films the event from a handful of fixed shots
// joined by cuts (a short fade to black) rather than camera moves, so each change of view is
// unmistakable; it decides how far the debris disk has formed, and captions what is happening.
class TdeDirector {
public:
    enum Stage { kIdle, kApproach, kTear, kStream, kReturn, kDisk, kSwallowed };

    struct Input {
        double dt = 0.0;               // real seconds since the last update
        double timeM = 0.0;            // event clock (M)
        bool active = false, intact = true, released = false, swallowed = false;
        double pericenterM = 0.0, starRadiusM = 0.0;
        glm::dvec3 pos{0.0}, vel{0.0}; // the star, after release its densest debris (M, M per M)
        double accretedFraction = 0.0;
        int particles = 0;             // particles the star was made of
        const GasParticleStats* gas = nullptr;   // particle statistics (may be invalid)
    };

    // A new event: the star at pos with velocity vel, on its way to pericentre.
    void start(const glm::dvec3& pos, const glm::dvec3& vel);
    void stop() { stage_ = kIdle; }
    void update(const Input& in);
    void engageCamera() { camera_ = stage_ != kIdle; }
    void releaseCamera() { camera_ = false; }

    Stage stage() const { return stage_; }
    bool active() const { return stage_ != kIdle; }
    double pace() const { return pace_; }          // M of event time per real second
    bool cameraEngaged() const { return camera_ && stage_ != kIdle; }
    // The director's viewpoint this frame.
    void view(glm::dvec3& pos, glm::dvec3& target) const { pos = viewPos_; target = viewTarget_; }
    float fade() const { return static_cast<float>(fade_); }   // 0..1: black between two shots
    float diskPresence() const { return static_cast<float>(disk_ * align_); }   // 0..1: how far the debris disk has formed
    double ringRadiusM() const { return 2.0 * rp_; }    // where the returning gas circularises; the disk spreads inward from it
    double nearRadiusM() const { return 4.0 * rp_; }    // gas this close counts as having come back
    double farRadiusM() const { return 12.0 * rp_; }    // gas this far out predicts its return
    double returnFraction() const { return 0.004; }     // of the star back at pericentre when the time-lapse ends (the
                                                         // first arrivals; the inflow then goes on for far longer)
    const char* title() const;
    const char* caption() const;

private:
    // The shots: riding with the star through its fall and tearing; riding behind the departing stream; a
    // vantage over the hole for the return and the disk (vTear, a vantage over the pericentre, is unused:
    // a Sun-like star is a speck next to the hole from any such distance).
    enum View { vRide, vTear, vStream, vHole };
    struct Pose { glm::dvec3 pos{0.0}, target{0.0}; };
    Pose viewPose(View v, const Input& in) const;
    Pose ridePose(const Input& in) const;
    Pose holePose(double dist, double elevDeg, double azDeg, const glm::dvec3& target) const;
    void setStage(Stage s);

    Stage stage_ = kIdle;
    bool camera_ = false;
    double rp_ = 10.0;
    double align_ = 1.0;           // 1 when the orbit lies in the accretion disk's plane (the debris disk is drawn there)
    glm::dvec3 n_{0.0, 1.0, 0.0}, p_{1.0, 0.0, 0.0}, q_{0.0, 0.0, 1.0};   // orbit normal, pericentre, motion there
    double tPeri_ = -1.0;          // event time of the pericentre passage
    double tFirst_ = -1.0;         // predicted event time of the first return
    double pace_ = 1.0;
    double stageSeconds_ = 0.0;    // real time in the current stage
    double waitSeconds_ = 0.0;     // real time past the predicted return with no gas back yet
    double returnSeconds_ = 0.0;   // real time since the gas started coming back
    double diskSeconds_ = 0.0;     // real time in the disk stage
    double accretedAtStream_ = 0.0;
    double disk_ = 0.0;
    double drift_ = 0.0;           // slow camera orbit (deg)
    // The shot being shown, and the cut in progress (-1: none; the view switches at the black frame).
    View view_ = vRide;
    double cut_ = -1.0;
    double fade_ = 0.0;
    bool earlyReturnView_ = false;
    glm::dvec3 viewPos_{0.0}, viewTarget_{0.0};
    // Riding along with the star (smoothed: it swings round the hole quickly).
    glm::dvec3 rideDir_{0.0, 1.0, 0.0};
    double rideDist_ = 1.0, rideLook_ = 0.15;
    bool rideInit_ = false;
    // Smoothed length of the stream (from the particle box; its centre is kept for the return cut).
    glm::dvec3 streamCentre_{0.0};
    double streamLen_ = 0.0;
};

}  // namespace bhs
