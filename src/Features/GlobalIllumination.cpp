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
    // Main toggle
    ImGui::Checkbox("Enable Global Illumination", &settings.Enabled);
    
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
    } else {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Raytracing available and ready");
    }
}

void GlobalIllumination::SetupResources()
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
    
    // Test KickstartRT to ensure it's working properly
    logger::info("[GlobalIllumination] Testing KickstartRT...");
    if (raytracing->TestKickstartRT()) {
        logger::info("[GlobalIllumination] KickstartRT test successful");
        raytracingAvailable = true;
    } else {
        logger::error("[GlobalIllumination] KickstartRT test failed");
        raytracingAvailable = false;
    }
    
    logger::info("[GlobalIllumination] Setup complete. Raytracing available: {}", raytracingAvailable);
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
    (void)depthBuffer;
    (void)normalBuffer;
    (void)outputBuffer;
    if (!settings.Enabled || !IsAvailable())
        return;
    
    // Update settings based on the current scene if needed
    UpdateSettingsForScene();
    
    // Get raytracing feature
    auto* raytracing = globals::features::raytracing;
    if (!raytracing)
        return;
        
    // Apply the GI-specific settings to the raytracer
    float intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
    
    // Use the simplified API - all buffers will be handled internally
    raytracing->ApplyGlobalIllumination(
        intensity,                   // Intensity for this specific effect
        settings.Distance,           // Distance for this specific effect
        settings.Saturation          // Saturation for this specific effect
    );
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