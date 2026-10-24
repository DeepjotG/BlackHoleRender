# Black Hole Simulation

A real-time, physically based renderer of a Schwarzschild black hole in the style of
*Interstellar*'s Gargantua, written in C++20 and OpenGL 4.6. Every pixel traces a light
ray backwards through curved spacetime, so the shadow, the photon ring, the disk wrapping
over and under the hole, and the Einstein-ring stretching of background stars all come
out of the physics rather than from tricks. You can fly around freely.

![](docs/preview.png)

## Build and run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build        # physics checks
./run.sh                      # launches on the RTX 3060 when its driver is loaded
```

Dependencies (GLFW 3.4, GLM, Dear ImGui, glad2, stb) are fetched by CMake at configure
time; the only system requirements are a C++20 compiler, CMake 3.24+, Python 3 with
`jinja2` (for the OpenGL loader generator) and the X11/Wayland development headers.

## Enabling the RTX 3060 (one-time)

On this laptop the NVIDIA driver is installed but its kernel module is rejected by Secure
Boot because the signing key was never enrolled (`modprobe: could not insert 'nvidia':
Key was rejected by service`). Enroll it once:

```bash
sudo mokutil --import /var/lib/shim-signed/mok/MOK.der   # choose a one-time password
sudo reboot
# Blue "MOK management" screen: Enroll MOK -> Continue -> Yes -> password -> Reboot
nvidia-smi                                               # must now list the RTX 3060
```

Alternatively disable Secure Boot in the BIOS. Without this the app still runs on the
Radeon iGPU (use the Low preset); `run.sh` detects the driver automatically.

## Controls

| Input | Action |
|---|---|
| Mouse | Look (Tab toggles mouse capture so you can use the panel) |
| W A S D | Move |
| Space / Ctrl | Up / down |
| Q / E | Roll |
| Shift | 5x boost |
| Scroll | Change base speed (speed also scales with distance to the hole) |
| 1-4 | Fly to bookmarks (Interstellar shot, photon-sphere skim, top-down, far) |
| R | Reset view |
| O | Auto-orbit |
| L | Toggle lensing |
| P | Pause the disk |
| H | Hide the panel |
| F2 / F3 | Screenshot / cinematic 4K capture (24 accumulated samples) |
| F5 | Reload shaders (they also hot-reload on save) |
| Esc | Quit |

## Command line

```
--preset low|medium|high|ultra   --bookmark N   --pos x,y,z --target x,y,z
--frames N --screenshot out.png  render N frames headlessly and save the last one
--cinematic out.png [--capture-size 3840x2160 --samples 24]
--benchmark                      time every preset and print a table
--lensing 0|1  --disk none|thin|volumetric  --doppler 0..1  --debug steps|footprint
--fullscreen --width W --height H --wayland|--x11 --seed N --vsync 0|1
```

## How it works

* **Units:** G = c = M = 1. Horizon r = 2, photon sphere r = 3, ISCO (disk inner edge) r = 6,
  critical impact parameter 3√3.
* **Geodesics:** the orbit shape of a photon in Schwarzschild spacetime obeys
  `x'' = -3 h² x / r⁵` with `h = |x × v|` conserved (exactly equivalent to the Binet
  equation `u'' + u = 3u²`). Each pixel integrates this with RK4, step ∝ r, from a static
  observer whose local view direction is mapped to coordinate velocity with the
  `sqrt(1 - 2/r)` radial contraction. Rays leave the march once the remaining first-order
  deflection is below a tolerance and that remainder is added analytically.
* **Disk:** a volumetric slab with Keplerian flow-advected tileable 3D noise, a thin-disk
  temperature profile `T ∝ (r_in/r)^(3/4) (1 - √(r_in/r))^(1/4)`, blackbody colours from a
  CIE-integrated Planck LUT, and an exact redshift factor
  `g = 1 / (√(1-2/r_cam) · u^t · (1 - Ω L/E))` for Doppler beaming and gravitational
  redshift. The *Doppler + redshift* slider blends between the film's symmetric look and
  the physical one.
* **Sky:** a compute shader bakes a cubemap with a Milky Way band (domain-warped fbm with
  dust lanes), five nebulae and thousands of tile-binned spiral/elliptical galaxies. Stars
  are analytic per pixel: screen-space Jacobians of the escaped direction give each star a
  lensing magnification and anisotropic stretch, so they brighten into arcs near the
  Einstein ring and average correctly where the footprint is large.
* **Post:** progressive supersampling while still, contrast-adaptive upscaling, 13-tap
  bloom chain, AgX/ACES tonemapping, optional auto-exposure, chromatic aberration, grain.

Physics checks live in `tests/geodesic_tests.cpp` (weak-field deflection against
`4/b + 15π/4b²`, capture threshold, shadow angular size for a static observer, photon
sphere stability, redshift closed forms, far-field correction).

## Project layout

```
src/            application (App, Camera, Renderer, SkyBaker, DiskTextures, Ui, Capture)
src/physics/    CPU copy of the geodesic math used by the tests
shaders/        GLSL: blackhole.frag (tracer), geodesic/disk/sky includes, bake and post passes
tests/          physics unit checks (ctest)
run.sh          launcher with NVIDIA PRIME offload
```
