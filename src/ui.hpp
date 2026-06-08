#pragma once

#include "renderer.hpp"
#include "volume_io.hpp"

namespace cloud_render {

bool buildUi(RenderSettings& settings, const Volume& volume, float fps, float frameMs);

} // namespace cloud_render
