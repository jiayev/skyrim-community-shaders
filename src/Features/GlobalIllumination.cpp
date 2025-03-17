#include "GlobalIllumination.h"
#include "Raytracing.h"
#include "State.h"
#include "Utils/Game.h"
#include "Globals.h"
#include "imgui.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    GlobalIllumination::Settings,
    Enabled,
    Intensity,
    Distance,
    Saturation,
    AdaptToScene,
    InteriorIntensityMultiplier,
    ExteriorIntensityMultiplier)

void GlobalIllumination::RestoreDefaultSettings()
{
    settings = {};
}

void GlobalIllumination::DrawSettings()
{
    bool wasEnabled = settings.Enabled;
    
    // Main toggle
    ImGui::Checkbox("Enable Global Illumination", &settings.Enabled);
    
    // Check if we need to initialize raytracing when enabling
    if (!wasEnabled && settings.Enabled) {
        logger::info("[GlobalIllumination] Global Illumination enabled, initializing raytracing");
        InitializeRaytracing();
    }
    
    if (!settings.Enabled)
        return;
    
    // GI Settings
    ImGui::SeparatorText("Global Illumination Settings");
    
    ImGui::SliderFloat("GI Intensity", &settings.Intensity, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("GI Distance", &settings.Distance, 50.0f, 500.0f, "%.1f game units");
    ImGui::SliderFloat("GI Saturation", &settings.Saturation, 0.0f, 2.0f, "%.2f");
    
    // Scene adaptation
    ImGui::Checkbox("Adapt to Scene Type", &settings.AdaptToScene);
    
    if (settings.AdaptToScene) {
        ImGui::SliderFloat("Interior Intensity Multiplier", &settings.InteriorIntensityMultiplier, 0.5f, 3.0f, "%.2f");
        ImGui::SliderFloat("Exterior Intensity Multiplier", &settings.ExteriorIntensityMultiplier, 0.5f, 3.0f, "%.2f");
    }
    
    // Status info
    if (!IsAvailable()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Raytracing not available or initialized");
        
        // Add a button to try initializing again
        if (ImGui::Button("Try Initialize Raytracing")) {
            InitializeRaytracing();
        }
    } else {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Raytracing available and ready");
    }
}

void GlobalIllumination::InitializeRaytracing()
{
    // Initialize and test Raytracing
    auto raytracing = globals::features::raytracing;
    if (!raytracing) {
        logger::error("[GlobalIllumination] Failed to get Raytracing singleton");
        raytracingAvailable = false;
        return;
    }
    
    logger::info("[GlobalIllumination] Setting up resources and testing Raytracing...");
    
    // Initialize resources if not already done
    if (!raytracing->IsInitialized()) {
        auto device = globals::d3d::device;
        if (!device) {
            logger::error("[GlobalIllumination] Failed to get D3D11 device");
            raytracingAvailable = false;
            return;
        }
        
        logger::info("[GlobalIllumination] Initializing Raytracing resources");
        if (!raytracing->InitializeResources(device)) {
            logger::error("[GlobalIllumination] Failed to initialize Raytracing resources");
            raytracingAvailable = false;
            return;
        }
    }
    
    // First verify the system is initialized
    if (!raytracing->IsInitialized()) {
        logger::error("[GlobalIllumination] Raytracing system reports it is not initialized");
        raytracingAvailable = false;
        return;
    }
    
    // Now test KickstartRT to ensure it's working properly
    logger::info("[GlobalIllumination] Testing KickstartRT...");
    if (raytracing->TestKickstartRT()) {
        logger::info("[GlobalIllumination] KickstartRT test successful");
        raytracingAvailable = true;
        
        // Now register scene geometry with KickstartRT if the test was successful
        logger::info("[GlobalIllumination] Registering scene geometry with KickstartRT...");
        if (!raytracing->RegisterGeometry()) {
            logger::warn("[GlobalIllumination] Failed to register geometry, GI may not work properly");
            // Don't set raytracingAvailable to false, we'll try to continue anyway
        } else {
            logger::info("[GlobalIllumination] Geometry registration successful");
        }
    } else {
        logger::error("[GlobalIllumination] KickstartRT test failed");
        raytracingAvailable = false;
    }
    
    logger::info("[GlobalIllumination] Setup complete. Raytracing available: {}", raytracingAvailable);
}

void GlobalIllumination::SetupResources()
{
    // Don't automatically initialize, wait for the user to enable the feature
    logger::info("[GlobalIllumination] Resource setup - waiting for user to enable feature");
    raytracingAvailable = false;
    
    // If already enabled in settings, initialize
    if (settings.Enabled) {
        logger::info("[GlobalIllumination] Feature is enabled in settings, initializing raytracing");
        InitializeRaytracing();
    }
}

void GlobalIllumination::LoadSettings(json& o_json)
{
    settings = o_json;
}

void GlobalIllumination::SaveSettings(json& o_json)
{
    o_json = settings;
}

bool GlobalIllumination::IsAvailable()
{
    // Check if raytracing is initialized and test passed
    if (!raytracingAvailable) {
        return false;
    }
    
    // Also check if the raytracing singleton is still valid and initialized
    auto raytracing = globals::features::raytracing;
    return raytracing && raytracing->IsInitialized();
}

void GlobalIllumination::UpdateSettingsForScene()
{
    if (!settings.AdaptToScene || !globals::features::raytracing)
        return;
    
    // Check if we're in an interior cell
    bool isInterior = false;
    if (globals::game::tes) {
        // Use Sky as an indicator - if there's no sky, we're in an interior
        isInterior = !globals::game::sky || globals::game::sky->mode == RE::Sky::Mode::kInterior;
    }
    
    // Apply intensity adjustment based on interior/exterior context
    float adjustedIntensity;
    if (isInterior) {
        adjustedIntensity = settings.Intensity * settings.InteriorIntensityMultiplier;
    } else {
        adjustedIntensity = settings.Intensity * settings.ExteriorIntensityMultiplier;
    }
    
    // Store the adjusted values for use during rendering
    effectiveIntensity = adjustedIntensity;
}

void GlobalIllumination::GenerateGI(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* outputBuffer)
{
    if (!settings.Enabled || !IsAvailable())
        return;
    
    // Update settings based on the current scene if needed
    UpdateSettingsForScene();
    
    // Get raytracing feature
    auto* raytracing = globals::features::raytracing;
    if (!raytracing)
        return;
    
    // Log what we're working with
    logger::info("[GlobalIllumination] GenerateGI called with depth:{}, normal:{}, output:{}", 
        depthBuffer ? "provided" : "null", 
        normalBuffer ? "provided" : "null", 
        outputBuffer ? "provided" : "null");
    
    // Get the current view and projection matrices
    DirectX::XMFLOAT4X4 viewMatrix, projMatrix;
    
    // Use identity matrices for now - will get proper matrices in the future
    // The Raytracing class can get its own matrices, but we're moving all buffer management here
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Extract DirectX resources from the Texture2D objects
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11ShaderResourceView* normalSRV = nullptr;
    ID3D11UnorderedAccessView* outputUAV = nullptr;
    
    // First try to use the provided buffers
    if (depthBuffer && depthBuffer->srv)
        depthSRV = depthBuffer->srv.get();
    
    if (normalBuffer && normalBuffer->srv)
        normalSRV = normalBuffer->srv.get();
    
    if (outputBuffer && outputBuffer->uav)
        outputUAV = outputBuffer->uav.get();
    
    // If we don't have all the required resources, try to get them from the game
    if (!depthSRV || !normalSRV || !outputUAV) {
        logger::info("[GlobalIllumination] Missing some buffers, getting from game renderer");
        
        // Get the depth buffer from the game if not provided
        if (!depthSRV) {
            if (auto renderer = globals::game::renderer) {
                auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
                depthSRV = depthTexture.depthSRV;
                logger::info("[GlobalIllumination] Got depth buffer from game renderer");
            }
        }
        
        // For normal buffer, we would need to extract from G-buffer
        // This is a placeholder - the exact method to access normals will depend on Skyrim's rendering setup
        if (!normalSRV) {
            logger::warn("[GlobalIllumination] Normal buffer not provided and no game method implemented yet");
            // Placeholder SRV - in a real implementation we'd get this from Skyrim's G-buffer
        }
        
        // For output buffer, we'd need a render target
        if (!outputUAV) {
            logger::warn("[GlobalIllumination] Output buffer not provided and no game method implemented yet");
            // Placeholder UAV - in a real implementation we'd create this
        }
    }
    
    // Check if we have all required resources
    if (!depthSRV || !normalSRV || !outputUAV) {
        logger::error("[GlobalIllumination] Missing required buffers for GI, cannot proceed");
        return;
    }
    
    // Apply the GI-specific settings to the raytracer
    float intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
    
    // Set ray length based on user settings
    float rayLength = settings.Distance;
    
    // For saturation, we'd need to modify the shader or post-process
    float saturation = settings.Saturation;
    
    // Log that we're generating GI
    logger::info("[GlobalIllumination] Generating GI with intensity={}, distance={}, saturation={}", 
                intensity, rayLength, saturation);
    
    // Call Raytracing::GenerateGI directly with our buffers
    bool success = raytracing->GenerateGI(
        depthSRV,
        normalSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
    
    if (!success) {
        logger::warn("[GlobalIllumination] Failed to generate global illumination");
    } else {
        logger::info("[GlobalIllumination] Global illumination rendered successfully");
    }
}

// This callback could be registered to be called during appropriate render events
void UpdateGlobalIllumination()
{
    // Get singleton instance
    auto* gi = GlobalIllumination::GetSingleton();
    
    // Check if raytracing is available and enabled
    if (!gi->IsAvailable())
        return;
        
    // Optional: update settings based on scene
    gi->UpdateSettingsForScene();
    
    // Process GI using the raytracing system
    gi->GenerateGI(nullptr, nullptr, nullptr);
} 