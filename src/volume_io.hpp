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
};

struct Volume {
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    nanovdb::GridHandle<nanovdb::HostBuffer> qCriterionHandle;
    nanovdb::GridHandle<nanovdb::HostBuffer> signedDistanceHandle;
    CoarseSignedDistanceVolume coarseSignedDistance;
    Vec3 nativeWorldMin = {0.0f, 0.0f, 0.0f};
    Vec3 nativeWorldMax = {0.0f, 0.0f, 0.0f};
    Vec3 worldMin = {0.0f, 0.0f, 0.0f};
    Vec3 worldMax = {0.0f, 0.0f, 0.0f};
    float maxDensity = 0.0f;
    float maxDistanceToZero = 0.0f;
    float qCriterionMin = 0.0f;
    float qCriterionMax = 0.0f;
    float qCriterionAbsMax = 0.0f;
    int nativeUpAxis = 2;
    bool hasQCriterion = false;
    std::string gridName;
    std::string qCriterionGridName;
};

Volume loadVolume(const std::filesystem::path& path);

} // namespace cloud_render
