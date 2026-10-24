#pragma once
#include <string>

namespace bhs::capture {

// Reads the current draw framebuffer (RGBA8) and writes a PNG. Returns true on success.
bool savePngFromFramebuffer(const std::string& path, int width, int height);

// Builds a timestamped path like "<dir>/<prefix>_YYYYmmdd_HHMMSS.png" and creates the directory.
std::string timestampedPath(const std::string& dir, const std::string& prefix);

}  // namespace bhs::capture
