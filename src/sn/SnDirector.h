#pragma once
// Directs the supernova like a short film (see TdeDirector, GrbDirector): real time while the dying
// star is shown, a time-lapse through the shock's half-day climb, the breakout flash and the first hour
// slowed, a time-lapse through weeks of expansion, then years of the fading remnant. One continuous
// shot: a slow orbit of the star that pulls back as the fireball swells to fill the frame, holds it
// there while the envelope turns transparent and shows the remnant inside, and eases back at the end to
// show the remnant whole.
#include <glm/glm.hpp>

#include "sn/SnModel.h"

namespace bhs {

class SnDirector {
public:
    enum Stage { kIdle, kStar, kCollapse, kBreakout, kExpansion, kRemnant, kOver };

    struct Input {
        double dt = 0.0;
        const sn::State* state = nullptr;
        const sn::Model* model = nullptr;
    };

    void start(const sn::Model& model);
    void stop() { stage_ = kIdle; }
    void update(const Input& in);
    void engageCamera() { camera_ = stage_ != kIdle; }
    void releaseCamera() { camera_ = false; }

    Stage stage() const { return stage_; }
    bool active() const { return stage_ != kIdle; }
    bool finished() const { return stage_ == kOver && overSeconds_ > 1.2; }
    double pace() const { return pace_; }   // M of event time per real second
    bool cameraEngaged() const { return camera_ && stage_ != kIdle; }
    void view(glm::dvec3& pos, glm::dvec3& target) const { pos = viewPos_; target = viewTarget_; }
    float fade() const { return static_cast<float>(fade_); }   // 0..1: to black at the end
    const char* title() const;
    const char* caption() const;

private:
    void setStage(Stage s);

    Stage stage_ = kIdle;
    bool camera_ = false;
    double pace_ = 1.0;
    double stageSeconds_ = 0.0, overSeconds_ = 0.0, remnantSeconds_ = 0.0;
    double orbit_ = 0.0;   // the camera's azimuth round the star (degrees)
    double R_ = 1.0, sPerM_ = 1.0, crossing_ = 1.0;
    double fade_ = 0.0;
    glm::dvec3 viewPos_{0.0}, viewTarget_{0.0};
};

}  // namespace bhs
