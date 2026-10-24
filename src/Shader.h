#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bhs {

// A GLSL program loaded from files in the shader directory. Supports
//   #include "file.glsl"     (resolved relative to the shader directory, included once)
//   compile-time defines injected after #version
// and hot reload when any involved file changes on disk.
class ShaderProgram {
public:
    static void setShaderDir(const std::string& dir) { shaderDir_ = dir; }
    static const std::string& shaderDir() { return shaderDir_; }

    ShaderProgram() = default;
    ~ShaderProgram();
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    bool loadGraphics(const std::string& vertFile, const std::string& fragFile);
    bool loadCompute(const std::string& compFile);
    void setDefines(const std::vector<std::string>& defines);  // triggers rebuild on next reload()
    bool reload();                 // rebuild from disk; keeps old program on failure
    bool reloadIfChanged();        // rebuild only if a source file changed; returns true if rebuilt
    void destroy();                // release the GL program (call while the context is current)
    bool valid() const { return program_ != 0; }
    GLuint id() const { return program_; }
    const std::string& log() const { return log_; }
    const std::string& name() const { return name_; }

    void use() const;
    GLint uniform(const std::string& name);

    void set(const std::string& n, int v);
    void set(const std::string& n, unsigned v);
    void set(const std::string& n, float v);
    void set(const std::string& n, const glm::vec2& v);
    void set(const std::string& n, const glm::vec3& v);
    void set(const std::string& n, const glm::vec4& v);
    void set(const std::string& n, const glm::ivec2& v);
    void set(const std::string& n, const glm::mat3& v);
    void set(const std::string& n, const glm::mat4& v);

private:
    struct Source {
        std::string path;
        std::filesystem::file_time_type mtime;
    };
    bool build();
    std::string preprocess(const std::filesystem::path& path, std::set<std::string>& visited,
                           std::vector<std::string>& fileList, bool& ok, bool isRoot);
    GLuint compileStage(GLenum type, const std::string& file, std::vector<std::string>& fileList, bool& ok);
    std::string decorateLog(const std::string& glLog, const std::vector<std::string>& fileList) const;

    static std::string shaderDir_;
    std::string vertFile_, fragFile_, compFile_;
    std::vector<std::string> defines_;
    std::vector<Source> sources_;
    GLuint program_ = 0;
    std::string log_;
    std::string name_;
    std::unordered_map<std::string, GLint> uniformCache_;
};

}  // namespace bhs
