#pragma once

#include "Feature.h"
#include "Buffer.h"
#include <DirectXMath.h>
#include <d3d11.h>
#include <memory>
#include <string>

// Forward declarations
class KickstartRTImpl;

struct Raytracing : Feature 
{
public:
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

    // Feature implementation
    std::string GetName() override { return "Raytracing"; }
    std::string GetShortName() override { return "Raytracing"; }
    bool SupportsVR() override { return true; }
    std::string_view GetShaderDefineName() override { return "RT"; }
    
    void RestoreDefaultSettings() override;
    void DrawSettings() override;
    void SetupResources() override;
    void LoadSettings(json& o_json) override;
    void SaveSettings(json& o_json) override;
    
    // Test function for KickstartRT
    bool TestKickstartRT();
    
    // Raytracing-specific functionality
    void ClearResources();
    
    // Low-level raytracing operations
    bool GenerateRays(ID3D11ShaderResourceView* depthSRV, 
                     ID3D11ShaderResourceView* normalSRV,
                     ID3D11UnorderedAccessView* outputUAV,
                     const DirectX::XMFLOAT4X4& viewMatrix,
                     const DirectX::XMFLOAT4X4& projMatrix,
                     float intensity,
                     float distance,
                     float saturation,
                     uint32_t sampleCount);
    
    bool GenerateReflectionRays(ID3D11ShaderResourceView* depthSRV, 
                               ID3D11ShaderResourceView* normalSRV, 
                               ID3D11ShaderResourceView* roughnessSRV,
                               ID3D11UnorderedAccessView* outputUAV,
                               const DirectX::XMFLOAT4X4& viewMatrix,
                               const DirectX::XMFLOAT4X4& projMatrix,
                               float intensity,
                               float roughness,
                               float distance,
                               uint32_t sampleCount);
    
    // Check if raytracing is initialized
    bool IsInitialized() const { return initialized; }
    
    // Settings
    Settings settings;
    
private:
    bool initialized = false;
    bool resourcesCreated = false;
};
