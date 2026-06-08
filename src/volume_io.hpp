#pragma once

#include "core.hpp"

#include <filesystem>
#include <string>

#include <nanovdb/io/IO.h>

namespace cloud_render {

struct Volume {
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    Vec3 nativeWorldMin = {0.0f, 0.0f, 0.0f};
    Vec3 nativeWorldMax = {0.0f, 0.0f, 0.0f};
    Vec3 worldMin = {0.0f, 0.0f, 0.0f};
    Vec3 worldMax = {0.0f, 0.0f, 0.0f};
    float maxDensity = 0.0f;
    int nativeUpAxis = 2;
    std::string gridName;
};

Volume loadVolume(const std::filesystem::path& path);

} // namespace cloud_render
