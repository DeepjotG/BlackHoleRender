#include <cstdio>
#include <cstdlib>
#include <string>

#include "App.h"

namespace {

void usage() {
    std::puts(
        "blackhole [options]\n"
        "  --width N --height N     window size (default 1600x900)\n"
        "  --fullscreen             borderless fullscreen on the primary monitor\n"
        "  --wayland | --x11        window platform (default: Wayland when available)\n"
        "  --preset low|medium|high|ultra\n"
        "  --bookmark N             start at bookmark 1-4\n"
        "  --pos x,y,z              camera position (M units); --target x,y,z look-at point\n"
        "  --frames N               render N frames then exit (automation)\n"
        "  --screenshot FILE.png    save the last frame (use with --frames)\n"
        "  --benchmark              time every preset and print a table\n"
        "  --lensing 0|1  --disk none|thin|volumetric  --doppler F  --exposure F  --time SEC\n"
        "  --debug steps|footprint  diagnostic views\n"
        "  --seed N                 sky seed\n"
        "  --vsync 0|1  --no-ui  --ui  --no-capture\n"
        "  --shaders DIR            shader directory (default: source tree)\n"
        "  --settings FILE          settings file (default settings.ini)\n");
}

bool parseVec3(const std::string& s, glm::vec3& v) {
    return std::sscanf(s.c_str(), "%f,%f,%f", &v.x, &v.y, &v.z) == 3;
}

}  // namespace

int main(int argc, char** argv) {
    bhs::Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--width") o.width = std::stoi(next());
        else if (a == "--height") o.height = std::stoi(next());
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--wayland") o.wayland = true;
        else if (a == "--x11") o.x11 = true;
        else if (a == "--preset") {
            const std::string p = next();
            if (p == "low" || p == "0") o.preset = bhs::PRESET_LOW;
            else if (p == "medium" || p == "1") o.preset = bhs::PRESET_MEDIUM;
            else if (p == "high" || p == "2") o.preset = bhs::PRESET_HIGH;
            else if (p == "ultra" || p == "3") o.preset = bhs::PRESET_ULTRA;
            else { std::fprintf(stderr, "unknown preset %s\n", p.c_str()); return 2; }
        }
        else if (a == "--bookmark") o.bookmark = std::stoi(next());
        else if (a == "--pos") { o.hasPos = parseVec3(next(), o.pos); if (!o.hasPos) { usage(); return 2; } }
        else if (a == "--target") { o.hasTarget = parseVec3(next(), o.target); if (!o.hasTarget) { usage(); return 2; } }
        else if (a == "--frames") o.frames = std::stoi(next());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--benchmark") o.benchmark = true;
        else if (a == "--lensing") o.lensing = std::stoi(next());
        else if (a == "--disk") {
            const std::string d = next();
            if (d == "none") o.diskMode = bhs::DISK_NONE;
            else if (d == "thin") o.diskMode = bhs::DISK_THIN;
            else if (d == "volumetric" || d == "vol") o.diskMode = bhs::DISK_VOLUMETRIC;
            else { std::fprintf(stderr, "unknown disk mode %s\n", d.c_str()); return 2; }
        }
        else if (a == "--doppler") o.doppler = std::stof(next());
        else if (a == "--exposure") o.exposure = std::stof(next());
        else if (a == "--time") o.startTime = std::stod(next());
        else if (a == "--debug") {
            const std::string d = next();
            o.debugView = (d == "steps") ? 1 : (d == "footprint") ? 2 : 0;
        }
        else if (a == "--seed") o.seed = static_cast<unsigned>(std::stoul(next()));
        else if (a == "--vsync") o.vsync = std::stoi(next());
        else if (a == "--no-ui") o.noUi = true;
        else if (a == "--ui") o.forceUi = true;
        else if (a == "--no-capture") o.noCapture = true;
        else if (a == "--shaders") o.shaderDir = next();
        else if (a == "--settings") o.settingsPath = next();
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    bhs::App app;
    return app.run(o);
}
