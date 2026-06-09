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

UiActions buildUi(RenderSettings& settings, const Volume* volume, VolumeUiState& volumeUi, float fps, float frameMs)
{
    UiActions actions;
    ImGui::Begin("Cloud Renderer");

    const bool inputSubmitted = ImGui::InputText("VDB path", volumeUi.path.data(), volumeUi.path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool pathReady = volumeUi.path[0] != '\0';
    ImGui::SameLine();
    if (!pathReady) {
        ImGui::BeginDisabled();
    }
    const bool loadClicked = ImGui::Button("Load");
    if (!pathReady) {
        ImGui::EndDisabled();
    }
    actions.loadVolumeRequested = pathReady && (inputSubmitted || loadClicked);

    if (!volumeUi.status.empty()) {
        const ImVec4 color = volumeUi.statusIsError ? ImVec4(1.0f, 0.35f, 0.32f, 1.0f) : ImVec4(0.55f, 0.95f, 0.65f, 1.0f);
        ImGui::TextColored(color, "%s", volumeUi.status.c_str());
    }

    if (volume) {
        ImGui::TextUnformatted(volume->gridName.c_str());
        ImGui::Text("Max mixing ratio %.6g kg/kg", volume->maxDensity);
        controlHint("Maximum active voxel density in the loaded volume before the UI Density multiplier.");
        ImGui::Text("Max distance to zero %.6g", volume->maxDistanceToZero);
        controlHint("Maximum signed-distance magnitude stored in the generated companion volume.");
        ImGui::Text("Scaled max density %.6g", volume->maxDensity * settings.densityMultiplier);
        controlHint("Maximum active voxel density after the current UI Density multiplier.");
    } else {
        ImGui::TextUnformatted("No volume loaded");
    }
    ImGui::Text("FPS %.1f (%.2f ms)", fps, frameMs);
    ImGui::TextWrapped("Fly: F1 toggles UI, mouse look, WASD move, Q/E down/up, Shift fast");
    ImGui::Separator();

    const char* modes[] = {"Ray marching", "Path tracer"};
    actions.settingsChanged |= ImGui::Combo("Renderer", &settings.rendererMode, modes, 2);
    controlHint("Selects the primary volume integrator.");
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    const char* debugViews[] = {"Off", "Volume sample count", "Ray max distance"};
    actions.settingsChanged |= ImGui::Combo("Debug view", &settings.debugViewMode, debugViews, 3);
    controlHint("Overrides the final image with renderer-side diagnostic output.");
    if (settings.debugViewMode == 1) {
        actions.settingsChanged |= ImGui::SliderFloat("Sample count scale", &settings.debugSampleCountScale, 1.0f, 4096.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
        controlHint("False-color scale for the logarithmic per-pixel volume sample count view.");
    }
#endif
    if (settings.rendererMode == 1) {
        const char* historyModes[] = {"Temporal denoiser", "Accumulation"};
        actions.settingsChanged |= ImGui::Combo("Path history", &settings.pathHistoryMode, historyModes, 2);
        controlHint("Accumulation averages path-traced samples until the camera or settings change.");
    }
    actions.settingsChanged |= ImGui::SliderFloat("Density", &settings.densityMultiplier, 0.0f, 500.0f, "%.3f");
    controlHint("Scales sampled VDB density before absorption and scattering.");
    actions.settingsChanged |= ImGui::SliderFloat("Nubis detail type", &settings.nubisDetailType, 0.0f, 1.0f, "%.2f");
    controlHint("Blends the Nubis up-res noise from wispy details at 0 to billowy details at 1.");
    actions.settingsChanged |= ImGui::SliderFloat("Max distance to zero", &settings.maxDistanceToZero, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    controlHint("Normalizes the signed-distance profile used for the dimension profile lookup.");
    actions.settingsChanged |= controlVec3Color("Absorption", settings.absorption);
    controlHint("Per-channel extinction that removes light in the medium.");
    actions.settingsChanged |= controlVec3Color("Scattering", settings.scattering);
    controlHint("Per-channel scattering coefficient for in-scattered light.");
    actions.settingsChanged |= controlVec3Drag("Light direction", settings.lightDirection, 0.01f, -1.0f, 1.0f);
    controlHint("Directional light vector; it is normalized after editing.");
    settings.lightDirection = normalize(settings.lightDirection);
    actions.settingsChanged |= controlVec3Color("Light color", settings.lightColor);
    controlHint("HDR directional light radiance.");
    const char* phaseModes[] = {"Henyey-Greenstein", "Draine", "Jendersie Mie"};
    actions.settingsChanged |= ImGui::Combo("Phase", &settings.phaseFunctionMode, phaseModes, 3);
    controlHint("Controls angular scattering distribution used by lighting and path scattering.");
    if (settings.phaseFunctionMode == 2) {
        actions.settingsChanged |= ImGui::SliderFloat("Particle diameter (um)", &settings.particleDiameterMicrons, 0.001f, 50.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        controlHint("Particle diameter for the CPU-evaluated Jendersie cloud phase approximation.");
    } else {
        actions.settingsChanged |= ImGui::SliderFloat("Anisotropy", &settings.cloudPhaseGhg, -0.95f, 0.95f, "%.2f");
        controlHint("Phase asymmetry parameter: positive values favor forward scattering.");
        if (settings.phaseFunctionMode == 1) {
            actions.settingsChanged |= ImGui::SliderFloat("Draine alpha", &settings.cloudPhaseAlpha, 0.0f, 250.0f, "%.3f");
            controlHint("Draine alpha shape parameter; 1 matches Cornette-Shanks.");
        }
    }
    actions.settingsChanged |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    controlHint("Post-tonemap exposure multiplier.");
    actions.settingsChanged |= ImGui::SliderFloat("Temporal blend", &settings.temporalBlend, 0.0f, 0.98f, "%.2f");
    controlHint("History weight for temporal denoising.");
    if (settings.rendererMode == 0) {
        actions.settingsChanged |= ImGui::SliderFloat("Step jitter", &settings.stepJitter, 0.0f, 1.0f, "%.2f");
        controlHint("Randomizes raymarch step positions to reduce banding.");
        actions.settingsChanged |= ImGui::SliderFloat("Primary min step", &settings.raymarchPrimaryMinStep, 0.01f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        controlHint("Minimum world-space step length for primary ray marching.");
        actions.settingsChanged |= ImGui::SliderFloat("Primary distance scale", &settings.raymarchPrimaryStepScale, 0.0f, 1.0f, "%.3f");
        controlHint("Scales sqrt(distance from camera) for adaptive primary ray steps.");
        actions.settingsChanged |= ImGui::SliderInt("Shadow ray steps", &settings.raymarchShadowSteps, 4, 256);
        controlHint("Ray marcher light transmittance steps.");
    } else {
        actions.settingsChanged |= ImGui::SliderFloat("Majorant", &settings.densityMajorant, 0.05f, 25.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        controlHint("Scalar null-collision majorant multiplier for the path tracer.");
        actions.settingsChanged |= ImGui::SliderInt("Max bounces", &settings.pathMaxBounces, 1, 12);
        controlHint("Maximum number of volume scattering events per path.");
    }

    ImGui::End();
    return actions;
}

} // namespace cloud_render
