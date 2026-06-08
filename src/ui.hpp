#pragma once

#include "renderer.hpp"
#include "volume_io.hpp"

#include <array>
#include <string>

namespace cloud_render {

struct VolumeUiState {
    std::array<char, 1024> path = {};
    std::string status;
    bool statusIsError = false;
};

struct UiActions {
    bool settingsChanged = false;
    bool loadVolumeRequested = false;
};

UiActions buildUi(RenderSettings& settings, const Volume* volume, VolumeUiState& volumeUi, float fps, float frameMs);

} // namespace cloud_render
