#pragma once

#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"
#include <DirectXMath.h>
#include <d3d11.h>
#include <unordered_map>
#include <string>

// Add include for Skyrim types
#include "RE/Skyrim.h"

namespace KickstartRTImpl
{
    // Core functions
    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized();
    void CleanupResources();
    bool WaitForGPU();

    // Geometry management
    bool RegisterGeometryWithKickstartRT(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, const std::string& name, KickstartRT::D3D11::GeometryHandle* outHandle);
    bool CreateInstance(KickstartRT::D3D11::GeometryHandle& geometryHandle, const DirectX::XMFLOAT4X4& transform, const std::string& name, KickstartRT::D3D11::InstanceHandle* outHandle);
    bool UpdateInstanceTransform(KickstartRT::D3D11::InstanceHandle& instanceHandle, const DirectX::XMFLOAT4X4& transform);
    bool RegisterSkyrimMesh(RE::BSTriShape* triShape, const std::string& name, KickstartRT::D3D11::GeometryHandle* outHandle);
    int CollectSceneGeometry(bool updateDynamicOnly);
    int CollectSceneGeometry();

    // Raytracing operations
    bool InjectDirectLighting(
        ID3D11ShaderResourceView* directLightingSRV,
        ID3D11ShaderResourceView* depthSRV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix);
        
    bool GenerateGI(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalSRV,
        ID3D11UnorderedAccessView* outputUAV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix);

    bool GenerateReflections(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalSRV,
        ID3D11ShaderResourceView* roughnessSRV,
        ID3D11UnorderedAccessView* outputUAV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix);

    // Global variables that need to be accessed from other files
    extern KickstartRT::D3D11::ExecuteContext* g_executeContext;
    extern Microsoft::WRL::ComPtr<ID3D11Fence> g_renderFence;
    extern uint64_t g_fenceValue;
    extern std::unordered_map<std::string, KickstartRT::D3D11::GeometryHandle> g_geometryHandles;
    extern std::unordered_map<std::string, KickstartRT::D3D11::InstanceHandle> g_instanceHandles;
    extern bool g_initialized;
} 