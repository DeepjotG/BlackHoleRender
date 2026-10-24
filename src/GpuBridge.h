#pragma once
#include <glad/gl.h>

#include <string>

struct GLFWwindow;

namespace bhs {

// Renders on the NVIDIA GPU through a headless EGL device context and shows the result in a
// window whose OpenGL context lives on the GPU that drives the display (the iGPU on hybrid
// laptops). Used under Wayland, where NVIDIA's native Wayland EGL path cannot offload and
// XWayland may lack DRI3, which leaves GLX offload stuck on a slow software copy.
//
// Each frame: render into backbuffer() on the NVIDIA context, then present() reads it back
// asynchronously into a pixel buffer, uploads the previous frame into a texture on the display
// context, draws it and swaps. Costs ~2.5 ms per 1080p frame and adds one frame of latency.
class GpuBridge {
public:
    // The window's context must be current. On success the NVIDIA context is left current.
    bool init(GLFWwindow* window, bool debugContext);
    void shutdown();
    bool active() const { return nvContext_ != nullptr; }

    void makeRenderCurrent();
    void makeDisplayCurrent();

    // Framebuffer object on the render context that stands in for the window's default
    // framebuffer (RGBA8, resized on demand). Render context must be current.
    GLuint backbuffer(int width, int height);
    // Render context must be current; it is current again on return.
    void present();
    void setSwapInterval(int interval);

    const std::string& renderer() const { return renderer_; }
    const std::string& error() const { return error_; }

private:
    void releaseBuffers();
    void displaySetup();

    GLFWwindow* window_ = nullptr;
    void* nvDisplay_ = nullptr;   // EGLDisplay
    void* nvContext_ = nullptr;   // EGLContext
    std::string renderer_, error_;

    // render-context objects
    GLuint fbo_ = 0, color_ = 0;
    GLuint pbo_[2] = {0, 0};
    GLsync fence_[2] = {nullptr, nullptr};
    int pboW_[2] = {0, 0}, pboH_[2] = {0, 0};
    int w_ = 0, h_ = 0;
    unsigned frame_ = 0;

    // display-context objects
    GLuint dispProg_ = 0, dispVao_ = 0, dispTex_ = 0;
    int dispTexW_ = 0, dispTexH_ = 0;
};

}  // namespace bhs
