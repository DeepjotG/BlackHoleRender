#pragma once
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace bhs {

template <class... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args) {
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stdout, "[bhs] %s\n", s.c_str());
    std::fflush(stdout);
}

template <class... Args>
void logWarn(std::format_string<Args...> fmt, Args&&... args) {
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "[bhs] WARNING: %s\n", s.c_str());
    std::fflush(stderr);
}

template <class... Args>
void logError(std::format_string<Args...> fmt, Args&&... args) {
    std::string s = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "[bhs] ERROR: %s\n", s.c_str());
    std::fflush(stderr);
}

}  // namespace bhs
