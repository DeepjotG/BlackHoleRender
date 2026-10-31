#include "Units.h"

#include <cmath>
#include <cstdio>

namespace bhs::units {

namespace {
std::string fmt(double v, const char* unit) {
    char buf[64];
    const double a = std::fabs(v);
    if (a != 0.0 && (a < 0.01 || a >= 1e6)) std::snprintf(buf, sizeof buf, "%.2e %s", v, unit);
    else if (a < 10.0) std::snprintf(buf, sizeof buf, "%.2f %s", v, unit);
    else if (a < 100.0) std::snprintf(buf, sizeof buf, "%.1f %s", v, unit);
    else std::snprintf(buf, sizeof buf, "%.0f %s", v, unit);
    return buf;
}
}  // namespace

std::string formatDistance(double m) {
    const double a = std::fabs(m);
    if (a < 1e3) return fmt(m, "m");
    if (a < 1e9) return fmt(m / 1e3, "km");
    if (a < 0.05 * kLightYear) return fmt(m / kAU, "AU");
    if (a < 1e6 * kLightYear) return fmt(m / kLightYear, "ly");
    if (a < 1e9 * kLightYear) return fmt(m / (1e6 * kLightYear), "million ly");
    return fmt(m / (1e9 * kLightYear), "billion ly");
}

std::string formatDuration(double s) {
    const double a = std::fabs(s);
    if (a < 120.0) return fmt(s, "s");
    if (a < 2.0 * 3600.0) return fmt(s / 60.0, "min");
    if (a < 2.0 * kDay) return fmt(s / 3600.0, "h");
    if (a < 2.0 * kYear) return fmt(s / kDay, "days");
    if (a < 1e6 * kYear) return fmt(s / kYear, "years");
    if (a < 1e9 * kYear) return fmt(s / (1e6 * kYear), "million years");
    return fmt(s / (1e9 * kYear), "billion years");
}

std::string formatSpeed(double v) {
    const double a = std::fabs(v);
    if (a < 1e3) return fmt(v, "m/s");
    if (a < 0.01 * kC) return fmt(v / 1e3, "km/s");
    if (a < 0.01 * kLightYear) return fmt(v / kC, "c");
    if (a < 1e6 * kLightYear) return fmt(v / kLightYear, "ly/s");
    return fmt(v / (1e6 * kLightYear), "million ly/s");
}

std::string formatMassSolar(double ms) {
    char buf[64];
    if (ms >= 1e9) std::snprintf(buf, sizeof buf, "%.2f billion suns", ms / 1e9);
    else if (ms >= 1e6) std::snprintf(buf, sizeof buf, "%.2f million suns", ms / 1e6);
    else if (ms >= 1e3) std::snprintf(buf, sizeof buf, "%.0f thousand suns", ms / 1e3);
    else std::snprintf(buf, sizeof buf, "%.1f suns", ms);
    return buf;
}

std::string formatFactor(double x) {
    char buf[64];
    if (x < 100.0) std::snprintf(buf, sizeof buf, "%.3gx", x);
    else if (x < 1e7) std::snprintf(buf, sizeof buf, "%.0fx", x);
    else std::snprintf(buf, sizeof buf, "%.2ex", x);
    return buf;
}

}  // namespace bhs::units
