// Tests of the supernova model (src/sn/SnModel.h) and its director's timeline.
#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

#include "sn/SnDirector.h"
#include "sn/SnModel.h"

namespace {

int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

void testModel() {
    bhs::sn::Params p;
    bhs::sn::Model m;
    m.init(p);
    const double sPerM = m.secondsPerM(), mPerM = m.metersPerM();
    // A 700 solar-radius star: the shock takes about half a day to climb out; the flash lasts about its
    // light-crossing time (27 minutes).
    std::printf("  star radius %.0f M, shock crossing %.1f h, flash %.0f s\n", m.starRadiusM(), m.crossingSeconds() / 3600.0, m.flashSeconds());
    CHECK(std::abs(m.starRadiusM() * mPerM - 700.0 * 6.957e8) < 1.0);
    CHECK(m.crossingSeconds() > 12.0 * 3600.0 && m.crossingSeconds() < 15.0 * 3600.0);
    CHECK(std::abs(m.flashSeconds() - m.starRadiusM() * sPerM) < 1e-9);
    const double tb = m.breakoutTimeM();
    const bhs::sn::State before = m.at(0.5 * m.collapseTimeM()), climbing = m.at(0.5 * (m.collapseTimeM() + tb));
    CHECK(!before.collapsed && !before.breakout && before.photoTempK == p.starTeffK && before.photoRadiusM == m.starRadiusM());
    CHECK(climbing.collapsed && !climbing.breakout && climbing.photoRadiusM == m.starRadiusM());
    const bhs::sn::State flash = m.at(tb + 1.0 / sPerM), hour = m.at(tb + 3600.0 / sPerM), day = m.at(tb + 86400.0 / sPerM),
                        week = m.at(tb + 7.0 * 86400.0 / sPerM), plateau = m.at(tb + 60.0 * 86400.0 / sPerM),
                        nebular = m.at(tb + 160.0 * 86400.0 / sPerM), year = m.at(tb + 365.25 * 86400.0 / sPerM),
                        late = m.at(tb + 4.0 * 365.25 * 86400.0 / sPerM);
    // The breakout: the hottest, brightest moment; cooling onto the plateau within a day or two.
    CHECK(flash.breakout && std::abs(flash.photoTempK - p.peakTempK) < 50.0 && flash.photoBright > 20.0);
    CHECK(hour.photoTempK < flash.photoTempK && hour.photoTempK > 15000.0);
    CHECK(week.photoTempK == p.plateauTempK && plateau.photoTempK == p.plateauTempK);
    CHECK(plateau.photoBright < 1.01 && plateau.photoOpacity == 1.0);
    // The envelope grows at the ejecta speed: thirtyfold in a month.
    CHECK(std::abs(day.photoRadiusM - (m.starRadiusM() + p.ejectaSpeed * 86400.0 / sPerM)) < 1e-6 * day.photoRadiusM);
    CHECK(plateau.photoRadiusM > 20.0 * m.starRadiusM());
    // The nebular phase: the photosphere gone, filaments lit by cobalt decay, fading over the years, a pulsar.
    CHECK(!plateau.nebular && nebular.nebular && nebular.photoOpacity == 0.0);
    CHECK(nebular.nebulaRadiusM > 0.0 && nebular.shellRadiusM > nebular.nebulaRadiusM && nebular.shellRadiusM == nebular.envelopeRadiusM);
    CHECK(nebular.nebulaBright > year.nebulaBright && year.nebulaBright > late.nebulaBright && late.nebulaBright > 0.05);
    CHECK(plateau.pulsarBright == 0.0 && nebular.pulsarBright > 0.9);
    CHECK(!year.over && late.over);
    CHECK(std::abs(glm::length(flash.site) - p.siteDistanceM) < 1e-6 * p.siteDistanceM);
}

void testDirector() {
    bhs::sn::Params p;
    bhs::sn::Model m;
    m.init(p);
    bhs::SnDirector d;
    d.start(m);
    CHECK(d.active() && d.stage() == bhs::SnDirector::kStar && d.cameraEngaged());
    const double dt = 1.0 / 60.0;
    double timeM = 0.0;
    std::vector<int> stages{d.stage()};
    double stageStart[7] = {};
    for (int frame = 0; frame < 60 * 240; ++frame) {
        const bhs::sn::State s = m.at(timeM);
        bhs::SnDirector::Input in;
        in.dt = dt;
        in.state = &s;
        in.model = &m;
        d.update(in);
        if (d.stage() != stages.back()) {
            stages.push_back(d.stage());
            stageStart[d.stage()] = frame * dt;
        }
        glm::dvec3 vp, vt;
        d.view(vp, vt);
        CHECK(std::isfinite(vp.x) && std::isfinite(vp.y) && std::isfinite(vp.z));
        CHECK(glm::length(vp - s.site) >= 3.4 * m.starRadiusM());   // never inside the star or the fireball
        if (d.finished()) break;
        timeM += d.pace() * dt;
    }
    const std::vector<int> want{bhs::SnDirector::kStar, bhs::SnDirector::kCollapse, bhs::SnDirector::kBreakout, bhs::SnDirector::kExpansion,
                                bhs::SnDirector::kRemnant, bhs::SnDirector::kOver};
    CHECK(stages == want);
    if (stages != want) {
        std::printf("  stages:");
        for (int s : stages) std::printf(" %d", s);
        std::printf("\n");
    }
    std::printf("  collapse at %.1f s, breakout %.1f s, expansion %.1f s, remnant %.1f s, over %.1f s\n", stageStart[bhs::SnDirector::kCollapse],
                stageStart[bhs::SnDirector::kBreakout], stageStart[bhs::SnDirector::kExpansion], stageStart[bhs::SnDirector::kRemnant],
                stageStart[bhs::SnDirector::kOver]);
    // The whole film takes about two minutes; the star is watched for ten seconds, the collapse's climb
    // takes about six.
    CHECK(stageStart[bhs::SnDirector::kCollapse] > 9.0 && stageStart[bhs::SnDirector::kCollapse] < 11.5);
    CHECK(stageStart[bhs::SnDirector::kBreakout] - stageStart[bhs::SnDirector::kCollapse] > 5.0 &&
          stageStart[bhs::SnDirector::kBreakout] - stageStart[bhs::SnDirector::kCollapse] < 9.0);
    CHECK(stageStart[bhs::SnDirector::kOver] > 60.0 && stageStart[bhs::SnDirector::kOver] < 150.0);
    CHECK(d.finished());
    CHECK(d.title()[0] != '\0' && d.caption()[0] != '\0');
}

}  // namespace

int main() {
    testModel();
    testDirector();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sn_tests: all passed\n");
    return 0;
}
