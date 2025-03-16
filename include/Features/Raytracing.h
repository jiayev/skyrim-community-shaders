#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>
#include <array>

#include "Menu.h"
#include "nlohmann/json.hpp"
#include "Feature.h"

#ifdef ENABLE_KICKSTART_RT
#include "../KickstartRTImpl.h"
#endif

using json = nlohmann::json;

struct Raytracing : Feature
{
public:
    struct Settings
    {
        bool Enabled = false;
        bool EnableGI = true;
        bool EnableReflections = false;
        
        // GI Settings
        float GIIntensity = 1.0f;
        float GIDistance = 100.0f;
        float GISaturation = 1.0f;
        uint32_t GISampleCount = 1;
        
        // Reflection Settings
        float ReflectionIntensity = 1.0f;
        float ReflectionRoughness = 0.5f;
        float ReflectionDistance = 200.0f;
        uint32_t ReflectionSampleCount = 1;
        
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
    void PostPostLoad() override { SetupResources(); }
    
    // Test function for KickstartRT
    bool TestKickstartRT();
    
    // Raytracing-specific functionality
    void ClearResources();
    
    void RegisterGeometry();
    void UpdateGeometry();
    
    // Updated method signature to match implementation
    bool InjectLighting(ID3D11ShaderResourceView* lightingSRV, 
                       ID3D11ShaderResourceView* depthSRV,
                       ID3D11ShaderResourceView* normalSRV, 
                       const DirectX::XMFLOAT4X4& viewMatrix,
                       const DirectX::XMFLOAT4X4& projMatrix);
    
    // Legacy method to maintain compatibility
    void InjectLighting(Texture2D* lightBuffer, Texture2D* depthBuffer, Texture2D* normalBuffer);
    
    void GenerateGI(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* outputBuffer);
    void GenerateReflections(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* roughnessBuffer, Texture2D* outputBuffer);
    
    bool IsInitialized() const { return initialized; }
    
    // Get the GI and reflection texture SRVs for compositing
    ID3D11ShaderResourceView* GetGISRV() const;
    ID3D11ShaderResourceView* GetReflectionSRV() const;
    
private:
    Settings settings;
    
#ifdef ENABLE_KICKSTART_RT
    // KickstartRT implementation
    std::unique_ptr<KickstartRTImpl> m_rtImpl;
    
    // Textures for GI and reflections
    eastl::unique_ptr<Texture2D> rtGITexture;
    eastl::unique_ptr<Texture2D> rtReflectionTexture;
    
    // UAVs for output
    winrt::com_ptr<ID3D11UnorderedAccessView> rtGIUAV;
    winrt::com_ptr<ID3D11UnorderedAccessView> rtReflectionUAV;
    
    // Track registered geometry
    struct RegisteredMesh {
        void* geoHandle = nullptr;  // Simplified to avoid undefined types
        void* instHandle = nullptr; // Simplified to avoid undefined types
        uint64_t meshID = 0;
    };
    std::vector<RegisteredMesh> registeredMeshes;
#endif
    
    bool initialized = false;
    bool resourcesCreated = false;
    bool geometryRegistered = false;
}; 