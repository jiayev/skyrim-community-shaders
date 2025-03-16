#pragma once

#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>

// Include KickstartRT headers
#include "KickstartRT.h"

class KickstartRTImpl {
public:
    KickstartRTImpl();
    ~KickstartRTImpl();

    // Initialize KickstartRT with a D3D11 device
    bool Initialize(ID3D11Device* device);
    
    // Shutdown and cleanup
    void Shutdown();

    // Register/update geometry from meshes
    bool RegisterGeometry(
        const void* vertexBuffer, uint32_t vertexCount, uint32_t vertexStride,
        const void* indexBuffer, uint32_t indexCount, uint32_t indexStride,
        const DirectX::XMFLOAT4X4& transform,
        KickstartRT::BVHTask::GeometryHandle& outGeometryHandle,
        KickstartRT::BVHTask::InstanceHandle& outInstanceHandle);

    // Update instance transform
    bool UpdateInstanceTransform(
        KickstartRT::BVHTask::InstanceHandle instanceHandle,
        const DirectX::XMFLOAT4X4& transform);

    // Inject lighting from a G-buffer (deferred rendering)
    bool InjectLighting(
        ID3D11ShaderResourceView* lightingBufferSRV,
        ID3D11ShaderResourceView* depthBufferSRV,
        ID3D11ShaderResourceView* normalBufferSRV,
        const DirectX::XMFLOAT4X4& viewMatrix,
        const DirectX::XMFLOAT4X4& projMatrix);

    // Generate reflections
    bool GenerateReflections(
        ID3D11ShaderResourceView* depthBufferSRV,
        ID3D11ShaderResourceView* normalBufferSRV,
        ID3D11ShaderResourceView* roughnessBufferSRV,
        ID3D11UnorderedAccessView* outputUAV,
        const DirectX::XMFLOAT4X4& viewMatrix,
        const DirectX::XMFLOAT4X4& projMatrix);

    // Clean up resources (call when changing scenes)
    void CleanupResources();

private:
    // KickstartRT execute context (main SDK handle)
    KickstartRT::D3D11::ExecuteContext* m_executeContext = nullptr;
    
    // Denoising context for reflections
    KickstartRT::DenoisingContextHandle m_denoisingContextHandle = {};
    
    // Flag to check if initialized
    bool m_initialized = false;
    
    // Current viewport dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    
    // Log callback setup
    static void LogCallback(KickstartRT::Log::Severity severity, const char* msg);
}; 