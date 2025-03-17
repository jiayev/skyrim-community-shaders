#include "GlobalIllumination.h"


#include "State.h"
#include "Utils/Game.h"


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
    }
}

void GlobalIllumination::SetupResources()
{
    // We don't need to set up any resources here as we're using Raytracing functionality
    logger::info("[GlobalIllumination] Setup complete");
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
    return true;
}

void GlobalIllumination::UpdateSettingsForScene()
{
    if (!settings.AdaptToScene || !globals::features::raytracing)
        return;
    
    // Check if we're in an interior cell
    bool isInterior = false;
    if (globals::game::tes && globals::game::tes->GetRuntimeData().interiorCell) {
        isInterior = true;
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
        
    // Get current view and projection matrices
    DirectX::XMFLOAT4X4 viewMatrix = {}; // Get this from the game engine
    DirectX::XMFLOAT4X4 projMatrix = {}; // Get this from the game engine
    
    // Apply the GI-specific settings to the raytracer
    float intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
    
    // Call the raytracing subsystem with our specific GI parameters
    if (depthBuffer && normalBuffer && outputBuffer) {
        raytracing->GenerateRays(
            depthBuffer->srv.get(),
            normalBuffer->srv.get(),
            outputBuffer->uav.get(),
            viewMatrix,
            projMatrix,
            intensity,                   // Intensity for this specific effect
            settings.Distance,           // Distance for this specific effect
            settings.Saturation,         // Saturation for this specific effect
            raytracing->settings.SampleCount // Use the global sample count from raytracing settings
        );
    }
}

// Add a new member to store the effective intensity after scene adaptation
private:
    float effectiveIntensity = 1.0f;

// This callback could be registered to be called during appropriate render events
void UpdateGlobalIllumination()
{
    // Get required buffers from the deferred renderer
    auto* deferredBuffers = globals::deferred;
    if (!deferredBuffers)
        return;

    // Check if raytracing is available and enabled
    if (!GlobalIllumination::IsAvailable())
        return;
        
    // Optional: update settings based on scene
    GlobalIllumination::UpdateSettingsForScene();
    
    // Process GI using the raytracing system
    // This is just an example - the actual implementation would use real buffers
    /*
    Texture2D* depthBuffer = deferredBuffers->GetDepthBuffer();
    Texture2D* normalBuffer = deferredBuffers->GetNormalBuffer();
    Texture2D* outputBuffer = deferredBuffers->GetGIBuffer();
    
    GlobalIllumination::Generate(depthBuffer, normalBuffer, outputBuffer);
    */
} 