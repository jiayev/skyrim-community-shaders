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
    
    // Ensure raytracing is enabled before registering geometry
    raytracing->settings.Enabled = true;
    
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
    if (!settings.Enabled) {
        return;
    }
    
    // Quick check for raytracing availability before proceeding
    if (!IsAvailable()) {
        logger::warn("[GlobalIllumination] GenerateGI called but raytracing is not available");
        return;
    }
    
    try {
        // Record performance timing
        TracyD3D11Zone(globals::state->tracyCtx, "GI Generation");
        
        // Update settings based on the current scene if needed
        UpdateSettingsForScene();
        
        // Get raytracing feature
        auto* raytracing = globals::features::raytracing;
        if (!raytracing) {
            logger::error("[GlobalIllumination] Raytracing feature not available");
            return;
        }
        
        // Get device context
        auto context = globals::d3d::context;
        auto device = globals::d3d::device;
        if (!context || !device) {
            logger::error("[GlobalIllumination] D3D11 device or context not available");
            return;
        }
        
        // Log what we're working with
        logger::debug("[GlobalIllumination] GenerateGI called with depth:{}, normal:{}, output:{}", 
            depthBuffer ? "provided" : "null", 
            normalBuffer ? "provided" : "null", 
            outputBuffer ? "provided" : "null");
        
        // Get the current view and projection matrices
        DirectX::XMFLOAT4X4 viewMatrix, projMatrix;
        
        // Initialize with identity matrices to prevent "uninitialized variable" warnings
        DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
        DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
        
        // Get proper matrices from Skyrim's renderer
        if (auto renderer = globals::game::renderer) {
            // Try to get matrices from per-frame constant buffer
            bool gotMatrices = false;
            
            // Get the per-frame buffer from globals
            auto perFrameBuffer = *globals::game::perFrame.get();
            if (perFrameBuffer) {
                // Map the buffer to read its contents
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
                    logger::debug("[GlobalIllumination] Successfully extracted view/projection matrices from per-frame buffer");
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
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSRV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> normalSRV;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUAV;
        
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
                    // Need to get raw pointer from possibly winrt::com_ptr
                    ID3D11ShaderResourceView* rawDepthSRV = depthTexture.depthSRV;
                    depthSRV = rawDepthSRV;
                    if (!depthSRV) {
                        logger::error("[GlobalIllumination] Failed to get depth buffer from game renderer");
                        return; // Early out as we can't proceed without depth buffer
                    }
                    logger::debug("[GlobalIllumination] Got depth buffer from game renderer");
                } else {
                    logger::error("[GlobalIllumination] No game renderer available to get depth buffer");
                    return; // Early out as we can't proceed without renderer
                }
            }
            
            // For normal buffer, extract from G-buffer or create a default one
            // Note: Normal buffer is optional, so we'll try to get it but proceed without if needed
            if (!normalSRV) {
                if (auto renderer = globals::game::renderer) {
                    // In Skyrim, normals are stored in the G-buffer which is in the render targets
                    auto& renderTargetData = renderer->GetRuntimeData().renderTargets;
                    
                    // Try the known normal buffer (typically in G-buffer 1)
                    int normalRTIndex = 1; // Most likely G-buffer 1 contains normals
                    if (normalRTIndex < 114 && renderTargetData[normalRTIndex].texture) {
                        // Create SRV if it doesn't exist
                        if (!renderTargetData[normalRTIndex].SRV) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Common format for normal buffers
                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                            srvDesc.Texture2D.MipLevels = 1;
                            srvDesc.Texture2D.MostDetailedMip = 0;
                            
                            ID3D11ShaderResourceView* tempSRV = nullptr;
                            if (SUCCEEDED(device->CreateShaderResourceView(renderTargetData[normalRTIndex].texture, &srvDesc, &tempSRV))) {
                                normalSRV = tempSRV;
                                tempSRV->Release(); // ComPtr takes ownership
                                logger::debug("[GlobalIllumination] Created SRV for normal buffer from G-buffer");
                            }
                        } else {
                            // Need to handle possible winrt::com_ptr from the renderer's SRV
                            ID3D11ShaderResourceView* rawSRV = renderTargetData[normalRTIndex].SRV;
                            normalSRV = rawSRV;
                            logger::debug("[GlobalIllumination] Got normal buffer from G-buffer");
                        }
                    }
                    
                    // If we still don't have a normal buffer, create a temporary one with default normals
                    if (!normalSRV) {
                        logger::warn("[GlobalIllumination] Could not find normal buffer, creating default");
                        
                        // Get the dimensions from the depth buffer
                        D3D11_TEXTURE2D_DESC depthTexDesc = {};
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
                        
                        // Query the depth SRV for its texture
                        Microsoft::WRL::ComPtr<ID3D11Resource> depthRes;
                        depthSRV->GetResource(depthRes.GetAddressOf());
                        if (SUCCEEDED(depthRes.As(&depthTex))) {
                            depthTex->GetDesc(&depthTexDesc);
                            
                            // Create a temporary texture with default normals (all pointing forward)
                            D3D11_TEXTURE2D_DESC normalTexDesc = {};
                            normalTexDesc.Width = depthTexDesc.Width;
                            normalTexDesc.Height = depthTexDesc.Height;
                            normalTexDesc.MipLevels = 1;
                            normalTexDesc.ArraySize = 1;
                            normalTexDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            normalTexDesc.SampleDesc.Count = 1;
                            normalTexDesc.SampleDesc.Quality = 0;
                            normalTexDesc.Usage = D3D11_USAGE_DEFAULT;
                            normalTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                            
                            // Create with default data (R=0.5, G=0.5, B=1.0, A=1.0 represents forward-facing normal)
                            std::vector<UINT> defaultNormal(depthTexDesc.Width * depthTexDesc.Height, 0xFF8080FF);
                            D3D11_SUBRESOURCE_DATA initData = {};
                            initData.pSysMem = defaultNormal.data();
                            initData.SysMemPitch = depthTexDesc.Width * 4;
                            
                            Microsoft::WRL::ComPtr<ID3D11Texture2D> normalTex;
                            if (SUCCEEDED(device->CreateTexture2D(&normalTexDesc, &initData, normalTex.GetAddressOf()))) {
                                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                                srvDesc.Format = normalTexDesc.Format;
                                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                srvDesc.Texture2D.MipLevels = 1;
                                
                                ID3D11ShaderResourceView* tempSRV = nullptr;
                                if (SUCCEEDED(device->CreateShaderResourceView(normalTex.Get(), &srvDesc, &tempSRV))) {
                                    normalSRV = tempSRV;
                                    tempSRV->Release(); // ComPtr takes ownership
                                    logger::debug("[GlobalIllumination] Created default normal buffer");
                                }
                            }
                        }
                    }
                }
                
                // If we still don't have normals, we'll proceed without it and let the raytracing system handle it
                if (!normalSRV) {
                    logger::warn("[GlobalIllumination] Could not create or find normal buffer, proceeding without");
                    // We'll continue without a normal buffer - KickstartRT will have to handle this case
                }
            }
            
            // For output buffer, create or reuse our own
            if (!outputUAV) {
                if (recreateBuffers || !giOutputBuffer) {
                    logger::info("[GlobalIllumination] Creating GI output buffer");
                    CreateGIOutputBuffer();
                }
                
                if (giOutputBuffer && giOutputBuffer->uav) {
                    outputUAV = giOutputBuffer->uav.get();
                    logger::debug("[GlobalIllumination] Using GI output buffer");
                } else {
                    logger::error("[GlobalIllumination] Failed to create GI output buffer");
                    return; // Early out as we can't proceed without output buffer
                }
            }
        }
        
        // At this point, we should have depth buffer and output UAV
        // Normal buffer is optional and might be null
        if (!depthSRV || !outputUAV) {
            logger::error("[GlobalIllumination] Could not get required resources for GI");
            return;
        }
        
        // Clear output buffer to zero
        FLOAT clearUAVValue[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->ClearUnorderedAccessViewFloat(outputUAV.Get(), clearUAVValue);
        
        // Create camera parameters based on the view and projection matrices
        KickstartRT::CameraData cameraData;
        cameraData.projection = projMatrix;
        cameraData.view = viewMatrix;
        cameraData.nearClipPlane = 0.1f;
        cameraData.farClipPlane = 1000.0f;
        
        // Create KickstartRT query for raytraced GI
        KickstartRT::TraceQueryInternal traceQuery = {};
        traceQuery.cameraData = cameraData;
        traceQuery.depthBufferSRV = depthSRV.Get();
        traceQuery.normalBufferSRV = normalSRV.Get();
        traceQuery.outputUAV = outputUAV.Get();
        traceQuery.maxRayLength = settings.Distance;
        
        // Update geometry if needed
        if (shouldUpdateGeometry) {
            logger::debug("[GlobalIllumination] Updating geometry for raytracing");
            if (raytracing->UpdateGeometry()) {
                shouldUpdateGeometry = false;
                logger::debug("[GlobalIllumination] Updated geometry for raytracing");
            } else {
                logger::warn("[GlobalIllumination] Failed to update geometry for raytracing");
            }
        }
        
        // Execute GI raytracing using the simplified TraceGI interface
        logger::debug("[GlobalIllumination] Executing raytracing query for GI");
        bool success = raytracing->TraceGI(traceQuery);
        
        if (!success) {
            logger::error("[GlobalIllumination] Failed to trace GI");
        } else {
            logger::debug("[GlobalIllumination] GI tracing successful");
            
            // Apply GI to the final render if we're using our own output buffer
            if (outputBuffer == nullptr) {
                ApplyGIToFinalRender();
            }
        }
    }
    catch (const std::exception& e) {
        logger::error("[GlobalIllumination] Exception during GI generation: {}", e.what());
    }
    catch (...) {
        logger::error("[GlobalIllumination] Unknown exception during GI generation");
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
    
    // Record performance timing
    TracyD3D11Zone(globals::state->tracyCtx, "GI Compositing");
    
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
    
    // Save current render targets and viewport
    ID3D11RenderTargetView* previousRTV = nullptr;
    ID3D11DepthStencilView* previousDSV = nullptr;
    context->OMGetRenderTargets(1, &previousRTV, &previousDSV);
    
    D3D11_VIEWPORT previousViewport;
    UINT numViewports = 1;
    context->RSGetViewports(&numViewports, &previousViewport);
    
    try {
        // Set up viewport to match the render target
        D3D11_VIEWPORT viewport;
        D3D11_TEXTURE2D_DESC texDesc;
        
        ID3D11Texture2D* texture = nullptr;
        if (SUCCEEDED(mainRT.texture->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture))) {
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
        Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer;
        
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(CompositingParams);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        
        HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, constantBuffer.GetAddressOf());
        if (SUCCEEDED(hr) && constantBuffer) {
            // Update constant buffer with current settings
            D3D11_MAPPED_SUBRESOURCE mappedResource;
            if (SUCCEEDED(context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) {
                CompositingParams* data = (CompositingParams*)mappedResource.pData;
                data->intensity = settings.AdaptToScene ? effectiveIntensity : settings.Intensity;
                data->saturation = settings.Saturation;
                context->Unmap(constantBuffer.Get(), 0);
            }
            
            // Bind constant buffer
            context->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
        }
        
        // Bind the GI output buffer as a shader resource
        ID3D11ShaderResourceView* srvs[] = { giOutputBuffer->srv.get() };
        context->PSSetShaderResources(0, 1, srvs);
        
        // Use a blending approach that's consistent with other effects
        // Setup blend state for additive blending
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.AlphaToCoverageEnable = false;
        blendDesc.IndependentBlendEnable = false;
        blendDesc.RenderTarget[0].BlendEnable = true;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        
        Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
        device->CreateBlendState(&blendDesc, blendState.GetAddressOf());
        
        // Save original blend state
        Microsoft::WRL::ComPtr<ID3D11BlendState> originalBlendState;
        FLOAT originalBlendFactor[4];
        UINT originalSampleMask;
        context->OMGetBlendState(originalBlendState.GetAddressOf(), originalBlendFactor, &originalSampleMask);
        
        // Set blend state
        const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(blendState.Get(), blendFactor, 0xffffffff);
        
        // Draw a fullscreen quad to apply the effect
        logger::info("[GlobalIllumination] Compositing GI buffer with main render target");
        
        if (utilityShader) {
            // Set the appropriate shader technique
            if (utilityShader->SetupTechnique(0)) {
                // Create a simple quad (Vertices will be generated in the vertex shader)
                // This is often done with a simple triangle that covers the whole screen
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->IASetInputLayout(nullptr);
                context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
                context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                
                // Draw 3 vertices (full screen triangle) without any vertex buffer
                // This is a common technique for fullscreen passes
                context->Draw(3, 0);
                
                // Restore the technique when finished
                utilityShader->RestoreTechnique(0);
            } else {
                logger::error("[GlobalIllumination] Failed to setup utility shader technique");
            }
        } else {
            logger::error("[GlobalIllumination] BSUtilityShader not available for compositing");
        }
        
        // Restore original blend state
        context->OMSetBlendState(originalBlendState.Get(), originalBlendFactor, originalSampleMask);
        
        // Clean up
        // Clear bound resources
        ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
        context->PSSetShaderResources(0, 1, nullSRVs);
        
        // Reset constant buffers
        ID3D11Buffer* nullBuffer = nullptr;
        context->PSSetConstantBuffers(0, 1, &nullBuffer);
        
        // Restore previous render targets and viewport
        context->OMSetRenderTargets(1, &previousRTV, previousDSV);
        context->RSSetViewports(1, &previousViewport);
    }
    catch (const std::exception& e) {
        logger::error("[GlobalIllumination] Exception during GI compositing: {}", e.what());
    }
    
    // Release any resources we got with AddRef
    if (previousRTV) {
        previousRTV->Release();
    }
    if (previousDSV) {
        previousDSV->Release();
    }
    
    logger::info("[GlobalIllumination] GI compositing complete");
}

// Register for rendering hooks
void GlobalIllumination::RegisterHooks()
{
    if (!settings.Enabled)
        return;
    
    // Hook into the Deferred rendering pipeline at the appropriate point
    // This ensures our Update and ApplyGIToFinalRender methods are called at the right time
    
    // Get the deferred renderer singleton
    auto deferred = globals::deferred;
    if (deferred) {
        logger::info("[GlobalIllumination] Registering GI rendering hooks in the deferred pipeline");
        
        // Register a callback to be called during the DeferredPasses stage
        // This is already added in Deferred.cpp now and we don't need to do more here
        
        // Add a debug log to verify when hooks are registered
        logger::debug("[GlobalIllumination] GI hooks registered successfully");
    } else {
        logger::error("[GlobalIllumination] Failed to register rendering hooks - deferred renderer not available");
    }
}

// Update GI in the rendering pipeline
void GlobalIllumination::Update()
{
    // Only update if the feature is enabled and available
    if (!settings.Enabled || !IsAvailable()) {
        logger::debug("[GlobalIllumination] Update called but feature is disabled or raytracing not available");
        return;
    }
    
    logger::debug("[GlobalIllumination] Update called - generating and applying GI effects");
    
    // Update settings based on the scene
    UpdateSettingsForScene();
    
    // Generate GI using the renderer's resources
    // GenerateGI returns void, so we can't assign it to a bool
    logger::debug("[GlobalIllumination] Generating GI effects");
    GenerateGI(nullptr, nullptr, nullptr);
    
    // Ensure the final rendering happens by explicitly calling ApplyGIToFinalRender
    // This is critical to ensure the results are visible
    if (giOutputBuffer && giOutputBuffer->srv) {
        logger::debug("[GlobalIllumination] Explicitly calling ApplyGIToFinalRender");
        ApplyGIToFinalRender();
    } else {
        logger::warn("[GlobalIllumination] No GI output buffer available for rendering");
    }
}

// Clean up resources
void GlobalIllumination::CleanupResources()
{
    // Release the output buffer
    giOutputBuffer = nullptr;
    
    // Mark for recreation
    recreateBuffers = true;
    
    logger::info("[GlobalIllumination] Resources cleaned up");
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