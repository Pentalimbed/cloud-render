#include "ui.hpp"

#include <imgui.h>

namespace cloud_render {
namespace {

bool controlVec3Color(const char* label, Vec3& value)
{
    float color[3] = {value.x, value.y, value.z};
    const bool changed = ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    if (changed) {
        value = {color[0], color[1], color[2]};
    }
    return changed;
}

bool controlVec3Drag(const char* label, Vec3& value, float speed, float minValue, float maxValue)
{
    float values[3] = {value.x, value.y, value.z};
    const bool changed = ImGui::DragFloat3(label, values, speed, minValue, maxValue, "%.3f");
    if (changed) {
        value = {values[0], values[1], values[2]};
    }
    return changed;
}

void controlHint(const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

bool buildUi(RenderSettings& settings, const Volume& volume, float fps, float frameMs)
{
    bool changed = false;
    ImGui::Begin("Cloud Renderer");
    ImGui::TextUnformatted(volume.gridName.c_str());
    ImGui::Text("Max density %.6g", volume.maxDensity);
    controlHint("Maximum active voxel density in the loaded volume before the UI Density multiplier.");
    ImGui::Text("Scaled max density %.6g", volume.maxDensity * settings.densityMultiplier);
    controlHint("Maximum active voxel density after the current UI Density multiplier.");
    ImGui::Text("FPS %.1f (%.2f ms)", fps, frameMs);
    ImGui::TextWrapped("Fly: F1 toggles UI, mouse look, WASD move, Q/E down/up, Shift fast");
    ImGui::Separator();

    const char* modes[] = {"Ray marching", "Path tracer"};
    changed |= ImGui::Combo("Renderer", &settings.rendererMode, modes, 2);
    controlHint("Selects the primary volume integrator.");
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    const char* debugViews[] = {"Off", "Volume sample count"};
    changed |= ImGui::Combo("Debug view", &settings.debugViewMode, debugViews, 2);
    controlHint("Overrides the final image with renderer-side diagnostic output.");
    if (settings.debugViewMode == 1) {
        changed |= ImGui::SliderFloat("Sample count scale", &settings.debugSampleCountScale, 1.0f, 4096.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        controlHint("False-color scale for the logarithmic per-pixel volume sample count view.");
    }
#endif
    if (settings.rendererMode == 1) {
        const char* historyModes[] = {"Temporal denoiser", "Accumulation"};
        changed |= ImGui::Combo("Path history", &settings.pathHistoryMode, historyModes, 2);
        controlHint("Accumulation averages path-traced samples until the camera or settings change.");
    }
    changed |= ImGui::SliderFloat("Density", &settings.densityMultiplier, 0.0f, 20.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    controlHint("Scales sampled VDB density before absorption and scattering.");
    changed |= controlVec3Color("Absorption", settings.absorption);
    controlHint("Per-channel extinction that removes light in the medium.");
    changed |= controlVec3Color("Scattering", settings.scattering);
    controlHint("Per-channel scattering coefficient for in-scattered light.");
    changed |= controlVec3Drag("Light direction", settings.lightDirection, 0.01f, -1.0f, 1.0f);
    controlHint("Directional light vector; it is normalized after editing.");
    settings.lightDirection = normalize(settings.lightDirection);
    changed |= controlVec3Color("Light color", settings.lightColor);
    controlHint("HDR directional light radiance.");
    const char* phaseModes[] = {"Henyey-Greenstein", "Draine", "Jendersie Mie"};
    changed |= ImGui::Combo("Phase", &settings.phaseFunctionMode, phaseModes, 3);
    controlHint("Controls angular scattering distribution used by lighting and path scattering.");
    if (settings.phaseFunctionMode == 2) {
        changed |= ImGui::SliderFloat("Cloud diameter (um)", &settings.cloudDiameterMicrons, 0.001f, 50.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        controlHint("Particle diameter for the CPU-evaluated Jendersie cloud phase approximation.");
    } else {
        changed |= ImGui::SliderFloat("Anisotropy", &settings.anisotropy, -0.95f, 0.95f, "%.2f");
        controlHint("Phase asymmetry parameter: positive values favor forward scattering.");
        if (settings.phaseFunctionMode == 1) {
            changed |= ImGui::SliderFloat("Draine alpha", &settings.draineAlpha, 0.0f, 250.0f, "%.3f");
            controlHint("Draine alpha shape parameter; 1 matches Cornette-Shanks.");
        }
    }
    changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    controlHint("Post-tonemap exposure multiplier.");
    changed |= ImGui::SliderFloat("Temporal blend", &settings.temporalBlend, 0.0f, 0.98f, "%.2f");
    controlHint("History weight for temporal denoising.");
    if (settings.rendererMode == 0) {
        changed |= ImGui::SliderFloat("Step jitter", &settings.stepJitter, 0.0f, 1.0f, "%.2f");
        controlHint("Randomizes raymarch step positions to reduce banding.");
        changed |= ImGui::SliderFloat("Primary min step", &settings.raymarchPrimaryMinStep, 0.01f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        controlHint("Minimum world-space step length for primary ray marching.");
        changed |= ImGui::SliderFloat("Primary distance scale", &settings.raymarchPrimaryStepScale, 0.0f, 1.0f, "%.3f");
        controlHint("Scales sqrt(distance from camera) for adaptive primary ray steps.");
        changed |= ImGui::SliderInt("Shadow ray steps", &settings.raymarchShadowSteps, 4, 256);
        controlHint("Ray marcher light transmittance steps.");
    } else {
        changed |= ImGui::SliderFloat("Majorant", &settings.densityMajorant, 0.05f, 25.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        controlHint("Scalar null-collision majorant multiplier for the path tracer.");
        changed |= ImGui::SliderInt("Max bounces", &settings.pathMaxBounces, 1, 12);
        controlHint("Maximum number of volume scattering events per path.");
    }

    ImGui::End();
    return changed;
}

} // namespace cloud_render
