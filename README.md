# Cloud Render

Interactive D3D11/NanoVDB cloud renderer implementing `design.md`.

## Build

```powershell
cmake --preset windows-msvc-vcpkg
cmake --build --preset windows-msvc-vcpkg
```

Run:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe .\cabauw.vdb
```

Headless smoke check:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe --check .\cabauw.vdb
```

One-frame D3D11 smoke run:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe --frames 1 .\cabauw.vdb
```

The renderer converts `.vdb` float grids to NanoVDB on load. `.nvdb` files are uploaded as NanoVDB payloads directly.
Slang sources are split by pass under `shaders/`; CMake emits per-pass HLSL files that the D3D11 app compiles at startup.
