# Cloud Render

Interactive D3D11/NanoVDB cloud renderer implementing `design.md`.

## Build

```powershell
cmake --preset windows-msvc-vcpkg
cmake --build --preset windows-msvc-vcpkg
```

Run:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe
```

Use the `VDB path` control in the UI to load or reload `.vdb` and `.nvdb` files. You can still pass an initial path on the command line.

Headless smoke check:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe --check .\data\cabauw.vdb
```

One-frame D3D11 smoke run:

```powershell
.\build\windows-vs-vcpkg\RelWithDebInfo\cloud_render.exe --frames 1 .\data\cabauw.vdb
```

The renderer converts `.vdb` float grids to NanoVDB on load. `.nvdb` files are uploaded as NanoVDB payloads directly.
`data/nubis.dds` is loaded as the Nubis Cubed 3D detail-noise texture and copied next to the executable by CMake.
Slang sources are split by pass under `shaders/`, with `PNanoVDB.h` colocated there; CMake emits per-pass HLSL files that the D3D11 app compiles at startup.

Press `F1` to toggle the UI. With the UI hidden, fly controls use mouse look plus `WASD/QE`, with movement scaled to the loaded volume bounds. In path tracer mode, the `Path history` control switches between temporal denoising and progressive accumulation.

## Reference
- OpenVDB `tools::fogToSdf` / fast sweeping
- `ref/VoxelCloudSampler.cg`
