#include "Capture.h"

#include <glad/gl.h>
#include <stb_image_write.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <vector>

#include "Log.h"

namespace bhs::capture {

bool savePngFromFramebuffer(const std::string& path, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // Flip vertically (GL origin is bottom-left) and force opaque alpha.
    std::vector<unsigned char> flipped(pixels.size());
    const size_t row = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = pixels.data() + static_cast<size_t>(height - 1 - y) * row;
        unsigned char* dst = flipped.data() + static_cast<size_t>(y) * row;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 0];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const int ok = stbi_write_png(path.c_str(), width, height, 4, flipped.data(), static_cast<int>(row));
    if (ok) logInfo("saved {} ({}x{})", path, width, height);
    else logError("failed to write {}", path);
    return ok != 0;
}

std::string timestampedPath(const std::string& dir, const std::string& prefix) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tmv);
    return dir + "/" + prefix + "_" + buf + ".png";
}

}  // namespace bhs::capture
