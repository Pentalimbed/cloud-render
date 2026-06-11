#pragma once

#include "core.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nanovdb/io/IO.h>

namespace cloud_render {

struct CoarseSignedDistanceVolume {
    std::array<uint32_t, 3> size = {};
    std::vector<float> values;
    float safetyMargin = 0.0f;
};

struct Volume {
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    nanovdb::GridHandle<nanovdb::HostBuffer> signedDistanceHandle;
    CoarseSignedDistanceVolume coarseSignedDistance;
    Vec3 nativeWorldMin = {0.0f, 0.0f, 0.0f};
    Vec3 nativeWorldMax = {0.0f, 0.0f, 0.0f};
    Vec3 worldMin = {0.0f, 0.0f, 0.0f};
    Vec3 worldMax = {0.0f, 0.0f, 0.0f};
    float maxDensity = 0.0f;
    float maxDistanceToZero = 0.0f;
    int nativeUpAxis = 2;
    std::string gridName;
};

Volume loadVolume(const std::filesystem::path& path);

} // namespace cloud_render
