#include "renderer.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>

namespace cloud_render {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kShadowVolumeWidth = 256;
constexpr uint32_t kShadowVolumeHeight = 256;
constexpr uint32_t kShadowVolumeDepth = 32;

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

Vec3 maxVec3(Vec3 value, float minValue)
{
    return {
        std::max(value.x, minValue),
        std::max(value.y, minValue),
        std::max(value.z, minValue),
    };
}

Vec3 saturateVec3(Vec3 value)
{
    return {
        std::clamp(value.x, 0.0f, 1.0f),
        std::clamp(value.y, 0.0f, 1.0f),
        std::clamp(value.z, 0.0f, 1.0f),
    };
}

Vec3 mulVec3(Vec3 a, Vec3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
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
    shaders.updateShadowVolume = compileComputeShader(device, shaderDir / "updateShadowVolume.hlsl", "updateShadowVolume");
    shaders.render = compileComputeShader(device, shaderDir / "renderVolume.hlsl", "renderVolume");
    shaders.denoise = compileComputeShader(device, shaderDir / "temporalDenoise.hlsl", "temporalDenoise");
    shaders.tonemap = compileComputeShader(device, shaderDir / "tonemap.hlsl", "tonemap");
    return shaders;
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, size_t offset)
{
    if (offset + 4u > bytes.size()) {
        throw std::runtime_error("Unexpected end of DDS header");
    }
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1u]) << 8u)
        | (static_cast<uint32_t>(bytes[offset + 2u]) << 16u)
        | (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open " + path.string());
    }

    const std::streamoff size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Empty file: " + path.string());
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Failed to read " + path.string());
    }
    return bytes;
}

std::filesystem::path resolveNubisNoisePath(const std::filesystem::path& shaderDir)
{
    const std::filesystem::path executableDir = shaderDir.parent_path();
    const std::filesystem::path candidates[] = {
        executableDir / "data" / "nubis.dds",
        std::filesystem::current_path() / "data" / "nubis.dds",
    };

    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    throw std::runtime_error("Could not find data/nubis.dds next to the executable or in the current working directory");
}

void createNubisNoiseTexture(D3DState& d3d, const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes = readBinaryFile(path);
    if (bytes.size() < 128u || readLe32(bytes, 0) != 0x20534444u || readLe32(bytes, 4) != 124u) {
        throw std::runtime_error("Unsupported DDS header: " + path.string());
    }

    constexpr uint32_t kDdsPixelFormatOffset = 76u;
    constexpr uint32_t kDdsDataOffset = 128u;
    constexpr uint32_t kDdsPfAlphaPixels = 0x1u;
    constexpr uint32_t kDdsPfRgb = 0x40u;
    constexpr uint32_t kDdsCaps2Volume = 0x200000u;

    const uint32_t height = readLe32(bytes, 12);
    const uint32_t width = readLe32(bytes, 16);
    const uint32_t depth = readLe32(bytes, 24);
    const uint32_t mipCount = std::max(readLe32(bytes, 28), 1u);
    const uint32_t pixelFormatSize = readLe32(bytes, kDdsPixelFormatOffset);
    const uint32_t pixelFormatFlags = readLe32(bytes, kDdsPixelFormatOffset + 4u);
    const uint32_t rgbBitCount = readLe32(bytes, kDdsPixelFormatOffset + 12u);
    const uint32_t rMask = readLe32(bytes, kDdsPixelFormatOffset + 16u);
    const uint32_t gMask = readLe32(bytes, kDdsPixelFormatOffset + 20u);
    const uint32_t bMask = readLe32(bytes, kDdsPixelFormatOffset + 24u);
    const uint32_t aMask = readLe32(bytes, kDdsPixelFormatOffset + 28u);
    const uint32_t caps2 = readLe32(bytes, 112);

    const bool isRgba8 = pixelFormatSize == 32u
        && (pixelFormatFlags & (kDdsPfRgb | kDdsPfAlphaPixels)) == (kDdsPfRgb | kDdsPfAlphaPixels)
        && rgbBitCount == 32u
        && rMask == 0x000000ffu
        && gMask == 0x0000ff00u
        && bMask == 0x00ff0000u
        && aMask == 0xff000000u;
    if (width == 0u || height == 0u || depth == 0u || (caps2 & kDdsCaps2Volume) == 0u || !isRgba8) {
        throw std::runtime_error("Expected a 3D RGBA8 DDS for Nubis noise: " + path.string());
    }

    std::vector<D3D11_SUBRESOURCE_DATA> subresources;
    subresources.reserve(mipCount);
    size_t offset = kDdsDataOffset;
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        const uint32_t mipWidth = std::max(width >> mip, 1u);
        const uint32_t mipHeight = std::max(height >> mip, 1u);
        const uint32_t mipDepth = std::max(depth >> mip, 1u);
        const size_t rowPitch = static_cast<size_t>(mipWidth) * 4u;
        const size_t slicePitch = rowPitch * mipHeight;
        const size_t mipBytes = slicePitch * mipDepth;
        if (offset + mipBytes > bytes.size()) {
            throw std::runtime_error("DDS mip data is truncated: " + path.string());
        }

        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = bytes.data() + offset;
        data.SysMemPitch = static_cast<UINT>(rowPitch);
        data.SysMemSlicePitch = static_cast<UINT>(slicePitch);
        subresources.push_back(data);
        offset += mipBytes;
    }

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = mipCount;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    throwIfFailed(
        d3d.device->CreateTexture3D(&desc, subresources.data(), d3d.nubisNoiseTexture.GetAddressOf()),
        "CreateTexture3D failed for " + path.string());

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Texture3D.MostDetailedMip = 0;
    srvDesc.Texture3D.MipLevels = mipCount;
    throwIfFailed(
        d3d.device->CreateShaderResourceView(d3d.nubisNoiseTexture.Get(), &srvDesc, d3d.nubisNoiseSrv.GetAddressOf()),
        "CreateShaderResourceView failed for " + path.string());
}

void createWrapSampler(D3DState& d3d)
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    throwIfFailed(d3d.device->CreateSamplerState(&desc, d3d.wrapSampler.GetAddressOf()), "CreateSamplerState failed");
}

void createClampSampler(D3DState& d3d)
{
    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    throwIfFailed(d3d.device->CreateSamplerState(&desc, d3d.clampSampler.GetAddressOf()), "CreateSamplerState failed");
}

void createNanoVdbBuffer(
    ID3D11Device* device,
    const nanovdb::GridHandle<nanovdb::HostBuffer>& handle,
    ComPtr<ID3D11Buffer>& outBuffer,
    ComPtr<ID3D11ShaderResourceView>& outSrv,
    std::string_view label)
{
    const uint64_t byteSize = handle.size();
    if (byteSize == 0 || !handle.data()) {
        throw std::runtime_error(std::string(label) + " NanoVDB handle is empty");
    }

    const uint32_t wordCount = static_cast<uint32_t>((byteSize + 3u) / 4u);
    std::vector<uint32_t> words(wordCount, 0u);
    std::memcpy(words.data(), handle.data(), static_cast<size_t>(byteSize));

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = wordCount * sizeof(uint32_t);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(uint32_t);

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = words.data();

    ComPtr<ID3D11Buffer> buffer;
    throwIfFailed(device->CreateBuffer(&desc, &data, buffer.GetAddressOf()), "CreateBuffer failed for " + std::string(label) + " NanoVDB payload");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = wordCount;
    ComPtr<ID3D11ShaderResourceView> srv;
    throwIfFailed(
        device->CreateShaderResourceView(buffer.Get(), &srvDesc, srv.GetAddressOf()),
        "CreateShaderResourceView failed for " + std::string(label) + " NanoVDB payload");

    outBuffer = std::move(buffer);
    outSrv = std::move(srv);
}

void createCoarseSignedDistanceTexture(D3DState& d3d, const CoarseSignedDistanceVolume& volume)
{
    d3d.coarseSignedDistanceTexture.Reset();
    d3d.coarseSignedDistanceSrv.Reset();

    const uint32_t width = volume.size[0];
    const uint32_t height = volume.size[1];
    const uint32_t depth = volume.size[2];
    const size_t expectedCount = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(depth);
    if (width == 0u || height == 0u || depth == 0u || volume.values.size() != expectedCount) {
        throw std::runtime_error("Coarse signed-distance volume data is invalid");
    }

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = volume.values.data();
    data.SysMemPitch = width * sizeof(float);
    data.SysMemSlicePitch = width * height * sizeof(float);

    throwIfFailed(
        d3d.device->CreateTexture3D(&desc, &data, d3d.coarseSignedDistanceTexture.GetAddressOf()),
        "CreateTexture3D failed for coarse signed-distance volume");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Texture3D.MostDetailedMip = 0;
    srvDesc.Texture3D.MipLevels = 1;
    throwIfFailed(
        d3d.device->CreateShaderResourceView(d3d.coarseSignedDistanceTexture.Get(), &srvDesc, d3d.coarseSignedDistanceSrv.GetAddressOf()),
        "CreateShaderResourceView failed for coarse signed-distance volume");
}

void clearShadowVolume(D3DState& d3d)
{
    if (!d3d.shadowVolumeUav) {
        return;
    }

    const float clearValue[4] = {};
    d3d.context->ClearUnorderedAccessViewFloat(d3d.shadowVolumeUav.Get(), clearValue);
}

void createShadowVolumeTexture(D3DState& d3d)
{
    d3d.shadowVolumeTexture.Reset();
    d3d.shadowVolumeSrv.Reset();
    d3d.shadowVolumeUav.Reset();

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = kShadowVolumeWidth;
    desc.Height = kShadowVolumeHeight;
    desc.Depth = kShadowVolumeDepth;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    throwIfFailed(
        d3d.device->CreateTexture3D(&desc, nullptr, d3d.shadowVolumeTexture.GetAddressOf()),
        "CreateTexture3D failed for shadow volume");

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Texture3D.MostDetailedMip = 0;
    srvDesc.Texture3D.MipLevels = 1;
    throwIfFailed(
        d3d.device->CreateShaderResourceView(d3d.shadowVolumeTexture.Get(), &srvDesc, d3d.shadowVolumeSrv.GetAddressOf()),
        "CreateShaderResourceView failed for shadow volume");

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = desc.Format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.MipSlice = 0;
    uavDesc.Texture3D.FirstWSlice = 0;
    uavDesc.Texture3D.WSize = desc.Depth;
    throwIfFailed(
        d3d.device->CreateUnorderedAccessView(d3d.shadowVolumeTexture.Get(), &uavDesc, d3d.shadowVolumeUav.GetAddressOf()),
        "CreateUnorderedAccessView failed for shadow volume");

    clearShadowVolume(d3d);
}

void createVolumeBuffers(D3DState& d3d, const Volume& volume)
{
    createNanoVdbBuffer(d3d.device.Get(), volume.handle, d3d.volumeBuffer, d3d.volumeSrv, "density");
    d3d.qCriterionBuffer.Reset();
    d3d.qCriterionSrv.Reset();
    if (volume.hasQCriterion) {
        createNanoVdbBuffer(
            d3d.device.Get(),
            volume.qCriterionHandle,
            d3d.qCriterionBuffer,
            d3d.qCriterionSrv,
            "q_criterion");
    }
    createNanoVdbBuffer(
        d3d.device.Get(),
        volume.signedDistanceHandle,
        d3d.signedDistanceBuffer,
        d3d.signedDistanceSrv,
        "signed-distance");
    createCoarseSignedDistanceTexture(d3d, volume.coarseSignedDistance);
}

void unbindComputeResources(ID3D11DeviceContext* context)
{
    std::array<ID3D11ShaderResourceView*, 9> nullSrvs = {};
    std::array<ID3D11UnorderedAccessView*, 5> nullUavs = {};
    std::array<ID3D11SamplerState*, 2> nullSamplers = {};
    context->CSSetShaderResources(0, static_cast<UINT>(nullSrvs.size()), nullSrvs.data());
    context->CSSetUnorderedAccessViews(0, static_cast<UINT>(nullUavs.size()), nullUavs.data(), nullptr);
    context->CSSetSamplers(0, static_cast<UINT>(nullSamplers.size()), nullSamplers.data());
}

void dispatch2D(ID3D11DeviceContext* context, uint32_t width, uint32_t height)
{
    context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);
}

void dispatchShadowVolume(ID3D11DeviceContext* context)
{
    context->Dispatch((kShadowVolumeWidth + 3u) / 4u, (kShadowVolumeHeight + 3u) / 4u, (kShadowVolumeDepth + 3u) / 4u);
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
    createNubisNoiseTexture(d3d, resolveNubisNoisePath(shaderDir));
    createWrapSampler(d3d);
    createClampSampler(d3d);
    createShadowVolumeTexture(d3d);

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
    compileShaderBytecode(shaderDir / "updateShadowVolume.hlsl", "updateShadowVolume");
    compileShaderBytecode(shaderDir / "renderVolume.hlsl", "renderVolume");
    compileShaderBytecode(shaderDir / "temporalDenoise.hlsl", "temporalDenoise");
    compileShaderBytecode(shaderDir / "tonemap.hlsl", "tonemap");
}

void setVolume(D3DState& d3d, const Volume& volume)
{
    unbindComputeResources(d3d.context.Get());
    createVolumeBuffers(d3d, volume);
    clearShadowVolume(d3d);
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
    const Vec3 extinction = maxVec3(settings.extinction, 0.0f);
    const Vec3 albedo = saturateVec3(settings.albedo);
    c.absorption = mulVec3(extinction, {1.0f - albedo.x, 1.0f - albedo.y, 1.0f - albedo.z});
    c.scattering = mulVec3(extinction, albedo);
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
    const CloudPhaseParameters cloudPhase = makeCloudPhaseParameters(settings.particleDiameterMicrons);
    c.phaseFunctionMode = static_cast<uint32_t>(std::clamp(settings.phaseFunctionMode, 0, 2));
    c.cloudPhaseGhg = c.phaseFunctionMode == 2u ? cloudPhase.gHg : settings.cloudPhaseGhg;
    c.cloudPhaseGd = cloudPhase.gD;
    c.cloudPhaseAlpha = c.phaseFunctionMode == 2u ? cloudPhase.alpha : std::max(settings.cloudPhaseAlpha, 0.0f);
    c.cloudPhaseWeight = cloudPhase.weightD;
    c.nubisDetailType = std::clamp(settings.nubisDetailType, 0.0f, 1.0f);
    c.qCriterionClipMin = std::min(settings.qCriterionClipMin, settings.qCriterionClipMax - 1.0e-6f);
    c.qCriterionClipMax = std::max(settings.qCriterionClipMax, c.qCriterionClipMin + 1.0e-6f);
    c.qCriterionBias = std::clamp(settings.qCriterionBias, -1.0f, 1.0f);
    c.hasQCriterion = volume.hasQCriterion ? 1u : 0u;
    c.nubisNoiseScale = std::max(settings.nubisNoiseScale, 1.0e-3f);
    c.maxDistanceToZero = std::max(settings.maxDistanceToZero, 1.0e-4f);
    c.msAttenuationScale = std::max(settings.msAttenuationScale, 0.0f);
    c.ambientLightColor = settings.ambientLightColor;
    c.raymarchPrimaryStepScale = std::max(settings.raymarchPrimaryStepScale, 0.0f);
    c.shadowUpdateFrames = static_cast<uint32_t>(std::clamp(settings.shadowVolumeUpdateFrames, 1, 64));
    c.shadowUpdateFrame = frameIndex % c.shadowUpdateFrames;
    c.shadowVolumeStepLength = std::max(settings.shadowVolumeStepLength, 1.0f);
    c.shadowLocalSampleOffset0 = std::max(settings.shadowLocalSampleOffset0, 0.0f);
    c.shadowLocalSampleOffset1 = std::max(settings.shadowLocalSampleOffset1, c.shadowLocalSampleOffset0 + 0.001f);
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    c.debugViewMode = static_cast<uint32_t>(std::clamp(settings.debugViewMode, 0, 2));
    c.debugSampleCountScale = std::max(settings.debugSampleCountScale, 1.0f);
#endif
    return c;
}

void dispatchRenderer(D3DState& d3d, const RenderConstants& constants)
{
    if (!d3d.volumeSrv || !d3d.signedDistanceSrv || !d3d.coarseSignedDistanceSrv || !d3d.shadowVolumeSrv || !d3d.shadowVolumeUav
        || !d3d.nubisNoiseSrv || !d3d.wrapSampler || !d3d.clampSampler || (constants.hasQCriterion != 0u && !d3d.qCriterionSrv)) {
        clearBackbuffer(d3d);
        return;
    }

    d3d.context->UpdateSubresource(d3d.constantsBuffer.Get(), 0, nullptr, &constants, 0, 0);

    ID3D11Buffer* cb = d3d.constantsBuffer.Get();
    d3d.context->CSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* shadowUpdateSrvs[] = {
        d3d.volumeSrv.Get(),
        nullptr,
        nullptr,
        nullptr,
        d3d.signedDistanceSrv.Get(),
        nullptr,
        nullptr,
        nullptr,
        d3d.qCriterionSrv ? d3d.qCriterionSrv.Get() : d3d.volumeSrv.Get(),
    };
    ID3D11UnorderedAccessView* shadowUpdateUavs[] = {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        d3d.shadowVolumeUav.Get(),
    };
    d3d.context->CSSetShader(d3d.shaders.updateShadowVolume.Get(), nullptr, 0);
    d3d.context->CSSetShaderResources(0, 9, shadowUpdateSrvs);
    d3d.context->CSSetUnorderedAccessViews(0, 5, shadowUpdateUavs, nullptr);
    dispatchShadowVolume(d3d.context.Get());
    unbindComputeResources(d3d.context.Get());
    d3d.context->CSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* renderSrvs[] = {
        d3d.volumeSrv.Get(),
        nullptr,
        nullptr,
        nullptr,
        d3d.signedDistanceSrv.Get(),
        d3d.nubisNoiseSrv.Get(),
        d3d.coarseSignedDistanceSrv.Get(),
        d3d.shadowVolumeSrv.Get(),
        d3d.qCriterionSrv ? d3d.qCriterionSrv.Get() : d3d.volumeSrv.Get(),
    };
    ID3D11UnorderedAccessView* renderUavs[] = {d3d.renderTexture.uav.Get(), nullptr, nullptr, nullptr, nullptr};
    ID3D11SamplerState* renderSamplers[] = {d3d.wrapSampler.Get(), d3d.clampSampler.Get()};
    d3d.context->CSSetShader(d3d.shaders.render.Get(), nullptr, 0);
    d3d.context->CSSetShaderResources(0, 9, renderSrvs);
    d3d.context->CSSetUnorderedAccessViews(0, 5, renderUavs, nullptr);
    d3d.context->CSSetSamplers(0, 2, renderSamplers);
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
