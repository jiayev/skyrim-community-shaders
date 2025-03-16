#pragma once

// Enable KickstartRT by default
#ifndef ENABLE_KICKSTART_RT
#define ENABLE_KICKSTART_RT 1
#endif

// Define API for KickstartRT
#define KickstartRT_Graphics_API_D3D11 1

// Disable warning about struct vs class mismatch
#pragma warning(disable: 4099)

#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>
#include <DirectXMath.h>

// Forward declarations for KickstartRT
// These match the forward declarations in the KickstartRT headers
namespace KickstartRT
{
    namespace D3D11
    {
        struct ExecuteContext;
    }
}

/**
 * @brief Simplified implementation of KickstartRT raytracing for the Community Shaders project
 */
class KickstartRTImpl {
public:
    KickstartRTImpl();
    ~KickstartRTImpl();

    // Initialize KickstartRT with a D3D11 device
    bool Initialize(ID3D11Device* device);
    
    // Shutdown and cleanup
    void Shutdown();
    
    // Simple test to check if KickstartRT is working
    bool RunTest();
    
    // Clean up resources (call when changing scenes)
    void CleanupResources();
    
    // Register geometry with KickstartRT
    bool RegisterGeometry(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, 
                         uint32_t vertexCount, uint32_t indexCount, 
                         uint32_t vertexStride, void** outGeoHandle);
    
    // Register an instance with KickstartRT
    bool RegisterInstance(void* geoHandle, const DirectX::XMFLOAT4X4& transform, void** outInstHandle);
    
    // Generate global illumination using raytracing
    bool GenerateGI(ID3D11ShaderResourceView* depthSRV, 
                  ID3D11ShaderResourceView* normalSRV,
                  ID3D11UnorderedAccessView* outputUAV,
                  const DirectX::XMFLOAT4X4& viewMatrix,
                  const DirectX::XMFLOAT4X4& projMatrix);
    
    // Generate reflections using raytracing
    bool GenerateReflections(ID3D11ShaderResourceView* depthSRV, 
                           ID3D11ShaderResourceView* normalSRV, 
                           ID3D11ShaderResourceView* roughnessSRV,
                           ID3D11UnorderedAccessView* outputUAV,
                           const DirectX::XMFLOAT4X4& viewMatrix,
                           const DirectX::XMFLOAT4X4& projMatrix);
    
    // Inject direct lighting for global illumination
    bool InjectLighting(ID3D11ShaderResourceView* lightingSRV,
                      ID3D11ShaderResourceView* depthSRV,
                      ID3D11ShaderResourceView* normalSRV,
                      const DirectX::XMFLOAT4X4& viewMatrix,
                      const DirectX::XMFLOAT4X4& projMatrix);

    // Check if initialized
    bool IsInitialized() const { return m_initialized; }

private:
    // KickstartRT implementation details are hidden
    void* m_executeContext = nullptr;
    
    // Flag to check if initialized
    bool m_initialized = false;
    
    // Current viewport dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;
}; 