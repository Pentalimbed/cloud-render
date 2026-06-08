#define GLFW_EXPOSE_NATIVE_WIN32

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_glfw.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nanovdb/NanoVDB.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>

using Microsoft::WRL::ComPtr;

namespace {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

Vec3 operator/(Vec3 a, float s)
{
    return {a.x / s, a.y / s, a.z / s};
}

float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float length(Vec3 v)
{
    return std::sqrt(dot(v, v));
}

Vec3 normalize(Vec3 v)
{
    const float len = length(v);
    if (len <= 1.0e-8f) {
        return {0.0f, 1.0f, 0.0f};
    }
    return v / len;
}

float maxComponent(Vec3 v)
{
    return std::max({v.x, v.y, v.z});
}

Vec3 lerp(Vec3 a, Vec3 b, float t)
{
    return a * (1.0f - t) + b * t;
}

float volumeMoveScale(Vec3 worldMin, Vec3 worldMax)
{
    const Vec3 extent = worldMax - worldMin;
    return std::max(maxComponent(extent), 1.0f);
}

void throwIfFailed(HRESULT hr, std::string_view message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(message) + " HRESULT=0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
}

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open " + path.string());
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("File is empty: " + path.string());
    }

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) {
        throw std::runtime_error("Failed to read " + path.string());
    }
    return bytes;
}

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

std::string lowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

struct Volume {
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    Vec3 worldMin;
    Vec3 worldMax;
    std::string gridName;
};

template <typename GridT>
Vec3 toVec3(const GridT& v)
{
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

Volume loadNvdb(const std::filesystem::path& path)
{
    Volume volume;
    volume.handle = nanovdb::io::readGrid(path.string());
    const auto* grid = volume.handle.grid<float>();
    if (!grid) {
        throw std::runtime_error(".nvdb file does not contain a float NanoVDB grid: " + path.string());
    }

    const auto bbox = grid->worldBBox();
    volume.worldMin = toVec3(bbox.min());
    volume.worldMax = toVec3(bbox.max());
    volume.gridName = grid->gridName();
    return volume;
}

Volume loadVdb(const std::filesystem::path& path)
{
    openvdb::io::File file(path.string());
    file.open();

    openvdb::GridBase::Ptr selectedBase;
    std::string selectedName;
    for (openvdb::io::File::NameIterator iter = file.beginName(); iter != file.endName(); ++iter) {
        openvdb::GridBase::Ptr base = file.readGrid(iter.gridName());
        if (base && base->isType<openvdb::FloatGrid>()) {
            selectedBase = std::move(base);
            selectedName = iter.gridName();
            break;
        }
    }
    file.close();

    if (!selectedBase) {
        throw std::runtime_error("No float grid found in " + path.string());
    }

    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(selectedBase);
    if (grid->getGridClass() == openvdb::GRID_UNKNOWN) {
        grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    }

    openvdb::CoordBBox activeBBox;
    if (!grid->tree().evalActiveVoxelBoundingBox(activeBBox)) {
        throw std::runtime_error("Selected VDB grid has no active voxels: " + selectedName);
    }

    const openvdb::Vec3d worldMin = grid->transform().indexToWorld(activeBBox.min());
    openvdb::Coord maxCoord = activeBBox.max();
    maxCoord.offsetBy(1);
    const openvdb::Vec3d worldMax = grid->transform().indexToWorld(maxCoord);

    Volume volume;
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }
    volume.worldMin = toVec3(worldMin);
    volume.worldMax = toVec3(worldMax);
    volume.gridName = selectedName;
    return volume;
}

Volume loadVolume(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Volume file does not exist: " + path.string());
    }

    const std::string ext = lowerExtension(path);
    if (ext == ".nvdb") {
        return loadNvdb(path);
    }
    if (ext == ".vdb") {
        return loadVdb(path);
    }
    throw std::runtime_error("Expected .vdb or .nvdb file: " + path.string());
}

struct Camera {
    Vec3 position;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovYRadians = 50.0f * 3.14159265358979323846f / 180.0f;

    Vec3 forward() const
    {
        const float cp = std::cos(pitch);
        return normalize({std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp});
    }

    Vec3 right() const
    {
        return normalize(cross({0.0f, 1.0f, 0.0f}, forward()));
    }

    Vec3 up() const
    {
        return normalize(cross(forward(), right()));
    }
};

Camera makeInitialCamera(Vec3 worldMin, Vec3 worldMax)
{
    const Vec3 center = (worldMin + worldMax) * 0.5f;
    const Vec3 extent = worldMax - worldMin;
    const float radius = std::max(maxComponent(extent) * 0.5f, 1.0f);
    const Vec3 position = center + Vec3{0.0f, radius * 0.35f, -radius * 2.2f};
    const Vec3 dir = normalize(center - position);

    Camera camera;
    camera.position = position;
    camera.pitch = std::asin(std::clamp(dir.y, -0.99f, 0.99f));
    camera.yaw = std::atan2(dir.x, dir.z);
    return camera;
}

struct RenderSettings {
    int rendererMode = 0;
    float densityMultiplier = 1.0f;
    Vec3 absorption = {0.08f, 0.08f, 0.08f};
    Vec3 scattering = {0.65f, 0.70f, 0.78f};
    Vec3 lightDirection = {-0.35f, 0.8f, 0.3f};
    Vec3 lightColor = {5.0f, 4.85f, 4.55f};
    float anisotropy = 0.15f;
    float exposure = 1.0f;
    float temporalBlend = 0.88f;
    float stepJitter = 1.0f;
    float densityMajorant = 1.0f;
    int pathHistoryMode = 0;
    int raymarchSteps = 160;
    int shadowSteps = 48;
    int pathSamples = 1;
    int pathDepth = 4;
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
    float anisotropy;

    Vec3 absorption;
    float stepJitter;

    Vec3 scattering;
    float exposure;

    Vec3 volumeWorldMin;
    uint32_t raymarchSteps;

    Vec3 volumeWorldMax;
    uint32_t shadowSteps;

    uint32_t pathSamples;
    uint32_t pathDepth;
    float fovYRadians;
    float temporalBlend;

    float densityMajorant;
    float timeSeconds;
    uint32_t resetHistory;
    uint32_t pathHistoryMode;
};

static_assert(sizeof(RenderConstants) % 16 == 0);

struct TextureResource {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Shaders {
    ComPtr<ID3D11ComputeShader> render;
    ComPtr<ID3D11ComputeShader> denoise;
    ComPtr<ID3D11ComputeShader> tonemap;
};

struct D3DState {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    ComPtr<ID3D11RenderTargetView> backbufferRtv;
    ComPtr<ID3D11Texture2D> backbuffer;

    TextureResource renderTexture;
    TextureResource denoiseTexture;
    TextureResource historyTextures[2];
    TextureResource displayTexture;

    ComPtr<ID3D11Buffer> constantsBuffer;
    ComPtr<ID3D11Buffer> volumeBuffer;
    ComPtr<ID3D11ShaderResourceView> volumeSrv;

    Shaders shaders;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t historyIndex = 0;
};

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

ComPtr<ID3DBlob> compileShaderBytecode(const std::filesystem::path& path, const char* entry)
{
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_SKIP_OPTIMIZATION;
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

void checkShaders(const std::filesystem::path& shaderDir)
{
    compileShaderBytecode(shaderDir / "renderVolume.hlsl", "renderVolume");
    compileShaderBytecode(shaderDir / "temporalDenoise.hlsl", "temporalDenoise");
    compileShaderBytecode(shaderDir / "tonemap.hlsl", "tonemap");
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

    throwIfFailed(d3d.device->CreateBuffer(&desc, &data, d3d.volumeBuffer.GetAddressOf()), "CreateBuffer failed for NanoVDB payload");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = wordCount;
    throwIfFailed(d3d.device->CreateShaderResourceView(d3d.volumeBuffer.Get(), &srvDesc, d3d.volumeSrv.GetAddressOf()), "CreateShaderResourceView failed for NanoVDB payload");
}

D3DState createD3D(HWND hwnd, uint32_t width, uint32_t height, const Volume& volume, const std::filesystem::path& shaderDir)
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

    createVolumeBuffer(d3d, volume);
    d3d.shaders = loadShaders(d3d.device.Get(), shaderDir);
    return d3d;
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

void dispatchRenderer(D3DState& d3d, const RenderConstants& constants)
{
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
        delta = delta + Vec3{0.0f, 1.0f, 0.0f};
    }
    if (keyPressed(window, GLFW_KEY_Q)) {
        delta = delta - Vec3{0.0f, 1.0f, 0.0f};
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

bool buildUi(RenderSettings& settings, const Volume& volume)
{
    bool changed = false;
    ImGui::Begin("Cloud Renderer");
    ImGui::TextUnformatted(volume.gridName.c_str());
    ImGui::Separator();

    const char* modes[] = {"Ray marching", "Path tracer"};
    changed |= ImGui::Combo("Renderer", &settings.rendererMode, modes, 2);
    if (settings.rendererMode == 1) {
        const char* historyModes[] = {"Temporal denoiser", "Accumulation"};
        changed |= ImGui::Combo("Path history", &settings.pathHistoryMode, historyModes, 2);
    }
    changed |= ImGui::SliderFloat("Density", &settings.densityMultiplier, 0.0f, 20.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    changed |= controlVec3Color("Absorption", settings.absorption);
    changed |= controlVec3Color("Scattering", settings.scattering);
    changed |= controlVec3Drag("Light direction", settings.lightDirection, 0.01f, -1.0f, 1.0f);
    settings.lightDirection = normalize(settings.lightDirection);
    changed |= controlVec3Color("Light color", settings.lightColor);
    changed |= ImGui::SliderFloat("Anisotropy", &settings.anisotropy, -0.95f, 0.95f, "%.2f");
    changed |= ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Temporal blend", &settings.temporalBlend, 0.0f, 0.98f, "%.2f");
    changed |= ImGui::SliderFloat("Step jitter", &settings.stepJitter, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Majorant", &settings.densityMajorant, 0.05f, 25.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderInt("Ray steps", &settings.raymarchSteps, 8, 1024);
    changed |= ImGui::SliderInt("Shadow steps", &settings.shadowSteps, 4, 256);
    changed |= ImGui::SliderInt("Samples", &settings.pathSamples, 1, 16);
    changed |= ImGui::SliderInt("Path depth", &settings.pathDepth, 1, 12);

    ImGui::End();
    return changed;
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
    c.raymarchSteps = static_cast<uint32_t>(std::max(1, settings.raymarchSteps));
    c.volumeWorldMax = volume.worldMax;
    c.shadowSteps = static_cast<uint32_t>(std::max(1, settings.shadowSteps));
    c.pathSamples = static_cast<uint32_t>(std::max(1, settings.pathSamples));
    c.pathDepth = static_cast<uint32_t>(std::max(1, settings.pathDepth));
    c.fovYRadians = camera.fovYRadians;
    c.temporalBlend = settings.temporalBlend;
    c.densityMajorant = settings.densityMajorant;
    c.timeSeconds = timeSeconds;
    c.resetHistory = resetHistory ? 1u : 0u;
    c.pathHistoryMode = static_cast<uint32_t>(std::clamp(settings.pathHistoryMode, 0, 1));
    return c;
}

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

void run(const std::filesystem::path& volumePath, uint32_t maxFrames = 0)
{
    openvdb::initialize();
    Volume volume = loadVolume(volumePath);
    std::cout << "Loaded " << volumePath << " grid=" << volume.gridName << " bytes=" << volume.handle.size() << "\n";

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
        volume,
        executableDirectory() / "shaders");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplDX11_Init(d3d.device.Get(), d3d.context.Get());

    Camera camera = makeInitialCamera(volume.worldMin, volume.worldMax);
    RenderSettings settings;
    bool showUi = true;
    bool lastF1 = false;
    bool resetHistory = true;
    uint32_t frameIndex = 0;

    auto previousTime = std::chrono::steady_clock::now();
    const auto startTime = previousTime;

    while (!glfwWindowShouldClose(window) && (maxFrames == 0 || frameIndex < maxFrames)) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previousTime).count();
        previousTime = now;
        const float timeSeconds = std::chrono::duration<float>(now - startTime).count();

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

        const bool cameraMoved = updateCamera(window, camera, dt, !showUi, volumeMoveScale(volume.worldMin, volume.worldMax));
        resetHistory = resetHistory || cameraMoved;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (showUi) {
            resetHistory = buildUi(settings, volume) || resetHistory;
        }

        ImGui::Render();

        const RenderConstants constants = makeConstants(
            camera,
            settings,
            volume,
            d3d.width,
            d3d.height,
            frameIndex,
            timeSeconds,
            resetHistory);

        dispatchRenderer(d3d, constants);

        ID3D11RenderTargetView* rtv = d3d.backbufferRtv.Get();
        d3d.context->OMSetRenderTargets(1, &rtv, nullptr);
        if (showUi) {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        throwIfFailed(d3d.swapchain->Present(1, 0), "IDXGISwapChain::Present failed");

        resetHistory = false;
        ++frameIndex;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    openvdb::uninitialize();
}

void runCheck(const std::filesystem::path& volumePath)
{
    openvdb::initialize();
    Volume volume = loadVolume(volumePath);
    checkShaders(executableDirectory() / "shaders");
    std::cout << "OK: " << volumePath << " grid=" << volume.gridName << " nanovdb_bytes=" << volume.handle.size() << "\n";
    openvdb::uninitialize();
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--check") {
            const std::filesystem::path volumePath = argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path("C:\\repos\\personal\\cloud-render\\cabauw.vdb");
            runCheck(volumePath);
        } else if (argc > 1 && std::string_view(argv[1]) == "--frames") {
            const uint32_t maxFrames = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 1u;
            const std::filesystem::path volumePath = argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path("C:\\repos\\personal\\cloud-render\\cabauw.vdb");
            run(volumePath, maxFrames);
        } else {
            const std::filesystem::path volumePath = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("C:\\repos\\personal\\cloud-render\\cabauw.vdb");
            run(volumePath);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
