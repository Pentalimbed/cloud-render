# Goal
An interactive cloud renderer that:
- Reads a .vdb/.nvdb file (e.g. [cabauw.vdb](cabauw.vdb)), and upload to GPU in nanovdb data structure
    - +Y is up
    - Unit is meter by default
- Implement a real time ray marching renderer of the volume, as well as a path tracer
    - In RGB
    - Samples the volume using linear stochastic interpolation
    - Has a simple temporal denoiser pass and a tonemapping post pass
- (When UI is off) Has fly camera control
- Has UI controls
    - density multiplier
    - absorption and scattering coefficients
    - direct light direction and color
    - sample budgets

# Stack
- C++
- CMake
- vcpkg
- openvdb/nanovdb
    - [PNanoVDB.h](PNanoVDB.h) in shader
- glfw
- imgui
- d3d11
    - slang shader language
    - compute shader only

# Note
- Can move existing files around for organization, except [design.md](design.md)

# Reference
- https://developer.nvidia.com/blog/accelerating-openvdb-on-gpus-with-nanovdb/ for how nanovdb is used
- https://github.com/mitsuba-renderer/mitsuba3/blob/master/src/integrators/volpathmis.cpp as the basis of the integrator of the path tracer
- [Interactive Path Tracing and Reconstruction of Sparse Volumes](ref/hofmann2021.pdf) for several optimization techniques w.r.t. vdb sampling, namely Stochastic Interpolation and Empty Space Skipping