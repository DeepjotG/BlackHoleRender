#pragma once
// BHS_PROFILE=1: where each frame's time goes. CPU sections are timed with the wall clock, GPU sections
// with timestamp queries read back four frames later (measuring never stalls the pipeline). Every 60
// frames one line is logged with each section's mean and worst time in milliseconds.
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "Log.h"

namespace bhs {

class Profiler {
public:
    static Profiler& get() {
        static Profiler p;
        return p;
    }
    bool on() const { return on_; }

    // Call once per frame, before any section: collects the GPU results of four frames ago.
    void frameBegin() {
        if (!on_) return;
        const double now = glfwGetTime();
        if (frameStart_ > 0.0) add(cpu_, "frame", (now - frameStart_) * 1000.0);
        frameStart_ = now;
        cur_ = (cur_ + 1) % kRing;
        for (const GpuPair& g : gpuFrames_[cur_]) {
            GLuint64 t0 = 0, t1 = 0;
            GLint ready = 0;
            glGetQueryObjectiv(g.q1, GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                glGetQueryObjectui64v(g.q0, GL_QUERY_RESULT, &t0);
                glGetQueryObjectui64v(g.q1, GL_QUERY_RESULT, &t1);
                add(gpu_, g.name, static_cast<double>(t1 - t0) * 1e-6);
            }
            free_.push_back(g.q0);
            free_.push_back(g.q1);
        }
        gpuFrames_[cur_].clear();
        if (++frames_ % 60 == 0) report();
    }
    void cpu(const char* name, double ms) {
        if (on_) add(cpu_, name, ms);
    }
    GLuint gpuStamp() {
        GLuint q;
        if (free_.empty()) {
            glGenQueries(1, &q);
        } else {
            q = free_.back();
            free_.pop_back();
        }
        glQueryCounter(q, GL_TIMESTAMP);
        return q;
    }
    void gpuSection(const char* name, GLuint q0, GLuint q1) { gpuFrames_[cur_].push_back({name, q0, q1}); }

    // A CPU section (and, with gpu = true, the GPU work issued inside it) for the scope's lifetime.
    struct Scope {
        Scope(const char* name, bool gpu = false) : name_(name) {
            Profiler& p = Profiler::get();
            if (!p.on()) return;
            t0_ = glfwGetTime();
            if (gpu) q0_ = p.gpuStamp();
        }
        ~Scope() {
            Profiler& p = Profiler::get();
            if (!p.on()) return;
            p.cpu(name_, (glfwGetTime() - t0_) * 1000.0);
            if (q0_) p.gpuSection(name_, q0_, p.gpuStamp());
        }
        const char* name_;
        double t0_ = 0.0;
        GLuint q0_ = 0;
    };

private:
    Profiler() : on_(std::getenv("BHS_PROFILE") != nullptr) {}
    struct Stat {
        std::string name;
        double sum = 0.0, worst = 0.0;
        int n = 0;
    };
    struct GpuPair {
        const char* name;
        GLuint q0, q1;
    };
    static void add(std::vector<Stat>& v, const char* name, double ms) {
        auto it = std::find_if(v.begin(), v.end(), [&](const Stat& s) { return s.name == name; });
        if (it == v.end()) {
            v.push_back({name});
            it = v.end() - 1;
        }
        it->sum += ms;
        it->worst = std::max(it->worst, ms);
        ++it->n;
    }
    void report() {
        std::string line = "profile (ms, mean/worst of 60 frames) cpu:";
        auto print = [&](std::vector<Stat>& v) {
            for (Stat& s : v) {
                if (s.n == 0) continue;
                char buf[96];
                std::snprintf(buf, sizeof buf, " %s %.2f/%.2f", s.name.c_str(), s.sum / s.n, s.worst);
                line += buf;
                s.sum = s.worst = 0.0;
                s.n = 0;
            }
        };
        print(cpu_);
        line += " | gpu:";
        print(gpu_);
        logInfo("{}", line);
    }

    static constexpr int kRing = 4;
    bool on_;
    double frameStart_ = 0.0;
    long frames_ = 0;
    int cur_ = 0;
    std::array<std::vector<GpuPair>, kRing> gpuFrames_;
    std::vector<GLuint> free_;
    std::vector<Stat> cpu_, gpu_;
};

}  // namespace bhs
