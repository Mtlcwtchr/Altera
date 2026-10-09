# Terrain Lab: NVIDIA DLSS Super Resolution

On Windows x64, Terrain Lab builds with the official NVIDIA NGX/DLSS SDK.
It uses Direct3D 12 so DLSS can be switched on and off without reloading the
terrain. DLSS starts **off**. Press **F12** to toggle it; the window title shows
the actual state and input/output resolutions.

```bat
terrain_lab.exe --dlss
terrain_lab.exe --dlss --dlss-quality balanced
terrain_lab.exe --dlss --dlss-quality performance
terrain_lab.exe --no-dlss
```

The default DLSS mode is `quality`. `--dlss-quality` selects the mode; it does
not enable DLSS by itself. `--dlss` and `--no-dlss` are processed in order.
The same options work with headless screenshots and benchmarks. Screenshots
remain at the output resolution. Benchmark summaries record whether DLSS was
actually active, so unsupported hardware cannot be mistaken for a DLSS result.

DLSS requires a compatible NVIDIA RTX GPU and driver. NGX checks support at
startup; failures are reported in the console and window title. Unsupported
GPUs, SDK-free builds and an explicitly selected Vulkan backend render at
native resolution. An NGX evaluation failure uses a scaled fallback for that
frame, then returns to native resolution on the next frame.

## Build

From the project's configured Visual Studio / clang-cl developer environment:

```bat
cmake -S . -B cmake-build-windows -DASR_ENABLE_DLSS=ON
cmake --build cmake-build-windows --target terrain_lab
```

The SDK is fetched from NVIDIA at commit
`374959484e79a640feaba44c93ac8cfb0a03f5b5`. An existing SDK can be supplied with
`-DASR_DLSS_SDK_ROOT=C:/path/to/DLSS`; the local `.cache/dlss-sdk` checkout is
also recognized. CMake copies the release `nvngx_dlss.dll` beside the executable.
The SDK and runtime are governed by NVIDIA's
[RTX SDK license](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt).
When distributing the executable, include that license with the runtime.

Use `-DASR_ENABLE_DLSS=OFF` for SDK-free Windows builds. Other platforms default
to that setting and retain their normal rendering backend. The small CPU
reprojection tests run without NVIDIA hardware:

```bat
cmake --build cmake-build-windows --target asr_dlss_math_tests
ctest --test-dir cmake-build-windows -R dlss_math --output-on-failure
```

With `ASR_BUILD_TESTS=ON`, `asr_dlss_gpu_check` tests actual GPU output, all three
quality modes, resizing and disable/re-enable cycles. Run it from the project
root. `ASR_TEST_GPU=ON` also registers it with CTest; unsupported GPUs skip it.

## Model, texture detail, cost

Every quality mode runs DLSS preset K (the transformer model the SDK
recommends). Left to NGX, Performance mode picks preset M, which on an RTX 3070
Laptop costs more than Quality's K at its lower input resolution: Performance
was the slowest of the three.

The ground's materials pick their mip for the screen, not the smaller render
target (`Renderer::setTextureScale`, render width over output width): the LOD
bias upscaling expects, so textures stay as sharp as at native resolution.

Walking through forest on the v5 map (RTX 3070 Laptop, all modules, mean frame
time, the better of two runs: a laptop's power states swing single runs by
several milliseconds):

| output | native | Quality | Balanced | Performance |
|---|---|---|---|---|
| 1920x1080 | 6.3 ms | 6.4 ms | 6.1 ms | 5.9 ms |
| 2560x1440 | 9.2 ms | 9.0 ms | 8.3 ms | 7.5 ms |

The scene's resolution-independent work (shadow cascades, tree geometry,
spawns) and DLSS's own pass keep the saving small at 1080p; it grows with the
output resolution. Before preset K was pinned, Performance was the slowest
mode (preset M).

## Rendering details and current limits

NGX selects the optimal internal resolution. The scene renders into separate
targets with Halton jitter. A compute pass reconstructs camera motion from the
reversed depth buffer and the previous eye-relative camera, including infinite
sky directions. Clouds are composited before DLSS; picture styles are applied
at output resolution afterwards. History resets on activation, resize, quality
changes, large camera jumps and sharp camera turns. All native resource barriers,
SDL lifetime tracking and descriptor heap restoration are handled by the
extension to the pinned SDL 3.2.8 D3D12 backend. The build compiles an altered
copy that also initializes the array UAV plane index missing in SDL 3.2.8;
the fetched SDL files are unmodified. Depth is read through SDL's supported
sampler interface in the cloud and motion compute shaders.

Motion vectors currently describe camera motion. They do not include vegetation
wind deformation or animated water/cloud motion; temporal trails can occur on
those surfaces. Use `--no-wind --no-water --no-clouds` when inspecting static
reconstruction quality. This integration is DLSS Super Resolution; frame
generation and ray reconstruction are separate features.
