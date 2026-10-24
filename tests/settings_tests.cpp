// Round-trip and preset checks for Settings.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "Settings.h"

using bhs::Settings;

static int failures = 0;
#define CHECK(c, msg) do { if (c) std::printf("  ok    %s\n", msg); else { ++failures; std::printf("  FAIL  %s\n", msg); } } while (0)

int main() {
    Settings a;
    a.applyPreset(bhs::PRESET_ULTRA);
    a.exposure = 2.5f;
    a.lensing = false;
    a.skySeed = 424242u;
    a.diskPeakTemp = 7777.0f;
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/bhs_settings_test.ini";
    CHECK(a.save(path), "save settings file");
    Settings b;
    CHECK(b.load(path), "load settings file");
    CHECK(b.fingerprint() == a.fingerprint(), "fingerprint survives save/load round trip");
    CHECK(b.preset == bhs::PRESET_ULTRA && b.skyResolution == 4096 && b.maxSteps == 700, "ultra preset fields restored");
    CHECK(!b.lensing && b.skySeed == 424242u && b.diskPeakTemp == 7777.0f, "individual fields restored");
    Settings c;
    c.applyPreset(bhs::PRESET_LOW);
    CHECK(c.diskMode == bhs::DISK_THIN && c.renderScale == 0.5f, "low preset uses the thin disk at half resolution");
    CHECK(c.fingerprint() != b.fingerprint(), "fingerprints differ between different settings");
    std::remove(path.c_str());
    std::printf("%s\n", failures ? "FAILED" : "all settings checks passed");
    return failures ? 1 : 0;
}
