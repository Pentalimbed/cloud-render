#pragma once

#include "core.hpp"
#include "volume_io.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#ifndef CLOUD_RENDER_ENABLE_DEBUG_VIZ
#define CLOUD_RENDER_ENABLE_DEBUG_VIZ 0
#endif

struct GLFWwindow;

namespace cloud_render {

struct Camera {
    Vec3 position = {0.0f, 0.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovYRadians = 50.0f * 3.14159265358979323846f / 180.0f;

    Vec3 forward() const;
    Vec3 right() const;
    Vec3 up() const;
};

struct RenderSettings {
    int rendererMode = 0;
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    int debugViewMode = 0;
#endif
    float densityMultiplier = 100.0f;
    Vec3 absorption = {0.08f, 0.08f, 0.08f};
    Vec3 scattering = {0.65f, 0.70f, 0.78f};
    Vec3 lightDirection = {-0.35f, 0.3f, 0.8f};
    Vec3 lightColor = {5.0f, 4.85f, 4.55f};
    int phaseFunctionMode = 2;
    float cloudPhaseGhg = 0.15f;
    float cloudPhaseAlpha = 1.0f;
    float particleDiameterMicrons = 20.0f;
    float exposure = 1.0f;
    float temporalBlend = 0.88f;
    float stepJitter = 1.0f;
    float densityMajorant = 1.0f;
    int pathHistoryMode = 0;
    float raymarchPrimaryMinStep = 1.0f;
    float raymarchPrimaryStepScale = 0.3f;
    int raymarchShadowSteps = 16;
    int pathMaxBounces = 8;
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    float debugSampleCountScale = 256.0f;
#endif
};

struct RenderConstants {
    Vec3 cameraPosition;
    uint32_t frameIndex;

    Vec3 cameraForward;
    uint32_t rendererMode;

    Vec3 cameraRight;
    uint32_t imageWidth;

    Vec3 cameraUp;
    uint32_t imageHeight;

    Vec3 lightDirection;
    float densityMultiplier;

    Vec3 lightColor;
    float exposure;

    Vec3 absorption;
    float stepJitter;

    Vec3 scattering;
    float densityMajorant;

    Vec3 volumeWorldMin;
    float raymarchPrimaryMinStep;

    Vec3 volumeWorldMax;
    uint32_t raymarchShadowSteps;

    uint32_t pathMaxBounces;
    float fovYRadians;
    float temporalBlend;
    float raymarchPrimaryStepScale;

    float timeSeconds;
    uint32_t resetHistory;
    uint32_t pathHistoryMode;
    uint32_t phaseFunctionMode;

    float cloudPhaseGhg;
    float cloudPhaseGd;
    float cloudPhaseAlpha;
    float cloudPhaseWeight;

#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    uint32_t debugViewMode;
    float debugSampleCountScale;
    float debugMaxDistanceToZero;
    float _debugPad1;
#endif
};

static_assert(sizeof(RenderConstants) % 16 == 0);

struct TextureResource {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Shaders {
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> render;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> denoise;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> tonemap;
};

struct D3DState {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapchain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backbufferRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;

    TextureResource renderTexture;
    TextureResource denoiseTexture;
    TextureResource historyTextures[2];
    TextureResource displayTexture;

    Microsoft::WRL::ComPtr<ID3D11Buffer> constantsBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> volumeBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> volumeSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> signedDistanceBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> signedDistanceSrv;

    Shaders shaders;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t historyIndex = 0;
};

std::filesystem::path executableDirectory();
void throwIfFailed(HRESULT hr, std::string_view message);

Camera makeInitialCamera(Vec3 worldMin, Vec3 worldMax);
bool keyPressed(GLFWwindow* window, int key);
bool updateCamera(GLFWwindow* window, Camera& camera, float dt, bool flyEnabled, float moveScale);

D3DState createD3D(HWND hwnd, uint32_t width, uint32_t height, const std::filesystem::path& shaderDir);
void resize(D3DState& d3d, uint32_t width, uint32_t height);
void checkShaders(const std::filesystem::path& shaderDir);
void setVolume(D3DState& d3d, const Volume& volume);
void clearBackbuffer(D3DState& d3d);

RenderConstants makeConstants(
    const Camera& camera,
    const RenderSettings& settings,
    const Volume& volume,
    uint32_t width,
    uint32_t height,
    uint32_t frameIndex,
    float timeSeconds,
    bool resetHistory);
void dispatchRenderer(D3DState& d3d, const RenderConstants& constants);

} // namespace cloud_render
