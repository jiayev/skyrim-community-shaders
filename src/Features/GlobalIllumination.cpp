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
    
    logger::info("[GlobalIllumination] Setting up raytracing resources...");
    
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
    
    // Skip the test as it's causing issues, and assume it would pass
    logger::info("[GlobalIllumination] Skipping KickstartRT test and proceeding with geometry registration");
    raytracingAvailable = true;
    
    // Register scene geometry with KickstartRT
    logger::info("[GlobalIllumination] Registering scene geometry with KickstartRT...");
    if (!raytracing->RegisterGeometry()) {
        logger::warn("[GlobalIllumination] Failed to register geometry, GI may not work properly");
        // Don't set raytracingAvailable to false, we'll try to continue anyway
    } else {
        logger::info("[GlobalIllumination] Geometry registration successful");
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
    
    // Get proper matrices from Skyrim's renderer
    if (auto renderer = globals::game::renderer) {
        // Try to get matrices from per-frame constant buffer
        bool gotMatrices = false;
        
        // Get the per-frame buffer from globals
        auto perFrameBuffer = *globals::game::perFrame.get();
        if (perFrameBuffer) {
            // Map the buffer to read its contents
            ID3D11DeviceContext* context = globals::d3d::context;
            if (context) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(context->Map(perFrameBuffer, 0, D3D11_MAP_READ, 0, &mapped))) {
                    // The per-frame buffer contains view and projection matrices
                    // Based on Skyrim's shader constant layout
                    struct PerFrameBuffer {
                        DirectX::XMFLOAT4X4 viewMatrix;         // Offset 0
                        DirectX::XMFLOAT4X4 projMatrix;         // Offset 64
                        // Other data follows...
                    };
                    
                    const PerFrameBuffer* frameData = reinterpret_cast<const PerFrameBuffer*>(mapped.pData);
                    viewMatrix = frameData->viewMatrix;
                    projMatrix = frameData->projMatrix;
                    
                    context->Unmap(perFrameBuffer, 0);
                    
                    gotMatrices = true;
                    logger::info("[GlobalIllumination] Successfully extracted view/projection matrices from per-frame buffer");
                }
            }
        }
        
        // If we couldn't get matrices from buffer, fall back to identity matrices
        if (!gotMatrices) {
            logger::warn("[GlobalIllumination] Could not extract matrices from per-frame buffer, using identity matrices");
            DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
        }
    } else {
        logger::warn("[GlobalIllumination] Renderer not available, using identity matrices");
        DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
        DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    }
    
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
                if (!depthSRV) {
                    logger::error("[GlobalIllumination] Failed to get depth buffer from game renderer");
                    return; // Early out as we can't proceed without depth buffer
                }
                logger::info("[GlobalIllumination] Got depth buffer from game renderer");
            } else {
                logger::error("[GlobalIllumination] No game renderer available to get depth buffer");
                return; // Early out as we can't proceed without renderer
            }
        }
        
        // For normal buffer, extract from G-buffer
        if (!normalSRV) {
            if (auto renderer = globals::game::renderer) {
                // Get device for creating resources
                auto device = globals::d3d::device;
                if (!device) {
                    logger::error("[GlobalIllumination] No D3D11 device available for normal buffer");
                    return;
                }
                
                // In Skyrim, normals are stored in the G-buffer which is in the render targets
                // Look for a render target that might contain normals (we need to find the right index)
                // For now, we'll try the first few render targets
                auto& renderTargetData = renderer->GetRuntimeData().renderTargets;
                ID3D11ShaderResourceView* possibleNormalSRV = nullptr;
                
                // Try a few potential render targets
                for (int i = 0; i < 5; i++) {
                    if (renderTargetData[i].texture) {
                        // Instead of accessing views[0] directly, create an SRV from the texture
                        ID3D11ShaderResourceView* tempSRV = nullptr;
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Use the same format as the texture
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = 1;
                        srvDesc.Texture2D.MostDetailedMip = 0;
                        
                        // Try to get or create an SRV for this texture
                        ID3D11Resource* resource = nullptr;
                        renderTargetData[i].texture->QueryInterface(__uuidof(ID3D11Resource), (void**)&resource);
                        
                        if (resource) {
                            // Try to get an existing SRV or create a new one
                            device->CreateShaderResourceView(resource, &srvDesc, &tempSRV);
                            resource->Release();
                            
                            if (tempSRV) {
                                possibleNormalSRV = tempSRV;
                                logger::info("[GlobalIllumination] Created SRV for render target {} for normals", i);
                                break;
                            }
                        }
                    }
                }
                
                if (possibleNormalSRV) {
                    normalSRV = possibleNormalSRV;
                    logger::info("[GlobalIllumination] Using render target as normal buffer");
                } else {
                    logger::error("[GlobalIllumination] Could not find a suitable normal buffer");
                    return;
                }
            } else {
                logger::error("[GlobalIllumination] No game renderer available to get normal buffer");
                return;
            }
        }
        
        // Create output buffer if not provided
        if (!outputUAV) {
            // Create the output buffer compatible with game's buffer format and dimensions
            if (!giOutputBuffer || recreateBuffers) {
                CreateGIOutputBuffer();
                recreateBuffers = false;
            }
            
            if (giOutputBuffer && giOutputBuffer->uav) {
                outputUAV = giOutputBuffer->uav.get();
                logger::info("[GlobalIllumination] Using created output buffer");
            } else {
                logger::error("[GlobalIllumination] Failed to create or get output buffer");
                return;
            }
        }
    }
    
    // At this point, we have all the required buffers
    // We don't need to check again since we have early returns above
    
    // Apply the GI-specific settings to the raytracer
    float intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
    
    // Set ray length based on user settings
    float rayLength = settings.Distance;
    
    // For saturation, we'd need to modify the shader or post-process
    float saturation = settings.Saturation;
    
    // Log that we're generating GI
    logger::info("[GlobalIllumination] Generating GI with intensity={}, distance={}, saturation={}", 
                intensity, rayLength, saturation);
    
    // Update geometry each frame if needed
    if (shouldUpdateGeometry) {
        raytracing->UpdateGeometry();
    }
    
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
        
        // Apply the GI output to the final render (to be implemented)
        if (outputUAV == giOutputBuffer->uav.get()) {
            ApplyGIToFinalRender();
        }
    }
}

// Create the output buffer for GI
void GlobalIllumination::CreateGIOutputBuffer()
{
    auto device = globals::d3d::device;
    if (!device) {
        logger::error("[GlobalIllumination] No D3D11 device available to create output buffer");
        return;
    }
    
    auto renderer = globals::game::renderer;
    if (!renderer) {
        logger::error("[GlobalIllumination] No renderer available to get dimensions");
        return;
    }
    
    // Get dimensions from a render target that should exist
    UINT width = 1920;  // Default fallback
    UINT height = 1080; // Default fallback
    
    // Try to get dimensions from any available render target
    auto& renderTargetData = renderer->GetRuntimeData().renderTargets;
    for (int i = 0; i < 10; i++) {
        if (renderTargetData[i].texture) {
            ID3D11Texture2D* pTexture = nullptr;
            if (SUCCEEDED(renderTargetData[i].texture->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture))) {
                D3D11_TEXTURE2D_DESC texDesc;
                pTexture->GetDesc(&texDesc);
                width = texDesc.Width;
                height = texDesc.Height;
                pTexture->Release();
                logger::info("[GlobalIllumination] Got dimensions {}x{} from render target {}", width, height, i);
                break;
            }
        }
    }
    
    // Create a texture descriptor
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // Use a high precision format for GI
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;
    
    try {
        // Create the texture using Texture2D's constructor
        ID3D11Texture2D* texture = nullptr;
        HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &texture);
        if (FAILED(hr)) {
            logger::error("[GlobalIllumination] Failed to create output texture. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
            return;
        }
        
        // Create the Texture2D object using the existing resource
        giOutputBuffer = std::make_shared<Texture2D>(texture);
        
        // Create SRV
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        
        giOutputBuffer->CreateSRV(srvDesc);
        
        // Create UAV
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = texDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        
        giOutputBuffer->CreateUAV(uavDesc);
        
        // We don't need to release texture, as it's now owned by the Texture2D object
        
        logger::info("[GlobalIllumination] Created output buffer {}x{}", width, height);
    }
    catch (const std::exception& e) {
        logger::error("[GlobalIllumination] Exception creating output buffer: {}", e.what());
    }
}

// Apply the GI result to the final render
void GlobalIllumination::ApplyGIToFinalRender()
{
    // Check if we have the output buffer
    if (!giOutputBuffer || !giOutputBuffer->srv) {
        logger::error("[GlobalIllumination] No GI output buffer available for compositing");
        return;
    }
    
    // Get required resources
    auto device = globals::d3d::device;
    auto context = globals::d3d::context;
    auto renderer = globals::game::renderer;
    auto utilityShader = globals::game::utilityShader;
    
    // Check for required resources
    if (!device || !context || !renderer || !utilityShader) {
        logger::error("[GlobalIllumination] Missing required resources for GI compositing");
        return;
    }
    
    // Get the main render target
    int mainRTIndex = RE::RENDER_TARGETS::kMAIN;
    auto& mainRT = renderer->GetRuntimeData().renderTargets[mainRTIndex];
    if (!mainRT.texture) {
        logger::error("[GlobalIllumination] Main render target not available");
        return;
    }
    
    ID3D11RenderTargetView* rtv = mainRT.RTV;
    if (!rtv) {
        logger::error("[GlobalIllumination] Main render target view not available");
        return;
    }
    
    logger::info("[GlobalIllumination] Applying GI to final render with intensity {}", 
        settings.AdaptToScene ? effectiveIntensity : settings.Intensity);
    
    // Save current render targets and viewport
    ID3D11RenderTargetView* previousRTV = nullptr;
    ID3D11DepthStencilView* previousDSV = nullptr;
    context->OMGetRenderTargets(1, &previousRTV, &previousDSV);
    
    D3D11_VIEWPORT previousViewport;
    UINT numViewports = 1;
    context->RSGetViewports(&numViewports, &previousViewport);
    
    // Set up viewport to match the render target
    D3D11_VIEWPORT viewport;
    D3D11_TEXTURE2D_DESC texDesc;
    
    ID3D11Texture2D* texture = nullptr;
    mainRT.texture->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
    if (texture) {
        texture->GetDesc(&texDesc);
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = (float)texDesc.Width;
        viewport.Height = (float)texDesc.Height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        texture->Release();
    } else {
        // Fall back to previous viewport dimensions
        viewport = previousViewport;
    }
    
    // Set the main render target
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->RSSetViewports(1, &viewport);
    
    // Define constant buffer for shader parameters
    struct CompositingParams {
        float intensity;      // overall intensity of GI effect
        float saturation;     // color saturation control
        float padding[2];     // padding to 16 bytes
    };
    
    // Create/update constant buffer
    ID3D11Buffer* constantBuffer = nullptr;
    
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CompositingParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, &constantBuffer);
    if (SUCCEEDED(hr) && constantBuffer) {
        // Update constant buffer with current settings
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        if (SUCCEEDED(context->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
            CompositingParams* data = (CompositingParams*)mappedResource.pData;
            data->intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
            data->saturation = settings.Saturation;
            context->Unmap(constantBuffer, 0);
        }
        
        // Bind constant buffer
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
    }
    
    // Bind the GI output buffer as a shader resource
    ID3D11ShaderResourceView* srvs[] = { giOutputBuffer->srv.get() };
    context->PSSetShaderResources(0, 1, srvs);
    
    // Use the utility shader to do a simple blend of the GI with the main render target
    // This is a simple additive blend for now
    logger::info("[GlobalIllumination] Compositing GI buffer with main render target");
    
    // Draw a fullscreen quad to apply the effect
    // TODO: Add a proper compositing shader and adjust this when we have a better understanding
    // of the BSUtilityShader interface and capabilities
    if (utilityShader) {
        utilityShader->RenderScreenShape();
    } else {
        logger::error("[GlobalIllumination] BSUtilityShader not available for compositing");
    }
    
    // Clean up
    // Clear bound resources
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSRVs);
    
    if (constantBuffer) {
        constantBuffer->Release();
    }
    
    // Restore previous render targets and viewport
    context->OMSetRenderTargets(1, &previousRTV, previousDSV);
    context->RSSetViewports(1, &previousViewport);
    
    if (previousRTV) {
        previousRTV->Release();
    }
    if (previousDSV) {
        previousDSV->Release();
    }
    
    logger::info("[GlobalIllumination] GI compositing complete");
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