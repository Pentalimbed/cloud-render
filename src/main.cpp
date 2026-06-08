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

#ifndef CLOUD_RENDER_ENABLE_DEBUG_VIZ
#define CLOUD_RENDER_ENABLE_DEBUG_VIZ 0
#endif

#include <nanovdb/NanoVDB.h>
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/NanoToOpenVDB.h>
#include <openvdb/io/File.h>
#include <openvdb/math/Mat4.h>
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
        return {0.0f, 0.0f, 1.0f};
    }
    return v / len;
}

constexpr Vec3 kWorldUp = {0.0f, 0.0f, 1.0f};

float maxComponent(Vec3 v)
{
    return std::max({v.x, v.y, v.z});
}

float component(Vec3 v, int axis)
{
    switch (axis) {
    case 0:
        return v.x;
    case 1:
        return v.y;
    default:
        return v.z;
    }
}

void setComponent(Vec3& v, int axis, float value)
{
    switch (axis) {
    case 0:
        v.x = value;
        break;
    case 1:
        v.y = value;
        break;
    default:
        v.z = value;
        break;
    }
}

const char* axisName(int axis)
{
    switch (axis) {
    case 0:
        return "X";
    case 1:
        return "Y";
    default:
        return "Z";
    }
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
    Vec3 nativeWorldMin;
    Vec3 nativeWorldMax;
    Vec3 worldMin;
    Vec3 worldMax;
    int nativeUpAxis = 2;
    std::string gridName;
};

struct Bounds {
    Vec3 min;
    Vec3 max;
};

struct VolumePlacement {
    Vec3 nativeWorldMin;
    Vec3 nativeWorldMax;
    Vec3 worldMin;
    Vec3 worldMax;
    openvdb::math::Mat4d nativeToRender = openvdb::math::Mat4d::identity();
    int nativeUpAxis = 2;
};

template <typename GridT>
Vec3 toVec3(const GridT& v)
{
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

void includePoint(Bounds& bounds, Vec3 point)
{
    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.min.z = std::min(bounds.min.z, point.z);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
    bounds.max.z = std::max(bounds.max.z, point.z);
}

Bounds activeWorldBounds(const openvdb::FloatGrid& grid, const openvdb::CoordBBox& activeBBox)
{
    openvdb::Coord minCoord = activeBBox.min();
    openvdb::Coord maxCoord = activeBBox.max();
    maxCoord.offsetBy(1);

    const Vec3 first = toVec3(grid.transform().indexToWorld(minCoord));
    Bounds bounds{first, first};
    const int xs[] = {minCoord.x(), maxCoord.x()};
    const int ys[] = {minCoord.y(), maxCoord.y()};
    const int zs[] = {minCoord.z(), maxCoord.z()};

    for (int x : xs) {
        for (int y : ys) {
            for (int z : zs) {
                includePoint(bounds, toVec3(grid.transform().indexToWorld(openvdb::Coord(x, y, z))));
            }
        }
    }

    return bounds;
}

int shortestAxis(Vec3 extent)
{
    int axis = 0;
    if (extent.y < component(extent, axis)) {
        axis = 1;
    }
    if (extent.z < component(extent, axis)) {
        axis = 2;
    }
    return axis;
}

VolumePlacement makeVolumePlacement(Vec3 nativeWorldMin, Vec3 nativeWorldMax)
{
    const Vec3 nativeExtent = nativeWorldMax - nativeWorldMin;
    const int upAxis = shortestAxis(nativeExtent);
    const int renderXAxis = (upAxis + 2) % 3;
    const int renderYAxis = (upAxis + 1) % 3;

    Vec3 nativeOrigin = (nativeWorldMin + nativeWorldMax) * 0.5f;
    setComponent(nativeOrigin, upAxis, component(nativeWorldMin, upAxis));

    const Vec3 renderExtent = {
        component(nativeExtent, renderXAxis),
        component(nativeExtent, renderYAxis),
        component(nativeExtent, upAxis),
    };

    openvdb::math::Mat4d nativeToRender = openvdb::math::Mat4d::zero();
    nativeToRender[renderXAxis][0] = 1.0;
    nativeToRender[renderYAxis][1] = 1.0;
    nativeToRender[upAxis][2] = 1.0;
    nativeToRender[3][0] = -static_cast<double>(component(nativeOrigin, renderXAxis));
    nativeToRender[3][1] = -static_cast<double>(component(nativeOrigin, renderYAxis));
    nativeToRender[3][2] = -static_cast<double>(component(nativeOrigin, upAxis));
    nativeToRender[3][3] = 1.0;

    VolumePlacement placement;
    placement.nativeWorldMin = nativeWorldMin;
    placement.nativeWorldMax = nativeWorldMax;
    placement.worldMin = {-renderExtent.x * 0.5f, -renderExtent.y * 0.5f, 0.0f};
    placement.worldMax = {renderExtent.x * 0.5f, renderExtent.y * 0.5f, renderExtent.z};
    placement.nativeToRender = nativeToRender;
    placement.nativeUpAxis = upAxis;
    return placement;
}

void applyVolumePlacement(openvdb::FloatGrid& grid, Volume& volume, const openvdb::CoordBBox& activeBBox)
{
    if (!grid.transform().isLinear()) {
        throw std::runtime_error("Selected VDB grid must have a linear transform");
    }

    const Bounds nativeWorldBounds = activeWorldBounds(grid, activeBBox);
    const VolumePlacement placement = makeVolumePlacement(nativeWorldBounds.min, nativeWorldBounds.max);
    const auto oldIndexToNative = grid.transform().baseMap()->getAffineMap()->getMat4();
    grid.setTransform(openvdb::math::Transform::createLinearTransform(oldIndexToNative * placement.nativeToRender));

    const Bounds renderWorldBounds = activeWorldBounds(grid, activeBBox);
    volume.nativeWorldMin = placement.nativeWorldMin;
    volume.nativeWorldMax = placement.nativeWorldMax;
    volume.worldMin = renderWorldBounds.min;
    volume.worldMax = renderWorldBounds.max;
    volume.nativeUpAxis = placement.nativeUpAxis;
}

Volume loadNvdb(const std::filesystem::path& path)
{
    auto nanoHandle = nanovdb::io::readGrid(path.string());
    const auto* nanoGrid = nanoHandle.grid<float>();
    if (!nanoGrid) {
        throw std::runtime_error(".nvdb file does not contain a float NanoVDB grid: " + path.string());
    }

    openvdb::GridBase::Ptr base = nanovdb::tools::nanoToOpenVDB(nanoHandle);
    if (!base || !base->isType<openvdb::FloatGrid>()) {
        throw std::runtime_error("NanoVDB to OpenVDB conversion did not produce a float grid: " + path.string());
    }

    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
    if (grid->getGridClass() == openvdb::GRID_UNKNOWN) {
        grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    }

    openvdb::CoordBBox activeBBox;
    if (!grid->tree().evalActiveVoxelBoundingBox(activeBBox)) {
        throw std::runtime_error("Selected NanoVDB grid has no active voxels: " + std::string(nanoGrid->gridName()));
    }

    Volume volume;
    applyVolumePlacement(*grid, volume, activeBBox);
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }
    volume.gridName = nanoGrid->gridName();
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

    Volume volume;
    applyVolumePlacement(*grid, volume, activeBBox);
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }
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
        return normalize({std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch)});
    }

    Vec3 right() const
    {
        Vec3 rightVector = cross(forward(), kWorldUp);
        if (length(rightVector) <= 1.0e-8f) {
            rightVector = {1.0f, 0.0f, 0.0f};
        }
        return normalize(rightVector);
    }

    Vec3 up() const
    {
        return normalize(cross(right(), forward()));
    }
};

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

struct RenderSettings {
    int rendererMode = 0;
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    int debugViewMode = 0;
#endif
    float densityMultiplier = 1.0f;
    Vec3 absorption = {0.08f, 0.08f, 0.08f};
    Vec3 scattering = {0.65f, 0.70f, 0.78f};
    Vec3 lightDirection = {-0.35f, 0.3f, 0.8f};
    Vec3 lightColor = {5.0f, 4.85f, 4.55f};
    int phaseFunctionMode = 0;
    float anisotropy = 0.15f;
    float draineAlpha = 1.0f;
    float cloudDiameterMicrons = 20.0f;
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
    float raymarchPrimaryMinStep;

    Vec3 volumeWorldMax;
    uint32_t raymarchShadowSteps;

    uint32_t pathMaxBounces;
    float fovYRadians;
    float temporalBlend;
    float densityMajorant;

    float timeSeconds;
    uint32_t resetHistory;
    uint32_t pathHistoryMode;
    uint32_t phaseFunctionMode;

    float draineAlpha;
    float cloudPhaseGhg;
    float cloudPhaseGd;
    float cloudPhaseAlpha;

    float cloudPhaseWeight;
    float raymarchPrimaryStepScale;
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    uint32_t debugViewMode;
    float debugSampleCountScale;
#else
    float _phasePad0;
    float _phasePad1;
#endif
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

bool buildUi(RenderSettings& settings, const Volume& volume, float fps, float frameMs)
{
    bool changed = false;
    ImGui::Begin("Cloud Renderer");
    ImGui::TextUnformatted(volume.gridName.c_str());
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
    const char* phaseModes[] = {"Henyey-Greenstein", "Draine", "Jendersie cloud"};
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
    std::cout << "Loaded " << volumePath << " grid=" << volume.gridName << " bytes=" << volume.handle.size()
              << " native_up=+" << axisName(volume.nativeUpAxis) << "\n";

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

        const bool cameraMoved = updateCamera(window, camera, dt, !showUi, volumeMoveScale(volume.worldMin, volume.worldMax));
        resetHistory = resetHistory || cameraMoved;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (showUi) {
            const float frameMs = smoothedFrameSeconds * 1000.0f;
            const float fps = smoothedFrameSeconds > 1.0e-6f ? 1.0f / smoothedFrameSeconds : 0.0f;
            resetHistory = buildUi(settings, volume, fps, frameMs) || resetHistory;
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

        throwIfFailed(d3d.swapchain->Present(0, 0), "IDXGISwapChain::Present failed");

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
    std::cout << "OK: " << volumePath << " grid=" << volume.gridName << " nanovdb_bytes=" << volume.handle.size()
              << " native_up=+" << axisName(volume.nativeUpAxis)
              << " render_bounds_min=(" << volume.worldMin.x << "," << volume.worldMin.y << "," << volume.worldMin.z << ")"
              << " render_bounds_max=(" << volume.worldMax.x << "," << volume.worldMax.y << "," << volume.worldMax.z << ")\n";
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
