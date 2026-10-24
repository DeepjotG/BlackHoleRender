cmake_minimum_required(VERSION 3.24)
project(BlackholeSimulation LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- GLFW 3.4
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
option(BHS_GLFW_WAYLAND "Build GLFW with native Wayland support" ON)
set(GLFW_BUILD_WAYLAND ${BHS_GLFW_WAYLAND} CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)

# ---------------------------------------------------------------- GLM 1.0.1
FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE)

# ---------------------------------------------------------------- Dear ImGui
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE)

# ---------------------------------------------------------------- glad2 (GL 4.6 core loader, generated at configure time)
FetchContent_Declare(glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v2.0.8
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR cmake)

# ---------------------------------------------------------------- stb (image write / load)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG master
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(glfw glm imgui glad stb)

glad_add_library(glad_gl_core_46 REPRODUCIBLE API gl:core=4.6)

# ImGui static library (core + GLFW + OpenGL3 backends)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# stb interface library
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

find_package(OpenGL REQUIRED)
# libEGL (glvnd) for the NVIDIA rendering bridge; found directly because FindOpenGL's EGL
# component insists on libOpenGL.so, which is not installed by default.
find_library(BHS_EGL_LIBRARY NAMES EGL REQUIRED)
find_path(BHS_EGL_INCLUDE_DIR EGL/egl.h REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- core library (shared by app and tests)
add_library(bhs_core STATIC
  src/Settings.cpp
  src/Camera.cpp
  src/Units.cpp
  src/BlackHoleSystem.cpp
  src/Bodies.cpp)
target_include_directories(bhs_core PUBLIC src)
target_link_libraries(bhs_core PUBLIC glm::glm)
target_compile_definitions(bhs_core PUBLIC GLM_ENABLE_EXPERIMENTAL GLM_FORCE_SILENT_WARNINGS)

# ---------------------------------------------------------------- application
add_executable(blackhole
  src/main.cpp
  src/App.cpp
  src/Shader.cpp
  src/Renderer.cpp
  src/SkyBaker.cpp
  src/DiskTextures.cpp
  src/Ui.cpp
  src/Capture.cpp
  src/GpuBridge.cpp
  src/stb_impl.cpp)
target_link_libraries(blackhole PRIVATE bhs_core glad_gl_core_46 glfw imgui stb OpenGL::GL ${BHS_EGL_LIBRARY} Threads::Threads ${CMAKE_DL_LIBS})
target_include_directories(blackhole PRIVATE ${BHS_EGL_INCLUDE_DIR})
target_compile_definitions(blackhole PRIVATE
  BHS_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
  BHS_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(blackhole PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_compile_options(bhs_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(geodesic_tests tests/geodesic_tests.cpp)
target_link_libraries(geodesic_tests PRIVATE bhs_core)
add_test(NAME geodesic_tests COMMAND geodesic_tests)
add_executable(settings_tests tests/settings_tests.cpp)
target_link_libraries(settings_tests PRIVATE bhs_core)
add_test(NAME settings_tests COMMAND settings_tests)
add_executable(bhsystem_tests tests/bhsystem_tests.cpp)
target_link_libraries(bhsystem_tests PRIVATE bhs_core)
add_test(NAME bhsystem_tests COMMAND bhsystem_tests)
