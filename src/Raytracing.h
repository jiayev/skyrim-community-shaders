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
    
    // High-level functions for feature use
    bool ApplyGlobalIllumination(float intensity, float distance, float saturation);
    bool ApplyReflections(float intensity, float roughness, float distance);
    
    // Settings
    Settings settings;
    
private:
    Raytracing() = default;
    ~Raytracing() = default;
    
    // Internal state
    bool initialized = false;
    bool resourcesCreated = false;
    
    // Internal functions for managing KickstartRT
    bool RegisterGeometry();
    bool UpdateGeometry();
    
    // Internal rendering functions
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
                           
    // Resource management
    bool GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix);
    bool GetRequiredBuffersForGI(ID3D11ShaderResourceView*& depthSRV, ID3D11ShaderResourceView*& normalSRV, ID3D11UnorderedAccessView*& outputUAV);
    bool GetRequiredBuffersForReflections(ID3D11ShaderResourceView*& depthSRV, ID3D11ShaderResourceView*& normalSRV, ID3D11ShaderResourceView*& roughnessSRV, ID3D11UnorderedAccessView*& outputUAV);
};
