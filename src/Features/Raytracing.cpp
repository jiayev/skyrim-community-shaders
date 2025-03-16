#include "Features/Raytracing.h"
#include "../../include/KickstartRTImpl.h"

// Enable KickstartRT by default
#ifndef ENABLE_KICKSTART_RT
#define ENABLE_KICKSTART_RT 1
#endif

#include <DirectXTex.h>

#include "Deferred.h"
#include "Menu.h"
#include "State.h"
#include "Utils/Game.h"
#include "Globals.h"

// Define the global instance
namespace
{
    Raytracing g_raytracing;
}

// Set the pointer in globals
namespace globals::features
{
    Raytracing* raytracing = &g_raytracing;
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    Raytracing::Settings,
    Enabled,
    EnableGI,
    EnableReflections,
    GIIntensity,
    GIDistance,
    GISaturation,
    GISampleCount,
    ReflectionIntensity,
    ReflectionRoughness,
    ReflectionDistance,
    ReflectionSampleCount,
    EnableDenoising,
    MaxDenoiserHistory,
    DenoiserBlend,
    ShowRaytracedResults)

//////////////////////////////////////////////////////////////////////////////////////////////

// Simple test function to verify KickstartRT loads and works correctly
bool Raytracing::TestKickstartRT()
{
#ifdef ENABLE_KICKSTART_RT
    logger::info("[Raytracing] Testing KickstartRT initialization...");
    
    // Create KickstartRT implementation
    auto testKickstart = std::make_unique<KickstartRTImpl>();
    
    // Get the D3D11 device
    ID3D11Device* device = globals::d3d::device;
    if (!device) {
        logger::error("[Raytracing] D3D11 device is null, cannot test KickstartRT");
        return false;
    }
    
    // Initialize KickstartRT
    if (!testKickstart->Initialize(device)) {
        logger::error("[Raytracing] Failed to initialize KickstartRT during test");
        return false;
    }
    
    logger::info("[Raytracing] KickstartRT initialized successfully for test");
    
    // Run a simple test
    if (!testKickstart->RunTest()) {
        logger::error("[Raytracing] KickstartRT functionality test failed");
        return false;
    }
    
    // Clean up
    testKickstart->Shutdown();
    logger::info("[Raytracing] KickstartRT test completed successfully");
    
    return true;
#else
    logger::warn("[Raytracing] KickstartRT not enabled in build, test skipped");
    return false;
#endif
}

void Raytracing::RestoreDefaultSettings()
{
    settings = {};
}

void Raytracing::DrawSettings()
{
    // Main toggles
    ImGui::SeparatorText("Toggles");
    
    if (ImGui::BeginTable("Toggles", 3)) {
        ImGui::TableNextColumn();
        ImGui::Checkbox("Enabled", &settings.Enabled);
        
        // Disable if KickstartRT is not enabled in build
#ifndef ENABLE_KICKSTART_RT
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "KickstartRT not enabled in build!");
        ImGui::BeginDisabled();
#else
        if (!IsInitialized()) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "KickstartRT not initialized!");
        }
#endif
        
        ImGui::TableNextColumn();
        ImGui::Checkbox("Global Illumination", &settings.EnableGI);
        
        ImGui::TableNextColumn();
        ImGui::Checkbox("Reflections", &settings.EnableReflections);
        
#ifndef ENABLE_KICKSTART_RT
        ImGui::EndDisabled();
#else
        if (!IsInitialized()) {
            ImGui::EndDisabled();
        }
#endif
        
        ImGui::EndTable();
    }
    
#ifdef ENABLE_KICKSTART_RT
    // Add a test button
    if (ImGui::Button("Test KickstartRT")) {
        bool testResult = TestKickstartRT();
        if (testResult) {
            logger::info("[Raytracing] KickstartRT test succeeded");
        } else {
            logger::error("[Raytracing] KickstartRT test failed");
        }
    }
#endif
    
    if (!settings.Enabled)
        return;
    
    // GI Settings
    if (settings.EnableGI) {
        ImGui::SeparatorText("Global Illumination Settings");
        
        ImGui::SliderFloat("GI Intensity", &settings.GIIntensity, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("GI Distance", &settings.GIDistance, 50.0f, 500.0f, "%.1f game units");
        ImGui::SliderFloat("GI Saturation", &settings.GISaturation, 0.0f, 2.0f, "%.2f");
        ImGui::SliderInt("GI Samples", (int*)&settings.GISampleCount, 1, 4);
    }
    
    // Reflection Settings
    if (settings.EnableReflections) {
        ImGui::SeparatorText("Reflection Settings");
        
        ImGui::SliderFloat("Reflection Intensity", &settings.ReflectionIntensity, 0.0f, 5.0f, "%.2f");
        ImGui::SliderFloat("Reflection Distance", &settings.ReflectionDistance, 50.0f, 1000.0f, "%.1f game units");
        ImGui::SliderInt("Reflection Samples", (int*)&settings.ReflectionSampleCount, 1, 8);
    }
    
    // Denoising Settings
    ImGui::SeparatorText("Denoising");
    
    ImGui::Checkbox("Enable Denoising", &settings.EnableDenoising);
    
    if (settings.EnableDenoising) {
        ImGui::SliderInt("Max Denoiser History", (int*)&settings.MaxDenoiserHistory, 1, 32);
        ImGui::SliderFloat("Denoiser Blend Factor", &settings.DenoiserBlend, 0.01f, 1.0f, "%.2f");
    }
    
    // Debug
    ImGui::SeparatorText("Debug");
    
    ImGui::Checkbox("Show Raytraced Results Only", &settings.ShowRaytracedResults);
    
#ifdef ENABLE_KICKSTART_RT
    if (IsInitialized()) {
        if (ImGui::TreeNode("Raytraced Buffers")) {
            static float debugRescale = 0.3f;
            ImGui::SliderFloat("View Resize", &debugRescale, 0.0f, 1.0f);
            
            // Display the GI and reflection buffers if available
            if (rtGITexture) {
                BUFFER_VIEWER_NODE(rtGITexture, debugRescale);
            }
            
            if (rtReflectionTexture) {
                BUFFER_VIEWER_NODE(rtReflectionTexture, debugRescale);
            }
            
            ImGui::TreePop();
        }
    }
#endif
}

void Raytracing::LoadSettings(json& o_json)
{
    settings = o_json;
    
    // Initialize if enabled
    if (settings.Enabled) {
        SetupResources();
    }
}

void Raytracing::SaveSettings(json& o_json)
{
    o_json = settings;
}

void Raytracing::SetupResources()
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized) {
        logger::info("[Raytracing] Setting up KickstartRT resources");
        
        // Check for hardware compatibility
        ID3D11Device* device = globals::d3d::device;
        if (!device) {
            logger::error("[Raytracing] D3D11 device is null, cannot initialize KickstartRT");
            return;
        }
        
        // Note: We can't check for raytracing support directly in D3D11
        // We'll rely on KickstartRT to determine compatibility
        
        // Create KickstartRT implementation
        m_rtImpl = std::make_unique<KickstartRTImpl>();
        
        // Initialize KickstartRT
        if (m_rtImpl->Initialize(device)) {
            initialized = true;
            logger::info("[Raytracing] KickstartRT initialized successfully");
            
            // Run a test to verify functionality
            if (m_rtImpl->RunTest()) {
                logger::info("[Raytracing] KickstartRT test passed");
            } else {
                logger::warn("[Raytracing] KickstartRT test failed, but continuing initialization");
            }
        } else {
            logger::error("[Raytracing] Failed to initialize KickstartRT");
            m_rtImpl.reset();
            return;
        }
    }
    
    if (!resourcesCreated && initialized) {
        // Getting D3D11 device directly
        auto device = globals::d3d::device;
        
        if (!device) {
            logger::error("[Raytracing] D3D11 device is null, cannot create resources");
            return;
        }
        
        logger::info("[Raytracing] Creating render textures for GI and reflections");
        
        try {
            // Get screen dimensions
            uint32_t width = static_cast<uint32_t>(globals::state->screenSize.x);
            uint32_t height = static_cast<uint32_t>(globals::state->screenSize.y);
            
            if (width == 0 || height == 0) {
                logger::error("[Raytracing] Invalid screen dimensions: {}x{}", width, height);
                return;
            }
            
            // Create textures for GI and reflections
            D3D11_TEXTURE2D_DESC texDesc{
                .Width = width,
                .Height = height,
                .MipLevels = 1,
                .ArraySize = 1,
                .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
                .SampleDesc = { 1, 0 },
                .Usage = D3D11_USAGE_DEFAULT,
                .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
                .CPUAccessFlags = 0,
                .MiscFlags = 0
            };
            
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
                .Texture2D = {
                    .MostDetailedMip = 0,
                    .MipLevels = 1
                }
            };
            
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
                .Texture2D = { .MipSlice = 0 }
            };
            
            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
                .Format = texDesc.Format,
                .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
                .Texture2D = { .MipSlice = 0 }
            };
            
            // Create GI texture
            rtGITexture = eastl::make_unique<Texture2D>(texDesc);
            rtGITexture->CreateSRV(srvDesc);
            rtGITexture->CreateUAV(uavDesc);
            rtGITexture->CreateRTV(rtvDesc);
            rtGIUAV = rtGITexture->uav;
            
            // Create reflection texture
            rtReflectionTexture = eastl::make_unique<Texture2D>(texDesc);
            rtReflectionTexture->CreateSRV(srvDesc);
            rtReflectionTexture->CreateUAV(uavDesc);
            rtReflectionTexture->CreateRTV(rtvDesc);
            rtReflectionUAV = rtReflectionTexture->uav;
            
            resourcesCreated = true;
            logger::info("[Raytracing] KickstartRT resources created successfully: {}x{}", width, height);
        } catch (const std::exception& e) {
            logger::error("[Raytracing] Failed to create resources: {}", e.what());
            ClearResources();
        }
    }
#else
    logger::warn("[Raytracing] KickstartRT support not enabled in build");
#endif
}

void Raytracing::ClearResources()
{
#ifdef ENABLE_KICKSTART_RT
    if (initialized) {
        logger::info("[Raytracing] Clearing KickstartRT resources");
        
        // Clear registered geometry
        registeredMeshes.clear();
        
        // Clean up resources
        rtGITexture.reset();
        rtReflectionTexture.reset();
        rtGIUAV = nullptr;
        rtReflectionUAV = nullptr;
        
        // Shutdown KickstartRT
        if (m_rtImpl) {
            m_rtImpl->Shutdown();
            m_rtImpl.reset();
        }
        
        initialized = false;
        resourcesCreated = false;
        geometryRegistered = false;
        
        logger::info("[Raytracing] KickstartRT resources cleared");
    }
#endif
}

void Raytracing::RegisterGeometry()
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized || !m_rtImpl)
        return;
    
    // Implementation for registering geometry with KickstartRT
    // This depends on the geometry representation used by the game
#endif
}

void Raytracing::UpdateGeometry()
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized || !m_rtImpl)
        return;
    
    // Implementation for updating geometry with KickstartRT
    // This depends on the geometry representation used by the game
#endif
}

bool Raytracing::InjectLighting(ID3D11ShaderResourceView* lightingSRV, 
                             ID3D11ShaderResourceView* depthSRV,
                             ID3D11ShaderResourceView* normalSRV, 
                             const DirectX::XMFLOAT4X4& viewMatrix,
                             const DirectX::XMFLOAT4X4& projMatrix) 
{
    // Disable warning for unreferenced parameters
    #pragma warning(push)
    #pragma warning(disable: 4100) // unreferenced formal parameter

    if (m_rtImpl) {
        return m_rtImpl->InjectLighting(lightingSRV, depthSRV, normalSRV, viewMatrix, projMatrix);
    }
    return false;

    #pragma warning(pop)
}

void Raytracing::InjectLighting(Texture2D* lightBuffer, Texture2D* depthBuffer, Texture2D* normalBuffer)
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized || !m_rtImpl)
        return;
    
    // For now, we'll use identity matrices as placeholders
    // In a real implementation, these would come from the game's camera
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Extract SRVs from the Texture2D objects
    ID3D11ShaderResourceView* lightingSRV = lightBuffer ? lightBuffer->srv.get() : nullptr;
    ID3D11ShaderResourceView* depthSRV = depthBuffer ? depthBuffer->srv.get() : nullptr;
    ID3D11ShaderResourceView* normalSRV = normalBuffer ? normalBuffer->srv.get() : nullptr;
    
    // Call the new method with the extracted SRVs
    InjectLighting(lightingSRV, depthSRV, normalSRV, viewMatrix, projMatrix);
#endif
}

#pragma warning(push)
#pragma warning(disable: 4100) // Unreferenced formal parameter
void Raytracing::GenerateGI(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* outputBuffer)
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized || !m_rtImpl || !rtGITexture)
        return;
        
    // Get view and projection matrices
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;
    
    // Get camera data
    auto cameraData = Util::GetCameraData(0);
    
    // Convert matrices to DirectX format
    auto& skyrimViewMatrix = cameraData.viewMat;
    auto& skyrimProjMatrix = cameraData.projMat;
    
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            viewMatrix.m[i][j] = skyrimViewMatrix(i, j);
            projMatrix.m[i][j] = skyrimProjMatrix(i, j);
        }
    }
    
    // Generate global illumination
    m_rtImpl->GenerateGI(
        depthBuffer->srv.get(),
        normalBuffer->srv.get(),
        rtGITexture->uav.get(),
        viewMatrix,
        projMatrix
    );
#endif
}

void Raytracing::GenerateReflections(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* roughnessBuffer, Texture2D* outputBuffer)
{
#ifdef ENABLE_KICKSTART_RT
    if (!initialized || !m_rtImpl || !rtReflectionTexture)
        return;
    
    // Get view and projection matrices
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;
    
    // Get camera data
    auto cameraData = Util::GetCameraData(0);
    
    // Convert matrices to DirectX format
    auto& skyrimViewMatrix = cameraData.viewMat;
    auto& skyrimProjMatrix = cameraData.projMat;
    
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            viewMatrix.m[i][j] = skyrimViewMatrix(i, j);
            projMatrix.m[i][j] = skyrimProjMatrix(i, j);
        }
    }
    
    // Generate reflections
    m_rtImpl->GenerateReflections(
        depthBuffer->srv.get(),
        normalBuffer->srv.get(),
        roughnessBuffer->srv.get(),
        rtReflectionTexture->uav.get(),
        viewMatrix,
        projMatrix
    );
#endif
}
#pragma warning(pop)

ID3D11ShaderResourceView* Raytracing::GetGISRV() const
{
#ifdef ENABLE_KICKSTART_RT
    if (initialized && resourcesCreated && rtGITexture)
        return rtGITexture->srv.get();
#endif
    return nullptr;
}

ID3D11ShaderResourceView* Raytracing::GetReflectionSRV() const
{
#ifdef ENABLE_KICKSTART_RT
    if (initialized && resourcesCreated && rtReflectionTexture)
        return rtReflectionTexture->srv.get();
#endif
    return nullptr; 
} 