#pragma once
// Cuts between an event director's shots: 0.8 s through black, the view switching at the black frame,
// so a change of viewpoint is unmistakable instead of a camera move (GrbDirector).
#include <algorithm>

namespace bhs {

struct ShotCut {
    int view = 0;          // the shot being shown
    double cut = -1.0;     // seconds into the cut (-1: none)
    double fade = 0.0;     // 0..1: black between the shots

    void reset(int v) {
        view = v;
        cut = -1.0;
        fade = 0.0;
    }
    // Asks for `want`; dt is the real time since the last call.
    void update(int want, double dt) {
        if (want != view && cut < 0.0) cut = 0.0;
        if (cut < 0.0) return;
        cut += dt;
        if (cut >= 0.4 && view != want) view = want;
        fade = cut < 0.4 ? cut / 0.4 : std::max(0.0, 1.0 - (cut - 0.4) / 0.4);
        if (cut >= 0.8) {
            cut = -1.0;
            fade = 0.0;
        }
    }
};

}  // namespace bhs
