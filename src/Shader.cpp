#include "Shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>

#include "Log.h"

namespace fs = std::filesystem;

namespace bhs {

std::string ShaderProgram::shaderDir_ = "shaders";

ShaderProgram::~ShaderProgram() = default;  // GL objects are released via destroy()

void ShaderProgram::destroy() {
    if (program_) glDeleteProgram(program_);
    program_ = 0;
    uniformCache_.clear();
}

bool ShaderProgram::loadGraphics(const std::string& vertFile, const std::string& fragFile) {
    vertFile_ = vertFile;
    fragFile_ = fragFile;
    compFile_.clear();
    name_ = fragFile;
    return build();
}

bool ShaderProgram::loadCompute(const std::string& compFile) {
    compFile_ = compFile;
    vertFile_.clear();
    fragFile_.clear();
    name_ = compFile;
    return build();
}

void ShaderProgram::setDefines(const std::vector<std::string>& defines) { defines_ = defines; }

bool ShaderProgram::reload() { return build(); }

bool ShaderProgram::reloadIfChanged() {
    bool changed = false;
    for (const auto& s : sources_) {
        std::error_code ec;
        auto mt = fs::last_write_time(s.path, ec);
        if (!ec && mt != s.mtime) {
            changed = true;
            break;
        }
    }
    if (!changed) return false;
    return build();
}

std::string ShaderProgram::preprocess(const fs::path& path, std::set<std::string>& visited,
                                      std::vector<std::string>& fileList, bool& ok, bool isRoot) {
    std::error_code ec;
    std::string canon = fs::weakly_canonical(path, ec).string();
    if (ec) canon = path.string();
    if (visited.count(canon)) return "";
    visited.insert(canon);

    std::ifstream in(path);
    if (!in) {
        ok = false;
        log_ += "cannot open shader file: " + canon + "\n";
        return "";
    }
    auto mt = fs::last_write_time(path, ec);
    sources_.push_back({canon, mt});
    const int idx = static_cast<int>(fileList.size());
    fileList.push_back(canon);

    std::stringstream out;
    if (!isRoot) out << "#line 1 " << idx << "\n";
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos && line.compare(s, 8, "#include") == 0) {
            const size_t q1 = line.find('"', s);
            const size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                ok = false;
                log_ += canon + ":" + std::to_string(lineNo) + ": malformed #include\n";
                continue;
            }
            const std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
            out << preprocess(fs::path(shaderDir_) / inc, visited, fileList, ok, false);
            out << "#line " << (lineNo + 1) << " " << idx << "\n";
        } else if (s != std::string::npos && line.compare(s, 8, "#version") == 0) {
            out << line << "\n";
            for (const auto& d : defines_) out << "#define " << d << "\n";
            out << "#line " << (lineNo + 1) << " " << idx << "\n";
        } else {
            out << line << "\n";
        }
    }
    return out.str();
}

std::string ShaderProgram::decorateLog(const std::string& glLog, const std::vector<std::string>& fileList) const {
    std::string s = "--- " + name_ + " ---\n" + glLog;
    s += "source string index -> file:\n";
    for (size_t i = 0; i < fileList.size(); ++i) s += "  " + std::to_string(i) + ": " + fileList[i] + "\n";
    return s;
}

GLuint ShaderProgram::compileStage(GLenum type, const std::string& file, std::vector<std::string>& fileList, bool& ok) {
    std::set<std::string> visited;
    fileList.clear();
    const std::string src = preprocess(fs::path(shaderDir_) / file, visited, fileList, ok, true);
    if (!ok) return 0;
    GLuint sh = glCreateShader(type);
    const char* csrc = src.c_str();
    glShaderSource(sh, 1, &csrc, nullptr);
    glCompileShader(sh);
    GLint status = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string info(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(sh, len, nullptr, info.data());
        log_ += decorateLog(info, fileList);
        glDeleteShader(sh);
        ok = false;
        return 0;
    }
    return sh;
}

bool ShaderProgram::build() {
    log_.clear();
    sources_.clear();
    bool ok = true;
    std::vector<GLuint> stages;
    std::vector<std::string> fileList;

    if (!compFile_.empty()) {
        GLuint c = compileStage(GL_COMPUTE_SHADER, compFile_, fileList, ok);
        if (c) stages.push_back(c);
    } else {
        GLuint v = compileStage(GL_VERTEX_SHADER, vertFile_, fileList, ok);
        if (v) stages.push_back(v);
        GLuint f = compileStage(GL_FRAGMENT_SHADER, fragFile_, fileList, ok);
        if (f) stages.push_back(f);
    }
    if (!ok) {
        for (GLuint s : stages) glDeleteShader(s);
        logError("shader build failed:\n{}", log_);
        return false;
    }
    GLuint prog = glCreateProgram();
    for (GLuint s : stages) glAttachShader(prog, s);
    glLinkProgram(prog);
    for (GLuint s : stages) {
        glDetachShader(prog, s);
        glDeleteShader(s);
    }
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string info(static_cast<size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, info.data());
        log_ += "link error: " + info;
        glDeleteProgram(prog);
        logError("shader link failed:\n{}", log_);
        return false;
    }
    if (program_) glDeleteProgram(program_);
    program_ = prog;
    uniformCache_.clear();
    return true;
}

void ShaderProgram::use() const { glUseProgram(program_); }

GLint ShaderProgram::uniform(const std::string& name) {
    auto it = uniformCache_.find(name);
    if (it != uniformCache_.end()) return it->second;
    GLint loc = glGetUniformLocation(program_, name.c_str());
    uniformCache_.emplace(name, loc);
    return loc;
}

void ShaderProgram::set(const std::string& n, int v) { glUniform1i(uniform(n), v); }
void ShaderProgram::set(const std::string& n, unsigned v) { glUniform1ui(uniform(n), v); }
void ShaderProgram::set(const std::string& n, float v) { glUniform1f(uniform(n), v); }
void ShaderProgram::set(const std::string& n, const glm::vec2& v) { glUniform2fv(uniform(n), 1, glm::value_ptr(v)); }
void ShaderProgram::set(const std::string& n, const glm::vec3& v) { glUniform3fv(uniform(n), 1, glm::value_ptr(v)); }
void ShaderProgram::set(const std::string& n, const glm::vec4& v) { glUniform4fv(uniform(n), 1, glm::value_ptr(v)); }
void ShaderProgram::set(const std::string& n, const glm::ivec2& v) { glUniform2iv(uniform(n), 1, glm::value_ptr(v)); }
void ShaderProgram::set(const std::string& n, const glm::mat3& v) { glUniformMatrix3fv(uniform(n), 1, GL_FALSE, glm::value_ptr(v)); }
void ShaderProgram::set(const std::string& n, const glm::mat4& v) { glUniformMatrix4fv(uniform(n), 1, GL_FALSE, glm::value_ptr(v)); }

}  // namespace bhs
