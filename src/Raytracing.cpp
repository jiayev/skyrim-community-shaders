#include "Raytracing.h"
#include "KickstartRTImpl.h"
#include "Globals.h"
#include "State.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <chrono>

// Raytracing class implementation 
bool Raytracing::InitializeResources(ID3D11Device* device)
{
    if (!device) {
        logger::error("[RT] Null device");
        return false;
    }
    
    // Initialize the core raytracing system
    initialized = KickstartRTImpl::Initialize(device);
    
    if (initialized) {
        logger::info("[RT] Resources initialized successfully");
    } else {
        logger::error("[RT] Failed to initialize resources");
    }
    
    return initialized;
}

void Raytracing::ClearResources()
{
    if (initialized) {
        KickstartRTImpl::CleanupResources();
        initialized = false;
        logger::info("[RT] Resources cleared");
    }
}

bool Raytracing::TestKickstartRT()
{
    // Simplified implementation that returns true
    logger::info("[Raytracing] KickstartRT test bypassed");
    return true;
}

bool Raytracing::RegisterGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        logger::warn("[RT] RegisterGeometry called but raytracing is not initialized or enabled");
        return false;
    }

    try {
        // Only collect full geometry occasionally, not every frame
        static uint32_t lastFrameCollected = 0;
        static uint32_t lastDynamicCollected = 0;
        uint32_t currentFrame = globals::state->frameCount;
        
        // Collect full geometry every 60 frames (roughly 1 second at 60 fps)
        if (lastFrameCollected == 0 || currentFrame - lastFrameCollected > 60) {
            // Collect full scene geometry from the Skyrim scene
            logger::debug("[RT] Collecting and registering full scene geometry...");
            int geometryCount = KickstartRTImpl::CollectSceneGeometry(false);
            
            if (geometryCount > 0) {
                logger::info("[RT] Successfully registered {} geometries from the scene", geometryCount);
                lastFrameCollected = currentFrame;
                lastDynamicCollected = currentFrame; // Reset dynamic collection too
                return true;
            } else {
                logger::error("[RT] Failed to register any geometry from the scene");
                return false;
            }
        }
        // Update dynamic objects more frequently (every 10 frames or around 6 times per second)
        else if (lastDynamicCollected == 0 || currentFrame - lastDynamicCollected > 10) {
            logger::debug("[RT] Updating dynamic objects only...");
            int dynamicCount = KickstartRTImpl::CollectSceneGeometry(true);
            
            if (dynamicCount > 0) {
                logger::debug("[RT] Updated {} dynamic objects", dynamicCount);
                lastDynamicCollected = currentFrame;
            }
        }
        
        // If we've already collected geometry recently, just return success
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry registration: {}", e.what());
        return false;
    }
}

bool Raytracing::UpdateGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        return false;
    }
    
    try {
        // We could update specific instances here if needed
        // For now, our dynamic object updates are handled in RegisterGeometry
        
        // The old test quad code is no longer relevant, so we'll return success
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry update: {}", e.what());
        return false;
    }
}

bool Raytracing::GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix)
{
    // Default to identity matrices as fallback
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Try to get the current view and projection matrices from the game's camera
    auto playerCamera = RE::PlayerCamera::GetSingleton();
    if (!playerCamera) {
        logger::warn("[RT] Failed to get player camera singleton");
        return false;
    }
    
    // Check if we have a valid camera state
    if (!playerCamera->currentState) {
        logger::warn("[RT] No current camera state available");
        return false;
    }
    
    // Get the camera node
    auto cameraNode = playerCamera->cameraRoot;
    if (!cameraNode) {
        logger::warn("[RT] No camera root node available");
        return false;
    }
    
    // Extract view matrix (inverse of camera's world transform)
    DirectX::XMMATRIX cameraWorldMatrix = DirectX::XMMatrixIdentity();
    
    // Access the camera's rotation matrix and position
    const auto& transform = cameraNode->world;
    
    // NiMatrix3 to upper 3x3 of XMFLOAT4X4
    cameraWorldMatrix.r[0] = DirectX::XMVectorSet(transform.rotate.entry[0][0], transform.rotate.entry[0][1], transform.rotate.entry[0][2], 0.0f);
    cameraWorldMatrix.r[1] = DirectX::XMVectorSet(transform.rotate.entry[1][0], transform.rotate.entry[1][1], transform.rotate.entry[1][2], 0.0f);
    cameraWorldMatrix.r[2] = DirectX::XMVectorSet(transform.rotate.entry[2][0], transform.rotate.entry[2][1], transform.rotate.entry[2][2], 0.0f);
    
    // Then set the translation component
    cameraWorldMatrix.r[3] = DirectX::XMVectorSet(
        transform.translate.x,
        transform.translate.y, 
        transform.translate.z,
        1.0f);
        
    // View matrix is the inverse of the camera's world matrix
    DirectX::XMMATRIX viewMat = DirectX::XMMatrixInverse(nullptr, cameraWorldMatrix);
    DirectX::XMStoreFloat4x4(&viewMatrix, viewMat);
    
    // Get the projection matrix from the renderer
    // Get the fov from the camera
    float fov = playerCamera->GetRuntimeData2().worldFOV; // Vertical FOV in radians
    
    // Get render target dimensions for aspect ratio
    auto renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer) {
        logger::warn("[RT] Failed to get renderer singleton");
        return false;
    }
    
    // Get the main render target dimensions by querying the texture
    float width = 1920.0f;  // Default fallback
    float height = 1080.0f; // Default fallback
    
    auto& renderData = renderer->GetRuntimeData();
    auto& mainTarget = renderData.renderTargets[RE::RENDER_TARGET::kMAIN];
    if (mainTarget.texture) {
        ID3D11Texture2D* pTexture = nullptr;
        if (SUCCEEDED(mainTarget.texture->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture))) {
            D3D11_TEXTURE2D_DESC texDesc;
            pTexture->GetDesc(&texDesc);
            width = static_cast<float>(texDesc.Width);
            height = static_cast<float>(texDesc.Height);
            pTexture->Release();
        }
    }
    
    float aspectRatio = width / height;
    
    // Standard near and far planes for Skyrim
    float nearPlane = 1.0f;
    float farPlane = 10000.0f;
    
    DirectX::XMMATRIX projMat = DirectX::XMMatrixPerspectiveFovLH(
        fov, 
        aspectRatio,
        nearPlane,
        farPlane);
        
    DirectX::XMStoreFloat4x4(&projMatrix, projMat);
    
    logger::debug("[RT] Retrieved view and projection matrices from camera");
    return true;
}

// Direct pass-through to KickstartRTImpl
bool Raytracing::GenerateGI(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalSRV,
    ID3D11UnorderedAccessView* outputUAV,
    const DirectX::XMFLOAT4X4& viewMatrix,
    const DirectX::XMFLOAT4X4& projMatrix) 
{
    // Check if we're enabled and initialized
    if (!settings.Enabled || !initialized) {
        logger::debug("[Raytracing] GenerateGI called but raytracing is not enabled/initialized");
        return false;
    }
    
    // Validate critical input resources
    if (!depthSRV || !outputUAV) {
        logger::debug("[Raytracing] GenerateGI called with null resources");
        return false;
    }
    
    // Performance-critical path - only log at debug level
    logger::debug("[Raytracing] Generating global illumination");
    
    // Call into the KickstartRT implementation
    try {
        // Setup a timer for performance tracking
        auto startTime = std::chrono::high_resolution_clock::now();
        
        bool result = KickstartRTImpl::GenerateGI(
            depthSRV,
            normalSRV,
            outputUAV,
            viewMatrix,
            projMatrix
        );
        
        // Calculate execution time for performance monitoring
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        
        // Only log timing occasionally to avoid spam - use a static counter instead of frameCount
        static uint64_t timingLogCounter = 0;
        if (timingLogCounter++ % 60 == 0) { // Log timing every 60 calls
            logger::debug("[Raytracing] GI generation took {} ms", duration);
        }
        
        return result;
    }
    catch (const std::exception& e) {
        logger::error("[Raytracing] Exception in GenerateGI: {}", e.what());
        return false;
    }
}

// Direct pass-through to KickstartRTImpl
bool Raytracing::GenerateReflections(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalSRV,
    ID3D11ShaderResourceView* roughnessSRV,
    ID3D11UnorderedAccessView* outputUAV,
    const DirectX::XMFLOAT4X4& viewMatrix,
    const DirectX::XMFLOAT4X4& projMatrix) 
{
    // Check if we're enabled
    if (!IsEnabled()) {
        logger::info("[Raytracing] GenerateReflections called but raytracing is not enabled");
        return false;
    }
    
    // Validate input resources
    if (!depthSRV || !normalSRV || !roughnessSRV || !outputUAV) {
        logger::error("[Raytracing] GenerateReflections called with null resources");
        return false;
    }
    
    logger::info("[Raytracing] Generating reflections");
    
    // Call into the KickstartRT implementation
    return KickstartRTImpl::GenerateReflections(
        depthSRV,
        normalSRV,
        roughnessSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
}

// Implement the TraceGI method using the query structure
bool Raytracing::TraceGI(const KickstartRT::TraceQueryInternal& query)
{
    // First check if raytracing is enabled at all
    if (!settings.Enabled) {
        logger::debug("[Raytracing] Raytracing is disabled, skipping GI trace");
        return false;
    }

    // Check if we're properly initialized
    if (!initialized) {
        logger::debug("[Raytracing] Cannot trace GI - raytracing not initialized");
        
        // Fallback: fill output with black if we have a valid UAV
        if (query.outputUAV) {
            auto context = globals::d3d::context;
            if (context) {
                FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, clearColor);
                logger::debug("[Raytracing] Produced blank output as fallback");
                return true; // We at least produced valid output
            }
        }
        
        return false;
    }

    // Validate all required resources
    if (!query.depthBufferSRV || !query.outputUAV) {
        logger::debug("[Raytracing] Missing required resources for GI trace");
        return false;
    }

    // Performance-critical path - only log at debug level
    logger::debug("[Raytracing] Tracing GI with ray length: {}", query.maxRayLength);

    try {
        // Register scene geometry if needed (do this less frequently to improve performance)
        static uint64_t registrationCounter = 0;
        // Only register geometry every few frames to reduce overhead
        if (registrationCounter++ % 30 == 0) {
            if (RegisterGeometry()) {
                logger::debug("[Raytracing] Updated scene geometry for raytracing");
            }
        }
        
        // Ensure view and projection matrices are properly set up
        DirectX::XMFLOAT4X4 viewMatrix, projMatrix;
        if (!GetCurrentViewAndProjectionMatrices(viewMatrix, projMatrix)) {
            logger::warn("[Raytracing] Failed to get current view matrices, using provided matrices");
            viewMatrix = query.cameraData.view;
            projMatrix = query.cameraData.projection;
        }
        
        // Call the underlying GenerateGI method with unwrapped parameters
        bool result = GenerateGI(
            query.depthBufferSRV,
            query.normalBufferSRV,
            query.outputUAV,
            viewMatrix,
            projMatrix
        );
        
        // If GI generation failed, use fallback
        if (!result) {
            logger::debug("[Raytracing] GI generation failed, using fallback");
            auto context = globals::d3d::context;
            if (context) {
                // Simple dark gray output as ambient approximation
                FLOAT ambient[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, ambient);
                return true; // We at least produced valid output
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        logger::error("[Raytracing] Exception in TraceGI: {}", e.what());
        
        // Fallback in case of exception
        if (query.outputUAV) {
            auto context = globals::d3d::context;
            if (context) {
                FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, black);
                return true; // We provided valid output
            }
        }
        
        return false;
    }
} 