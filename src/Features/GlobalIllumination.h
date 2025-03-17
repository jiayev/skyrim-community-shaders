#pragma once

// GlobalIllumination feature that leverages the Raytracing system
struct GlobalIllumination : Feature
{
    static GlobalIllumination* GetSingleton()
    {
        static GlobalIllumination singleton;
        return &singleton;
    }

    // Feature implementation
    std::string GetName() override { return "Global Illumination"; }
    std::string GetShortName() override { return "GlobalIllumination"; }
    bool SupportsVR() override { return true; }
    std::string_view GetShaderDefineName() override { return "GI"; }

    // These methods are required by Feature
    void RestoreDefaultSettings() override;
    void DrawSettings() override;
    void SetupResources() override;
    void LoadSettings(json& o_json) override;
    void SaveSettings(json& o_json) override;
    
    // GI-specific functionality
    void InitializeRaytracing();
    void GenerateGI(Texture2D* depthBuffer, Texture2D* normalBuffer, Texture2D* outputBuffer);
    void UpdateSettingsForScene();
    bool IsAvailable();
    
    // Buffer management methods
    void CreateGIOutputBuffer();
    void ApplyGIToFinalRender();
    
    // Pipeline integration methods
    void Update();
    void RegisterHooks();
    void CleanupResources();

    struct Settings
    {
        bool Enabled = true;
        float Intensity = 1.0f;
        float Distance = 200.0f;
        float Saturation = 1.0f;
        bool AdaptToScene = false;
        float InteriorIntensityMultiplier = 1.2f;
        float ExteriorIntensityMultiplier = 0.8f;
    };

    Settings settings;
    
private:
    float effectiveIntensity = 1.0f;
    bool raytracingAvailable = false; // Tracks whether raytracing is available and functioning
    
    // Buffer management
    std::shared_ptr<Texture2D> giOutputBuffer = nullptr;
    bool recreateBuffers = true;
    bool shouldUpdateGeometry = true;
};
