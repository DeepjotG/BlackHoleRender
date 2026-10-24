#include "GpuBridge.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>
#include <vector>

#include "Log.h"

namespace bhs {

namespace {

const char* kBlitVs =
    "#version 330 core\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    uv = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
const char* kBlitFs =
    "#version 330 core\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D frame;\n"
    "void main() { color = vec4(texture(frame, uv).rgb, 1.0); }\n";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        logError("bridge blit shader: {}", log);
    }
    return s;
}

bool hasToken(const char* list, const char* token) {
    if (!list) return false;
    const size_t n = std::strlen(token);
    for (const char* p = list; (p = std::strstr(p, token)) != nullptr; p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    }
    return false;
}

}  // namespace

bool GpuBridge::init(GLFWwindow* window, bool debugContext) {
    window_ = window;
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto queryDeviceString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!queryDevices || !queryDeviceString || !getPlatformDisplay) {
        error_ = "EGL device enumeration is not available";
        return false;
    }
    EGLDeviceEXT devices[16];
    EGLint count = 0;
    if (!queryDevices(16, devices, &count) || count <= 0) {
        error_ = "no EGL devices found";
        return false;
    }
    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (EGLint i = 0; i < count && dpy == EGL_NO_DISPLAY; ++i) {
        // NVIDIA devices advertise EGL_NV_device_cuda; skipping the others avoids probing Mesa
        // devices (which prints confusing warnings about the NVIDIA card).
        const char* ext = queryDeviceString(devices[i], EGL_EXTENSIONS);
        if (!hasToken(ext, "EGL_NV_device_cuda")) continue;
        EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
        EGLint major = 0, minor = 0;
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) continue;
        const char* vendor = eglQueryString(d, EGL_VENDOR);
        if (vendor && std::strstr(vendor, "NVIDIA")) dpy = d;
        else eglTerminate(d);
    }
    if (dpy == EGL_NO_DISPLAY) {
        error_ = "no NVIDIA EGL device";
        return false;
    }
    if (!hasToken(eglQueryString(dpy, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context")) {
        error_ = "the NVIDIA EGL device lacks EGL_KHR_surfaceless_context";
        eglTerminate(dpy);
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        error_ = "eglBindAPI(OpenGL) failed";
        eglTerminate(dpy);
        return false;
    }
    const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1) {
        error_ = "no suitable NVIDIA EGL config";
        eglTerminate(dpy);
        return false;
    }
    std::vector<EGLint> ctxAttr = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 6,
                                   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT};
    if (debugContext) {
        ctxAttr.push_back(EGL_CONTEXT_OPENGL_DEBUG);
        ctxAttr.push_back(EGL_TRUE);
    }
    ctxAttr.push_back(EGL_NONE);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr.data());
    if (ctx == EGL_NO_CONTEXT) {
        error_ = "could not create an OpenGL 4.6 core context on the NVIDIA GPU";
        eglTerminate(dpy);
        return false;
    }
    nvDisplay_ = dpy;
    nvContext_ = ctx;

    // Display-side objects first (window context is current), then switch to the NVIDIA one.
    displaySetup();
    makeRenderCurrent();
    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    renderer_ = r ? r : "NVIDIA GPU";
    glGenBuffers(2, pbo_);
    return true;
}

void GpuBridge::displaySetup() {
    GLuint vs = compile(GL_VERTEX_SHADER, kBlitVs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kBlitFs);
    dispProg_ = glCreateProgram();
    glAttachShader(dispProg_, vs);
    glAttachShader(dispProg_, fs);
    glLinkProgram(dispProg_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGenVertexArrays(1, &dispVao_);
    glGenTextures(1, &dispTex_);
}

void GpuBridge::makeRenderCurrent() {
    if (!nvContext_) return;
    eglMakeCurrent(static_cast<EGLDisplay>(nvDisplay_), EGL_NO_SURFACE, EGL_NO_SURFACE, static_cast<EGLContext>(nvContext_));
}

void GpuBridge::makeDisplayCurrent() { glfwMakeContextCurrent(window_); }

void GpuBridge::setSwapInterval(int interval) {
    if (!active()) {
        glfwSwapInterval(interval);
        return;
    }
    makeDisplayCurrent();
    glfwSwapInterval(interval);
    makeRenderCurrent();
}

void GpuBridge::releaseBuffers() {
    for (int i = 0; i < 2; ++i) {
        if (fence_[i]) glDeleteSync(fence_[i]);
        fence_[i] = nullptr;
        pboW_[i] = pboH_[i] = 0;
    }
}

GLuint GpuBridge::backbuffer(int width, int height) {
    if (fbo_ && width == w_ && height == h_) return fbo_;
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    w_ = width;
    h_ = height;
    glGenTextures(1, &color_);
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w_, h_);
    glClearTexImage(color_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logError("bridge backbuffer incomplete ({}x{})", w_, h_);
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(w_) * h_ * 4;
    releaseBuffers();
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return fbo_;
}

void GpuBridge::present() {
    if (!active() || !fbo_) {
        glfwSwapBuffers(window_);
        return;
    }
    // 1. Start the asynchronous readback of this frame.
    const int cur = static_cast<int>(frame_ & 1u), prev = cur ^ 1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[cur]);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (fence_[cur]) glDeleteSync(fence_[cur]);
    fence_[cur] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    pboW_[cur] = w_;
    pboH_[cur] = h_;
    glFlush();

    // 2. Map the previous frame (normally finished long ago).
    const void* pixels = nullptr;
    const int pw = pboW_[prev], ph = pboH_[prev];
    if (fence_[prev] && pw > 0 && ph > 0) {
        glClientWaitSync(fence_[prev], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(fence_[prev]);
        fence_[prev] = nullptr;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[prev]);
        pixels = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(pw) * ph * 4, GL_MAP_READ_BIT);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // 3. Upload and draw it on the display context, then swap.
    makeDisplayCurrent();
    glBindTexture(GL_TEXTURE_2D, dispTex_);
    if (pixels) {
        if (pw != dispTexW_ || ph != dispTexH_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            dispTexW_ = pw;
            dispTexH_ = ph;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fbw, fbh);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    if (dispTexW_ > 0) {
        glUseProgram(dispProg_);
        glUniform1i(glGetUniformLocation(dispProg_, "frame"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dispTex_);
        glBindVertexArray(dispVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glfwSwapBuffers(window_);

    // 4. Back to the render context and release the mapping.
    makeRenderCurrent();
    if (pixels) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[prev]);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    ++frame_;
}

void GpuBridge::shutdown() {
    if (!active()) return;
    makeRenderCurrent();
    releaseBuffers();
    if (pbo_[0]) glDeleteBuffers(2, pbo_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    pbo_[0] = pbo_[1] = fbo_ = color_ = 0;
    makeDisplayCurrent();
    if (dispProg_) glDeleteProgram(dispProg_);
    if (dispVao_) glDeleteVertexArrays(1, &dispVao_);
    if (dispTex_) glDeleteTextures(1, &dispTex_);
    dispProg_ = dispVao_ = dispTex_ = 0;
    auto dpy = static_cast<EGLDisplay>(nvDisplay_);
    eglDestroyContext(dpy, static_cast<EGLContext>(nvContext_));
    eglTerminate(dpy);
    nvContext_ = nullptr;
    nvDisplay_ = nullptr;
}

}  // namespace bhs
