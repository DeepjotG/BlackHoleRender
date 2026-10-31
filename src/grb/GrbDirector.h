#pragma once
// Directs the neutron-star merger like a short film (see TdeDirector for the star disruption): the
// event's own clock runs in slow motion through the inspiral's last orbits (showing the chirp), slower
// still through the merger and the launch of the jets, then as a time-lapse while the jets race away and
// the kilonova swells, holds while the ejecta sweep over the black hole and the camera, then runs through
// the following days from far out as the ejecta cool to red and thin away. Four shots joined by cuts
// through black (riding with the stars; the flash from outside, then a steady pull-back that keeps the
// swelling ball at a constant size; a wide view of the hole and the ejecta; the far pull-back); captions
// for each stage.
#include <glm/glm.hpp>

#include "ShotCut.h"
#include "grb/GrbModel.h"

namespace bhs {

class GrbDirector {
public:
    enum Stage { kIdle, kInspiral, kMerger, kBurst, kAfterglow, kDays, kOver };

    struct Input {
        double dt = 0.0;                     // real seconds since the last update
        const grb::State* state = nullptr;
        const grb::Model* model = nullptr;
        glm::dvec3 cameraPos{0.0};           // where the camera is (to end the event when the ejecta reach it)
    };

    void start(const grb::Model& model);
    void stop() { stage_ = kIdle; }
    void update(const Input& in);
    void engageCamera() { camera_ = stage_ != kIdle; }
    void releaseCamera() { camera_ = false; }

    Stage stage() const { return stage_; }
    bool active() const { return stage_ != kIdle; }
    bool finished() const { return stage_ == kOver && overSeconds_ > 1.2; }   // the app clears the event
    double pace() const { return pace_; }   // M of event time per real second
    bool cameraEngaged() const { return camera_ && stage_ != kIdle; }
    void view(glm::dvec3& pos, glm::dvec3& target) const { pos = viewPos_; target = viewTarget_; }
    float fade() const { return static_cast<float>(cut_.fade); }
    const char* title() const;
    const char* caption() const;

private:
    enum View { vClose, vMedium, vFar, vWide, vPull };
    struct Pose { glm::dvec3 pos{0.0}, target{0.0}; };
    Pose viewPose(View v, const Input& in) const;
    void setStage(Stage s);

    Stage stage_ = kIdle;
    bool camera_ = false;
    double pace_ = 1.0;
    double stageSeconds_ = 0.0, overSeconds_ = 0.0;
    double a0_ = 1.0, period0_ = 1.0, sPerM_ = 1.0, scale_ = 60.0;
    bool standalone_ = false;
    double holdPace_ = 0.0, daysPace0_ = 1.0;
    glm::dvec3 pullDir_{0.0, 1.0, 0.0};   // the far pull-back's direction from the ejecta's centre
    double drift_ = 0.0;
    glm::dvec3 closeDir_{0.0, 1.0, 0.0};   // the close shots' direction from the site (its frame)
    ShotCut cut_;                  // the shot being shown, and the cut in progress
    glm::dvec3 viewPos_{0.0}, viewTarget_{0.0};
};

}  // namespace bhs
