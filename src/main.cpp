#define GLFW_EXPOSE_NATIVE_WIN32

#include "core.hpp"
#include "renderer.hpp"
#include "ui.hpp"
#include "volume_io.hpp"

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_glfw.h>
#include <openvdb/openvdb.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_render {
namespace {

class OpenVdbRuntime {
public:
    OpenVdbRuntime()
    {
        openvdb::initialize();
    }

    ~OpenVdbRuntime()
    {
        openvdb::uninitialize();
    }

    OpenVdbRuntime(const OpenVdbRuntime&) = delete;
    OpenVdbRuntime& operator=(const OpenVdbRuntime&) = delete;
};

class GlfwRuntime {
public:
    GlfwRuntime()
    {
        if (!glfwInit()) {
            throw std::runtime_error("glfwInit failed");
        }
    }

    ~GlfwRuntime()
    {
        glfwTerminate();
    }

    GlfwRuntime(const GlfwRuntime&) = delete;
    GlfwRuntime& operator=(const GlfwRuntime&) = delete;
};

void copyPathToUi(VolumeUiState& state, const std::filesystem::path& path)
{
    const std::string text = path.string();
    state.path.fill('\0');
    const size_t count = std::min(text.size(), state.path.size() - 1);
    std::memcpy(state.path.data(), text.data(), count);
}

bool loadVolumeIntoRenderer(
    const std::filesystem::path& path,
    D3DState& d3d,
    std::optional<Volume>& currentVolume,
    RenderSettings& settings,
    Camera& camera,
    VolumeUiState& volumeUi)
{
    try {
        Volume loadedVolume = loadVolume(path);
        setVolume(d3d, loadedVolume);
        currentVolume = std::move(loadedVolume);
        settings.maxDistanceToZero = currentVolume->maxDistanceToZero;
        camera = makeInitialCamera(currentVolume->worldMin, currentVolume->worldMax);
        volumeUi.status = "Loaded " + path.string();
        volumeUi.statusIsError = false;

        std::cout << "Loaded " << path << " grid=" << currentVolume->gridName << " bytes=" << currentVolume->handle.size()
                  << " sdf_bytes=" << currentVolume->signedDistanceHandle.size()
                  << " max_distance_to_zero=" << currentVolume->maxDistanceToZero
                  << " native_up=+" << axisName(currentVolume->nativeUpAxis) << "\n";
        return true;
    } catch (const std::exception& e) {
        volumeUi.status = e.what();
        volumeUi.statusIsError = true;
        std::cerr << e.what() << "\n";
        return false;
    }
}

} // namespace

void run(const std::optional<std::filesystem::path>& initialVolumePath, uint32_t maxFrames = 0)
{
    OpenVdbRuntime openvdbRuntime;

    GlfwRuntime glfw;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Cloud Render", nullptr, nullptr);
    if (!window) {
        throw std::runtime_error("glfwCreateWindow failed");
    }

    HWND hwnd = glfwGetWin32Window(window);
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    D3DState d3d = createD3D(
        hwnd,
        static_cast<uint32_t>(std::max(framebufferWidth, 1)),
        static_cast<uint32_t>(std::max(framebufferHeight, 1)),
        executableDirectory() / "shaders");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX11_Init(d3d.device.Get(), d3d.context.Get());

    std::optional<Volume> volume;
    VolumeUiState volumeUi;
    if (initialVolumePath) {
        copyPathToUi(volumeUi, *initialVolumePath);
    }

    Camera camera;
    RenderSettings settings;
    bool showUi = true;
    bool lastF1 = false;
    bool resetHistory = true;
    uint32_t frameIndex = 0;

    if (initialVolumePath) {
        resetHistory = loadVolumeIntoRenderer(*initialVolumePath, d3d, volume, settings, camera, volumeUi) || resetHistory;
    }

    auto previousTime = std::chrono::steady_clock::now();
    const auto startTime = previousTime;
    float smoothedFrameSeconds = 1.0f / 60.0f;

    while (!glfwWindowShouldClose(window) && (maxFrames == 0 || frameIndex < maxFrames)) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previousTime).count();
        previousTime = now;
        const float timeSeconds = std::chrono::duration<float>(now - startTime).count();
        if (dt > 0.0f) {
            const float blend = dt > 0.25f ? 1.0f : 0.08f;
            smoothedFrameSeconds += (dt - smoothedFrameSeconds) * blend;
        }

        const bool f1 = keyPressed(window, GLFW_KEY_F1);
        if (f1 && !lastF1) {
            showUi = !showUi;
            glfwSetInputMode(window, GLFW_CURSOR, showUi ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
        }
        lastF1 = f1;

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        if (width <= 0 || height <= 0) {
            continue;
        }

        if (static_cast<uint32_t>(width) != d3d.width || static_cast<uint32_t>(height) != d3d.height) {
            resize(d3d, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            resetHistory = true;
        }

        const float moveScale = volume ? volumeMoveScale(volume->worldMin, volume->worldMax) : 1.0f;
        const bool cameraMoved = updateCamera(window, camera, dt, !showUi, moveScale);
        resetHistory = resetHistory || cameraMoved;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (showUi) {
            const float frameMs = smoothedFrameSeconds * 1000.0f;
            const float fps = smoothedFrameSeconds > 1.0e-6f ? 1.0f / smoothedFrameSeconds : 0.0f;
            const UiActions uiActions = buildUi(settings, volume ? &*volume : nullptr, volumeUi, fps, frameMs);
            resetHistory = uiActions.settingsChanged || resetHistory;
            if (uiActions.loadVolumeRequested) {
                resetHistory = loadVolumeIntoRenderer(std::filesystem::path(volumeUi.path.data()), d3d, volume, settings, camera, volumeUi) || resetHistory;
            }
        }

        ImGui::Render();

        if (volume) {
            const RenderConstants constants = makeConstants(
                camera,
                settings,
                *volume,
                d3d.width,
                d3d.height,
                frameIndex,
                timeSeconds,
                resetHistory);

            dispatchRenderer(d3d, constants);
        } else {
            clearBackbuffer(d3d);
        }

        ID3D11RenderTargetView* rtv = d3d.backbufferRtv.Get();
        d3d.context->OMSetRenderTargets(1, &rtv, nullptr);
        if (showUi) {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        throwIfFailed(d3d.swapchain->Present(0, 0), "IDXGISwapChain::Present failed");

        resetHistory = false;
        ++frameIndex;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
}

void runCheck(const std::filesystem::path& volumePath)
{
    OpenVdbRuntime openvdbRuntime;
    Volume volume = loadVolume(volumePath);
    checkShaders(executableDirectory() / "shaders");
    std::cout << "OK: " << volumePath << " grid=" << volume.gridName << " nanovdb_bytes=" << volume.handle.size()
              << " sdf_nanovdb_bytes=" << volume.signedDistanceHandle.size()
              << " max_distance_to_zero=" << volume.maxDistanceToZero
              << " native_up=+" << axisName(volume.nativeUpAxis)
              << " render_bounds_min=(" << volume.worldMin.x << "," << volume.worldMin.y << "," << volume.worldMin.z << ")"
              << " render_bounds_max=(" << volume.worldMax.x << "," << volume.worldMax.y << "," << volume.worldMax.z << ")\n";
}

} // namespace cloud_render

int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--check") {
            if (argc <= 2) {
                throw std::runtime_error("--check requires a .vdb or .nvdb path");
            }
            const std::filesystem::path volumePath = argv[2];
            cloud_render::runCheck(volumePath);
        } else if (argc > 1 && std::string_view(argv[1]) == "--frames") {
            const uint32_t maxFrames = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 1u;
            const std::optional<std::filesystem::path> volumePath = argc > 3 ? std::optional<std::filesystem::path>(argv[3]) : std::nullopt;
            cloud_render::run(volumePath, maxFrames);
        } else {
            const std::optional<std::filesystem::path> volumePath = argc > 1 ? std::optional<std::filesystem::path>(argv[1]) : std::nullopt;
            cloud_render::run(volumePath);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
