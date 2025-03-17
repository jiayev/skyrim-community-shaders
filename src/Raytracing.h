#pragma once

#include "../include/KickstartRT/KickstartRT.h"
#include <DirectXMath.h>
#include <d3d11.h>

// Forward declarations
class Texture2D;
struct ExecuteContextD3D11;

/**
 * Utility class for raytracing functionality using KickstartRT.
 * This provides a simplified interface for features to access raytracing.
 */
class Raytracing
{
public:
    static Raytracing* GetSingleton()
    {
        static Raytracing singleton;
        return &singleton;
    }

    struct Settings
    {
        bool Enabled = false;
        
        // Core raytracing settings
        uint32_t SampleCount = 1;
        float RayLength = 200.0f;
        
        // Denoising
        bool EnableDenoising = true;
        uint32_t MaxDenoiserHistory = 8;
        float DenoiserBlend = 0.1f;
        
        // Debug
        bool ShowRaytracedResults = false;
    };
    
    // Initialization
    bool InitializeResources(ID3D11Device* device);
    void ClearResources();
    
    // Check if KickstartRT is initialized and available
    bool IsInitialized() const { return initialized; }
    bool TestKickstartRT();
    
    // Make geometry registration functions public
    bool RegisterGeometry();
    bool UpdateGeometry();
    
    // Settings
    Settings settings;
    
    // Direct access to core rendering functions (moved from private to public)
    // GlobalIllumination will provide the buffers
    bool GenerateGI(ID3D11ShaderResourceView* depthSRV, 
                   ID3D11ShaderResourceView* normalSRV,
                   ID3D11UnorderedAccessView* outputUAV,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   const DirectX::XMFLOAT4X4& projMatrix);
                   
    bool GenerateReflections(ID3D11ShaderResourceView* depthSRV, 
                           ID3D11ShaderResourceView* normalSRV, 
                           ID3D11ShaderResourceView* roughnessSRV,
                           ID3D11UnorderedAccessView* outputUAV,
                           const DirectX::XMFLOAT4X4& viewMatrix,
                           const DirectX::XMFLOAT4X4& projMatrix);
    
private:
    Raytracing() = default;
    ~Raytracing() = default;
    
    // Internal state
    bool initialized = false;
    bool resourcesCreated = false;
                           
    // Resource management
    bool GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix);

    // Shortcut to enable/disable the system
    void Enable() { settings.Enabled = true; }
    void Disable() { settings.Enabled = false; }
    bool IsEnabled() const { return settings.Enabled && initialized; }
};
