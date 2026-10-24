# 3D universe, planets and star disruption: design

## Goal (from the user)

* Choose between the existing painted **Backdrop**, an **Invented** 3D universe and the **Real** 3D
  universe (real stars, real galaxies, the Milky Way, the solar system).
* Real stars and galaxies with actual positions: parallax when flying, galaxies you can fly into,
  travel at any speed up to far beyond light speed.
* Planets you can fly to, orbiting the black hole (Interstellar-style) and, in the real universe,
  the solar system.
* Spawn a star and watch the black hole tear it apart and eat it (tidal disruption event).
* A black hole mass slider.

## Frames and units

* **Renderer frame**: black hole at the origin, +Y = spin axis = accretion disk normal, lengths in
  M = G M_bh / c^2, times in G M_bh / c^3 (`src/Units.h`). The camera, the disk, black-hole planets
  and the star-disruption simulation live here. Changing the mass slider keeps everything defined in
  M fixed (the black hole looks the same); physical objects (stars, planets' physical radii, the
  universe) change size relative to it.
* **Galactic frame**: meters, double precision, origin at the host galaxy centre, +Y = host disk
  normal (`src/universe/Universe.h`). Real universe: galactocentric, +X from the Sun towards the
  Galactic Centre, +Y north galactic pole, +Z = X x Y; Sun at about (-8178 pc, +20.8 pc, 0).
* Mapping: `renderer = R * (galactic - blackHolePos) / metersPerM`, R rotates `blackHoleSpinAxis`
  to +Y.
* Simulation clock: physical seconds of coordinate time far from the hole, advanced by
  `timeWarp * frame dt`. Black-hole-system code converts with `secondsPerM`.

## Module ownership

| Module | Owner | Files |
|---|---|---|
| Star disruption simulation | worker "tde" | `src/tde/*.cpp`, `src/tde/*_internal.h`, `shaders/tde_*.comp`, `tests/tde_tests.cpp` |
| Galaxy model, procedural star field, invented universe | worker "invented" | `src/universe/GalaxyModel.cpp`, `shaders/galaxy_model.glsl`, `src/universe/StarField.cpp`, `src/universe/InventedUniverse.cpp`, `tests/galaxy_model_tests.cpp`, `tests/starfield_tests.cpp`, `tests/invented_universe_tests.cpp` |
| Real catalogues and real universe | worker "real" | `tools/fetch_real_catalogs.py`, `assets/real/**`, `src/universe/RealUniverse.cpp`, `src/universe/RealUniverse_internal.h`, `tests/real_universe_tests.cpp` |
| Everything else (app, camera, navigation, renderer, ray tracer, sky, planets rendering, UI, CMake) | lead | all other files |

The interface headers (`src/Units.h`, `src/universe/GalaxyModel.h`, `src/universe/Universe.h`,
`src/universe/StarField.h`, `src/tde/TdeSim.h`) are owned by the lead. Workers must not change
them; if a contract is wrong or insufficient, say so precisely in the final report and work around
it inside your own files.

## Rules for workers

* Only create or edit files listed for your module. Do not touch `CMakeLists.txt`, other sources,
  shaders or the main checkout at `/mnt/storage/BlackholeSimulation` (the user runs the app from it).
* Build and test with your own commands in a private scratch directory (`<job tmp>/<worker>/` during development)
  (direct `g++ -std=c++20` or a private CMake project). Useful prebuilt pieces:
  * GLM headers: `/mnt/storage/BlackholeSimulation/build/_deps/glm-src`
  * glad (GL 4.6 core): headers `/mnt/storage/BlackholeSimulation/build/gladsources/glad_gl_core_46/include`,
    library `/mnt/storage/BlackholeSimulation/build/libglad_gl_core_46.a`
  * libEGL: `-lEGL`; project sources: `src/` of this worktree (e.g. `src/Shader.cpp`, `src/Log.h`).
* GPU tests must be headless: create a surfaceless EGL device context on the NVIDIA GPU exactly as
  `src/GpuBridge.cpp` does (enumerate devices, pick the one whose extensions contain
  `EGL_NV_device_cuda`, `eglBindAPI(EGL_OPENGL_API)`, GL 4.6 core context, `eglMakeCurrent` with
  `EGL_NO_SURFACE`), then `gladLoadGL((GLADloadfunc)eglGetProcAddress)`. Never open a window; keep
  GPU runs short (the user is using the machine).
* C++20, namespace `bhs::...`, logging through `src/Log.h`. Load compute shaders with
  `bhs::ShaderProgram::loadCompute("name.comp")` after `ShaderProgram::setShaderDir(<worktree>/shaders)`;
  `#include "common.glsl"` provides hashes and gradient noise.
* GLSL 4.60 core that compiles on both NVIDIA and Mesa radeonsi: no float atomics (use integer
  fixed-point atomics), no vendor extensions, no undefined behaviour (check NaN sources, integer
  division, out-of-range indexing).
* Deterministic results for a given seed. No new third-party dependencies (Python scripts may use
  the standard library only).

## Budgets (RTX 3060 Laptop, 1080p)

* Star disruption: <= 3 ms GPU per frame at 262k particles, independent of the time warp.
* Star field `gather`: <= 3 ms amortised; <= 300k stars in total.
* Diffuse sky bake (lead): progressive, one cube face per frame while moving.

## Rendering overview (lead)

* `blackhole.frag` marches geodesics near the hole. Along each step it tests spheres from a body
  buffer (planets, stars close to the camera, the intact disrupted star) with step limiting near
  their surfaces, and samples the star-disruption gas volumes. After escape the ray is straight and
  is tested against near bodies, then the sky is sampled.
* Sky in 3D modes: a diffuse cubemap baked from the viewpoint by ray marching the galaxy models
  (`galaxy_model.glsl`) plus distant galaxies, and point stars from a GPU star buffer binned by
  direction every frame and evaluated per pixel with the existing lensing-aware footprint math.
* Far from the hole (> 1e7 M) lensing is sub-pixel and rays are straight from the camera; the hole
  is drawn as a point of light.
