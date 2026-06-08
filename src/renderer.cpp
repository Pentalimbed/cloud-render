#include "renderer.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>

namespace cloud_render {
namespace {

using Microsoft::WRL::ComPtr;

struct CloudPhaseParameters {
    float gHg = 0.0f;
    float gD = 0.0f;
    float alpha = 1.0f;
    float weightD = 0.0f;
};

CloudPhaseParameters makeCloudPhaseParameters(float diameterMicrons)
{
    const float d = std::clamp(diameterMicrons, 0.001f, 50.0f);
    CloudPhaseParameters params;

    if (d <= 0.1f) {
        params.gHg = 13.8f * d * d;
        params.gD = 1.1456f * d * std::sin(9.29044f * d);
        params.alpha = 250.0f;
        params.weightD = 0.252977f - 312.983f * std::pow(d, 4.3f);
    } else if (d < 1.5f) {
        const float logD = std::log(d);
        const float inner = ((logD - 0.238604f) * (logD + 1.00667f)) / (0.507522f - 0.15677f * logD);
        params.gHg = 0.862f - 0.143f * logD * logD;
        params.gD = 0.379685f * std::cos(1.19692f * std::cos(inner) + 1.37932f * logD + 0.0625835f) + 0.344213f;
        params.alpha = 250.0f;
        params.weightD = 0.146209f * std::cos(3.38707f * logD + 2.11193f) + 0.316072f + 0.0778917f * logD;
    } else if (d < 5.0f) {
        const float logD = std::log(d);
        params.gHg = 0.0604931f * std::log(logD) + 0.940256f;
        params.gD = 0.500411f - 0.081287f / (-2.0f * logD + std::tan(logD) + 1.27551f);
        params.alpha = 7.30354f * logD + 6.31675f;
        params.weightD = 0.026914f * (logD - std::cos(5.68947f * (std::log(logD) - 0.0292149f))) + 0.376475f;
    } else {
        params.gHg = std::exp(-0.0990567f / (d - 1.67154f));
        params.gD = std::exp(-2.20679f / (d + 3.91029f) - 0.428934f);
        params.alpha = std::exp(3.62489f - 8.29288f / (d + 5.52825f));
        params.weightD = std::exp(-0.599085f / (d - 0.641583f) - 0.665888f);
    }

    params.gHg = std::clamp(params.gHg, -0.999f, 0.999f);
    params.gD = std::clamp(params.gD, -0.999f, 0.999f);
    params.alpha = std::max(params.alpha, 0.0f);
    params.weightD = std::clamp(params.weightD, 0.0f, 1.0f);
    return params;
}

void createTexture(
    ID3D11Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    bool shaderResource,
    bool unorderedAccess,
    TextureResource& out)
{
    out = {};

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    if (shaderResource) {
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (unorderedAccess) {
        desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }

    throwIfFailed(device->CreateTexture2D(&desc, nullptr, out.texture.GetAddressOf()), "CreateTexture2D failed");

    if (shaderResource) {
        throwIfFailed(device->CreateShaderResourceView(out.texture.Get(), nullptr, out.srv.GetAddressOf()), "CreateShaderResourceView failed");
    }
    if (unorderedAccess) {
        throwIfFailed(device->CreateUnorderedAccessView(out.texture.Get(), nullptr, out.uav.GetAddressOf()), "CreateUnorderedAccessView failed");
    }
}

void createBackbufferTargets(D3DState& d3d)
{
    d3d.backbufferRtv.Reset();
    d3d.backbuffer.Reset();
    throwIfFailed(d3d.swapchain->GetBuffer(0, IID_PPV_ARGS(d3d.backbuffer.GetAddressOf())), "IDXGISwapChain::GetBuffer failed");
    throwIfFailed(d3d.device->CreateRenderTargetView(d3d.backbuffer.Get(), nullptr, d3d.backbufferRtv.GetAddressOf()), "CreateRenderTargetView failed");
}

void createFrameResources(D3DState& d3d, uint32_t width, uint32_t height)
{
    d3d.width = width;
    d3d.height = height;

    createTexture(d3d.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true, d3d.renderTexture);
    createTexture(d3d.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true, d3d.denoiseTexture);
    createTexture(d3d.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true, d3d.historyTextures[0]);
    createTexture(d3d.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true, true, d3d.historyTextures[1]);
    createTexture(d3d.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false, true, d3d.displayTexture);
    d3d.historyIndex = 0;
}

ComPtr<ID3DBlob> compileShaderBytecode(const std::filesystem::path& path, const char* entry)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompileFromFile(
        path.wstring().c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,
        "cs_5_0",
        flags,
        0,
        bytecode.GetAddressOf(),
        errors.GetAddressOf());

    if (FAILED(hr)) {
        std::string message = "D3DCompileFromFile failed for " + path.string();
        if (errors) {
            message += "\n";
            message.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }

    return bytecode;
}

ComPtr<ID3D11ComputeShader> compileComputeShader(ID3D11Device* device, const std::filesystem::path& path, const char* entry)
{
    ComPtr<ID3DBlob> bytecode = compileShaderBytecode(path, entry);
    ComPtr<ID3D11ComputeShader> shader;
    throwIfFailed(
        device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.GetAddressOf()),
        "CreateComputeShader failed for " + path.string());
    return shader;
}

Shaders loadShaders(ID3D11Device* device, const std::filesystem::path& shaderDir)
{
    Shaders shaders;
    shaders.render = compileComputeShader(device, shaderDir / "renderVolume.hlsl", "renderVolume");
    shaders.denoise = compileComputeShader(device, shaderDir / "temporalDenoise.hlsl", "temporalDenoise");
    shaders.tonemap = compileComputeShader(device, shaderDir / "tonemap.hlsl", "tonemap");
    return shaders;
}

void createVolumeBuffer(D3DState& d3d, const Volume& volume)
{
    const uint64_t byteSize = volume.handle.size();
    if (byteSize == 0 || !volume.handle.data()) {
        throw std::runtime_error("NanoVDB handle is empty");
    }

    const uint32_t wordCount = static_cast<uint32_t>((byteSize + 3u) / 4u);
    std::vector<uint32_t> words(wordCount, 0u);
    std::memcpy(words.data(), volume.handle.data(), static_cast<size_t>(byteSize));

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = wordCount * sizeof(uint32_t);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(uint32_t);

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = words.data();

    ComPtr<ID3D11Buffer> volumeBuffer;
    throwIfFailed(d3d.device->CreateBuffer(&desc, &data, volumeBuffer.GetAddressOf()), "CreateBuffer failed for NanoVDB payload");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = wordCount;
    ComPtr<ID3D11ShaderResourceView> volumeSrv;
    throwIfFailed(d3d.device->CreateShaderResourceView(volumeBuffer.Get(), &srvDesc, volumeSrv.GetAddressOf()), "CreateShaderResourceView failed for NanoVDB payload");

    d3d.volumeBuffer = std::move(volumeBuffer);
    d3d.volumeSrv = std::move(volumeSrv);
}

void unbindComputeResources(ID3D11DeviceContext* context)
{
    std::array<ID3D11ShaderResourceView*, 4> nullSrvs = {};
    std::array<ID3D11UnorderedAccessView*, 4> nullUavs = {};
    context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()), nullUavs.data(), nullptr);
}

void dispatch2D(ID3D11DeviceContext* context, uint32_t width, uint32_t height)
{
    context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
}

} // namespace

std::filesystem::path executableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = 0;
    while (true) {
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (size < buffer.size() - 1) {
            buffer.resize(size);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

void throwIfFailed(HRESULT hr, std::string_view message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(message) + " HRESULT=0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
}

Vec3 Camera::forward() const
{
    const float cp = std::cos(pitch);
    return normalize({std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch)});
}

Vec3 Camera::right() const
{
    Vec3 rightVector = cross(forward(), kWorldUp);
    if (length(rightVector) <= 1.0e-8f) {
        rightVector = {1.0f, 0.0f, 0.0f};
    }
    return normalize(rightVector);
}

Vec3 Camera::up() const
{
    return normalize(cross(right(), forward()));
}

Camera makeInitialCamera(Vec3 worldMin, Vec3 worldMax)
{
    const Vec3 extent = worldMax - worldMin;
    const float horizontalRadius = std::max(std::max(extent.x, extent.y) * 0.5f, 1.0f);
    const Vec3 target = {0.0f, horizontalRadius * 0.35f, std::max(extent.z * 0.45f, 1.0f)};
    const Vec3 dir = normalize(target);

    Camera camera;
    camera.position = {0.0f, 0.0f, 0.0f};
    camera.pitch = std::asin(std::clamp(dir.z, -0.99f, 0.99f));
    camera.yaw = std::atan2(dir.x, dir.y);
    return camera;
}

bool keyPressed(GLFWwindow* window, int key)
{
    return glfwGetKey(window, key) == GLFW_PRESS;
}

bool updateCamera(GLFWwindow* window, Camera& camera, float dt, bool flyEnabled, float moveScale)
{
    static bool wasFlyEnabled = false;
    static double lastX = 0.0;
    static double lastY = 0.0;

    if (!flyEnabled) {
        wasFlyEnabled = false;
        return false;
    }

    bool moved = false;
    const float baseSpeed = keyPressed(window, GLFW_KEY_LEFT_SHIFT) ? moveScale * 1.2f : moveScale * 0.25f;
    const float speed = baseSpeed * dt;

    Vec3 delta = {};
    if (keyPressed(window, GLFW_KEY_W)) {
        delta = delta + camera.forward();
    }
    if (keyPressed(window, GLFW_KEY_S)) {
        delta = delta - camera.forward();
    }
    if (keyPressed(window, GLFW_KEY_D)) {
        delta = delta + camera.right();
    }
    if (keyPressed(window, GLFW_KEY_A)) {
        delta = delta - camera.right();
    }
    if (keyPressed(window, GLFW_KEY_E)) {
        delta = delta + kWorldUp;
    }
    if (keyPressed(window, GLFW_KEY_Q)) {
        delta = delta - kWorldUp;
    }

    if (length(delta) > 0.0f) {
        camera.position = camera.position + normalize(delta) * speed;
        moved = true;
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window, &x, &y);
    if (!wasFlyEnabled) {
        lastX = x;
        lastY = y;
        wasFlyEnabled = true;
        return moved;
    }

    const double dx = x - lastX;
    const double dy = y - lastY;
    lastX = x;
    lastY = y;

    if (std::abs(dx) > 0.0 || std::abs(dy) > 0.0) {
        constexpr float sensitivity = 0.0022f;
        camera.yaw += static_cast<float>(dx) * sensitivity;
        camera.pitch -= static_cast<float>(dy) * sensitivity;
        camera.pitch = std::clamp(camera.pitch, -1.52f, 1.52f);
        moved = true;
    }

    return moved;
}

D3DState createD3D(HWND hwnd, uint32_t width, uint32_t height, const std::filesystem::path& shaderDir)
{
    D3DState d3d;

    DXGI_SWAP_CHAIN_DESC swapDesc = {};
    swapDesc.BufferDesc.Width = width;
    swapDesc.BufferDesc.Height = height;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.OutputWindow = hwnd;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requestedLevels,
        static_cast<UINT>(std::size(requestedLevels)),
        D3D11_SDK_VERSION,
        &swapDesc,
        d3d.swapchain.GetAddressOf(),
        d3d.device.GetAddressOf(),
        &createdLevel,
        d3d.context.GetAddressOf());
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requestedLevels + 1,
            1,
            D3D11_SDK_VERSION,
            &swapDesc,
            d3d.swapchain.GetAddressOf(),
            d3d.device.GetAddressOf(),
            &createdLevel,
            d3d.context.GetAddressOf());
    }
    throwIfFailed(hr, "D3D11CreateDeviceAndSwapChain failed");

    createBackbufferTargets(d3d);
    createFrameResources(d3d, width, height);

    D3D11_BUFFER_DESC constantDesc = {};
    constantDesc.ByteWidth = sizeof(RenderConstants);
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    throwIfFailed(d3d.device->CreateBuffer(&constantDesc, nullptr, d3d.constantsBuffer.GetAddressOf()), "CreateBuffer failed for constants");

    d3d.shaders = loadShaders(d3d.device.Get(), shaderDir);
    return d3d;
}

void resize(D3DState& d3d, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == d3d.width && height == d3d.height)) {
        return;
    }

    d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
    d3d.backbufferRtv.Reset();
    d3d.backbuffer.Reset();
    d3d.renderTexture = {};
    d3d.denoiseTexture = {};
    d3d.historyTextures[0] = {};
    d3d.historyTextures[1] = {};
    d3d.displayTexture = {};

    throwIfFailed(d3d.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers failed");
    createBackbufferTargets(d3d);
    createFrameResources(d3d, width, height);
}

void checkShaders(const std::filesystem::path& shaderDir)
{
    compileShaderBytecode(shaderDir / "renderVolume.hlsl", "renderVolume");
    compileShaderBytecode(shaderDir / "temporalDenoise.hlsl", "temporalDenoise");
    compileShaderBytecode(shaderDir / "tonemap.hlsl", "tonemap");
}

void setVolume(D3DState& d3d, const Volume& volume)
{
    unbindComputeResources(d3d.context.Get());
    createVolumeBuffer(d3d, volume);
    d3d.historyIndex = 0;
}

void clearBackbuffer(D3DState& d3d)
{
    const float color[] = {0.015f, 0.016f, 0.018f, 1.0f};
    d3d.context->ClearRenderTargetView(d3d.backbufferRtv.Get(), color);
}

RenderConstants makeConstants(
    const Camera& camera,
    const RenderSettings& settings,
    const Volume& volume,
    uint32_t width,
    uint32_t height,
    uint32_t frameIndex,
    float timeSeconds,
    bool resetHistory)
{
    RenderConstants c = {};
    c.cameraPosition = camera.position;
    c.frameIndex = frameIndex;
    c.cameraForward = camera.forward();
    c.rendererMode = static_cast<uint32_t>(std::clamp(settings.rendererMode, 0, 1));
    c.cameraRight = camera.right();
    c.imageWidth = width;
    c.cameraUp = camera.up();
    c.imageHeight = height;
    c.lightDirection = normalize(settings.lightDirection);
    c.densityMultiplier = settings.densityMultiplier;
    c.lightColor = settings.lightColor;
    c.anisotropy = settings.anisotropy;
    c.absorption = settings.absorption;
    c.stepJitter = settings.stepJitter;
    c.scattering = settings.scattering;
    c.exposure = settings.exposure;
    c.volumeWorldMin = volume.worldMin;
    c.raymarchPrimaryMinStep = std::max(settings.raymarchPrimaryMinStep, 0.001f);
    c.volumeWorldMax = volume.worldMax;
    c.raymarchShadowSteps = static_cast<uint32_t>(std::max(1, settings.raymarchShadowSteps));
    c.pathMaxBounces = static_cast<uint32_t>(std::max(1, settings.pathMaxBounces));
    c.fovYRadians = camera.fovYRadians;
    c.temporalBlend = settings.temporalBlend;
    c.densityMajorant = settings.densityMajorant;
    c.timeSeconds = timeSeconds;
    c.resetHistory = resetHistory ? 1u : 0u;
    c.pathHistoryMode = static_cast<uint32_t>(std::clamp(settings.pathHistoryMode, 0, 1));
    c.phaseFunctionMode = static_cast<uint32_t>(std::clamp(settings.phaseFunctionMode, 0, 2));
    c.draineAlpha = std::max(settings.draineAlpha, 0.0f);
    const CloudPhaseParameters cloudPhase = makeCloudPhaseParameters(settings.cloudDiameterMicrons);
    c.cloudPhaseGhg = cloudPhase.gHg;
    c.cloudPhaseGd = cloudPhase.gD;
    c.cloudPhaseAlpha = cloudPhase.alpha;
    c.cloudPhaseWeight = cloudPhase.weightD;
    c.raymarchPrimaryStepScale = std::max(settings.raymarchPrimaryStepScale, 0.0f);
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    c.debugViewMode = static_cast<uint32_t>(std::clamp(settings.debugViewMode, 0, 1));
    c.debugSampleCountScale = std::max(settings.debugSampleCountScale, 1.0f);
#endif
    return c;
}

void dispatchRenderer(D3DState& d3d, const RenderConstants& constants)
{
    if (!d3d.volumeSrv) {
        clearBackbuffer(d3d);
        return;
    }

    d3d.context->UpdateSubresource(d3d.constantsBuffer.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11Buffer* cb = d3d.constantsBuffer.Get();
    d3d.context->CSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* renderSrvs[] = {d3d.volumeSrv.Get(), nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* renderUavs[] = {d3d.renderTexture.uav.Get(), nullptr, nullptr, nullptr};
    d3d.context->CSSetShader(d3d.shaders.render.Get(), nullptr, 0);
    d3d.context->CSSetShaderResources(0, 4, renderSrvs);
    d3d.context->CSSetUnorderedAccessViews(0, 4, renderUavs, nullptr);
    dispatch2D(d3d.context.Get(), d3d.width, d3d.height);
    unbindComputeResources(d3d.context.Get());

    const uint32_t nextHistoryIndex = 1u - d3d.historyIndex;
    ID3D11ShaderResourceView* denoiseSrvs[] = {
        d3d.volumeSrv.Get(),
        d3d.renderTexture.srv.Get(),
        d3d.historyTextures[d3d.historyIndex].srv.Get(),
        nullptr,
    };
    ID3D11UnorderedAccessView* denoiseUavs[] = {
        nullptr,
        d3d.denoiseTexture.uav.Get(),
        d3d.historyTextures[nextHistoryIndex].uav.Get(),
        nullptr,
    };
    d3d.context->CSSetShader(d3d.shaders.denoise.Get(), nullptr, 0);
    d3d.context->CSSetShaderResources(0, 4, denoiseSrvs);
    d3d.context->CSSetUnorderedAccessViews(0, 4, denoiseUavs, nullptr);
    dispatch2D(d3d.context.Get(), d3d.width, d3d.height);
    unbindComputeResources(d3d.context.Get());
    d3d.historyIndex = nextHistoryIndex;

    ID3D11ShaderResourceView* tonemapSrvs[] = {
        d3d.volumeSrv.Get(),
        nullptr,
        nullptr,
        d3d.denoiseTexture.srv.Get(),
    };
    ID3D11UnorderedAccessView* tonemapUavs[] = {
        nullptr,
        nullptr,
        nullptr,
        d3d.displayTexture.uav.Get(),
    };
    d3d.context->CSSetShader(d3d.shaders.tonemap.Get(), nullptr, 0);
    d3d.context->CSSetShaderResources(0, 4, tonemapSrvs);
    d3d.context->CSSetUnorderedAccessViews(0, 4, tonemapUavs, nullptr);
    dispatch2D(d3d.context.Get(), d3d.width, d3d.height);
    unbindComputeResources(d3d.context.Get());

    d3d.context->CopyResource(d3d.backbuffer.Get(), d3d.displayTexture.texture.Get());
}

} // namespace cloud_render
