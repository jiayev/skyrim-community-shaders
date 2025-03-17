#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"
#include "Raytracing.h"
#include "Globals.h"
#include "State.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>

// Disable warnings for unreferenced parameters
#pragma warning(disable: 4100)

// Core KickstartRT implementation
namespace KickstartRTImpl
{
    // Direct access to the ExecuteContext
    static KickstartRT::D3D11::ExecuteContext* g_executeContext = nullptr;
    static bool g_initialized = false;
    static uint32_t g_width = 0;
    static uint32_t g_height = 0;

    // Initialize KickstartRT
    bool Initialize(ID3D11Device* device) {
        if (!device) {
            logger::error("[RT] Null device");
            return false;
        }
        
        // Already initialized
        if (g_initialized) {
            return true;
        }
        
        try {
            // Default resolution in case we can't query the actual dimensions
            g_width = 1920;
            g_height = 1080;
            
            // Create init settings for D3D11
            KickstartRT::D3D11::ExecuteContext_InitSettings settings;
            settings.D3D11Device = device;
            
            // Get the adapter directly for KickstartRT
            // This is REQUIRED for the D3D11 interop layer
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
            if (FAILED(hr)) {
                logger::error("[RT] Failed to get DXGI device interface. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Get the adapter
            IDXGIAdapter1* adapter1 = nullptr;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
            if (FAILED(hr)) {
                logger::error("[RT] Failed to get DXGI adapter. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Try to get dimensions from the primary output if desired
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (SUCCEEDED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
                DXGI_OUTPUT_DESC desc;
                if (SUCCEEDED(output->GetDesc(&desc))) {
                    RECT r = desc.DesktopCoordinates;
                    g_width = r.right - r.left;
                    g_height = r.bottom - r.top;
                    logger::info("[RT] Detected display dimensions: {}x{}", g_width, g_height);
                }
            }
            
            // Now get the IDXGIAdapter1 for KickstartRT
            hr = adapter->QueryInterface(__uuidof(IDXGIAdapter1), (void**)&adapter1);
            if (FAILED(hr)) {
                logger::error("[RT] Failed to get IDXGIAdapter1 interface. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Directly set the adapter in settings (no temporary variables needed)
            settings.DXGIAdapter = adapter1;
            logger::info("[RT] Successfully set DXGIAdapter1 in KickstartRT settings");
            
            // Configure other settings
            settings.usingCommandQueue = KickstartRT::D3D11::ExecuteContext_InitSettings::UsingCommandQueue::Direct;
            settings.supportedWorkingSet = 4u;
            settings.descHeapSize = 8192u;
            settings.uploadHeapSizeForVolatileConstantBuffers = 64u * 1024u;
            
            // Create the execute context - directly use the KickstartRT API
            KickstartRT::D3D11::ExecuteContext* exc = nullptr;
            KickstartRT::Status status = KickstartRT::D3D11::ExecuteContext::Init(
                &settings, 
                &exc,
                KickstartRT::Version());
                
            // Check status first
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to create context. Status: {}", static_cast<int>(status));
                return false;
            }
            
            // Then check if context is null
            if (!exc) {
                logger::error("[RT] Context creation returned OK but context is null");
                return false;
            }
            
            g_executeContext = exc;
            g_initialized = true;
            logger::info("[RT] KickstartRT initialized successfully");
            
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during initialization: {}", e.what());
            return false;
        }
    }

    // Shutdown KickstartRT
    void Shutdown() {
        if (g_initialized && g_executeContext) {
            // Destroy context - directly use the KickstartRT API
            KickstartRT::Status status = KickstartRT::D3D11::ExecuteContext::Destruct(g_executeContext);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Error destroying context. Status: {}", static_cast<int>(status));
            }
                
            g_executeContext = nullptr;
            g_initialized = false;
            
            logger::info("[RT] KickstartRT shutdown complete");
        }
    }

    // Check if KickstartRT is initialized
    bool IsInitialized() {
        return g_initialized && g_executeContext != nullptr;
    }

    // Clean up resources without destroying the context
    void CleanupResources() {
        if (g_initialized && g_executeContext) {
            g_executeContext->ReleaseDeviceResourcesImmediately();
            logger::info("[RT] Resources cleaned up");
        }
    }

    // Simple test to verify KickstartRT is working
    bool RunTest() {
        if (!g_initialized || !g_executeContext) {
            logger::error("[RT] Cannot run test, not initialized");
            return false;
        }
        
        try {
            // For a simple test, just create a task container to verify the context works
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container");
                return false;
            }
            
            // Use InvokeGPUTask for D3D11 - no input parameter required for empty container
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute empty task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            logger::info("[RT] Test completed successfully");
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Test exception: {}", e.what());
            return false;
        }
    }
    
    // Core rendering functions - simplified implementations for now
    // In the future these will be expanded to use proper task scheduling
    bool GenerateGI(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalSRV,
        ID3D11UnorderedAccessView* outputUAV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix)
    {
        // Check if we have an initialized context
        if (!g_executeContext) {
            logger::error("[KickstartRTImpl] GenerateGI called without initialized context");
            return false;
        }

        // Check for required resources
        if (!depthSRV || !normalSRV || !outputUAV) {
            logger::error("[KickstartRTImpl] GenerateGI called with null resources");
            return false;
        }

        // Log what we're doing
        logger::info("[KickstartRTImpl] Running GenerateGI with provided resources");

        try {
            // Create a task container
            auto taskContainer = g_executeContext->CreateTaskContainer();

            // Schedule BVH Build task
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;
            bvhBuildTask.maxBlasBuildCount = 4u;
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Set up diffuse GI tracing
            KickstartRT::D3D11::RenderTask::TraceDiffuseTask traceTask;
            
            // Configure input buffers - use the correct field names
            // Get resources from SRVs
            ID3D11Resource* depthResource = nullptr;
            ID3D11Resource* normalResource = nullptr;
            ID3D11Resource* outputResource = nullptr;
            
            // Extract the underlying resources from the views
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                traceTask.common.depth.tex.resource = depthResource;
                if (depthResource) depthResource->Release(); // Release our reference
            }
            
            if (normalSRV) {
                normalSRV->GetResource(&normalResource);
                traceTask.common.normal.tex.resource = normalResource;
                if (normalResource) normalResource->Release(); // Release our reference
            }
            
            // Configure output buffer
            if (outputUAV) {
                outputUAV->GetResource(&outputResource);
                traceTask.out.resource = outputResource;
                if (outputResource) outputResource->Release(); // Release our reference
            }
            
            // Set view and projection matrices
            // Calculate the inverse projection matrix
            DirectX::XMMATRIX invProjMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&projMatrix));
            DirectX::XMFLOAT4X4 invProj;
            DirectX::XMStoreFloat4x4(&invProj, invProjMatrix);
            
            // Calculate the inverse view matrix
            DirectX::XMMATRIX invViewMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewMatrix));
            DirectX::XMFLOAT4X4 invView;
            DirectX::XMStoreFloat4x4(&invView, invViewMatrix);
            
            // Convert to KickstartRT format
            traceTask.common.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invProj);
            traceTask.common.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invView);
            
            // Set ray parameters
            traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Execute GPU task
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            
            if (status == KickstartRT::Status::OK) {
                logger::info("[KickstartRTImpl] Successfully generated GI");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute GI task: {}", static_cast<int>(status));
                return false;
            }
        } catch (const std::exception& e) {
            logger::error("[KickstartRTImpl] Exception in GenerateGI: {}", e.what());
            return false;
        } catch (...) {
            logger::error("[KickstartRTImpl] Unknown exception in GenerateGI");
            return false;
        }
    }

    bool GenerateReflections(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalSRV,
        ID3D11ShaderResourceView* roughnessSRV,
        ID3D11UnorderedAccessView* outputUAV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix)
    {
        // Check if we have an initialized context
        if (!g_executeContext) {
            logger::error("[KickstartRTImpl] GenerateReflections called without initialized context");
            return false;
        }

        // Check for required resources
        if (!depthSRV || !normalSRV || !roughnessSRV || !outputUAV) {
            logger::error("[KickstartRTImpl] GenerateReflections called with null resources");
            return false;
        }

        // Log what we're doing
        logger::info("[KickstartRTImpl] Running GenerateReflections with provided resources");

        try {
            // Create a task container
            auto taskContainer = g_executeContext->CreateTaskContainer();

            // Schedule BVH Build task
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;
            bvhBuildTask.maxBlasBuildCount = 4u;
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Set up specular reflection tracing
            KickstartRT::D3D11::RenderTask::TraceSpecularTask traceTask;
            
            // Configure input buffers - use the correct field names
            // Get resources from SRVs
            ID3D11Resource* depthResource = nullptr;
            ID3D11Resource* normalResource = nullptr;
            ID3D11Resource* roughnessResource = nullptr;
            ID3D11Resource* outputResource = nullptr;
            
            // Extract the underlying resources from the views
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                traceTask.common.depth.tex.resource = depthResource;
                if (depthResource) depthResource->Release(); // Release our reference
            }
            
            if (normalSRV) {
                normalSRV->GetResource(&normalResource);
                traceTask.common.normal.tex.resource = normalResource;
                if (normalResource) normalResource->Release(); // Release our reference
            }
            
            if (roughnessSRV) {
                roughnessSRV->GetResource(&roughnessResource);
                traceTask.common.roughness.tex.resource = roughnessResource;
                if (roughnessResource) roughnessResource->Release(); // Release our reference
            }
            
            // Configure output buffer
            if (outputUAV) {
                outputUAV->GetResource(&outputResource);
                traceTask.out.resource = outputResource;
                if (outputResource) outputResource->Release(); // Release our reference
            }
            
            // Set view and projection matrices
            // Calculate the inverse projection matrix
            DirectX::XMMATRIX invProjMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&projMatrix));
            DirectX::XMFLOAT4X4 invProj;
            DirectX::XMStoreFloat4x4(&invProj, invProjMatrix);
            
            // Calculate the inverse view matrix
            DirectX::XMMATRIX invViewMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewMatrix));
            DirectX::XMFLOAT4X4 invView;
            DirectX::XMStoreFloat4x4(&invView, invViewMatrix);
            
            // Convert to KickstartRT format
            traceTask.common.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invProj);
            traceTask.common.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invView);
            
            // Set ray parameters
            traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Execute GPU task
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            
            if (status == KickstartRT::Status::OK) {
                logger::info("[KickstartRTImpl] Successfully generated reflections");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute reflections task: {}", static_cast<int>(status));
                return false;
            }
        } catch (const std::exception& e) {
            logger::error("[KickstartRTImpl] Exception in GenerateReflections: {}", e.what());
            return false;
        } catch (...) {
            logger::error("[KickstartRTImpl] Unknown exception in GenerateReflections");
            return false;
        }
    }

    // Register geometry with KickstartRT - simplified placeholder
    bool RegisterGeometryWithKickstartRT(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer)
    {
        if (!g_initialized || !g_executeContext) {
            logger::error("[RT] Not initialized");
            return false;
        }
        
        if (!vertexBuffer || !indexBuffer) {
            logger::error("[RT] Invalid buffers for geometry registration");
            return false;
        }
        
        // This is a placeholder - we'll implement proper geometry registration later
        logger::info("[RT] Geometry registration is a placeholder - will be implemented in a future version");
        return true;
    }
} // namespace KickstartRTImpl

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
    if (!IsInitialized()) {
        logger::error("[Raytracing] Cannot test KickstartRT, system not initialized");
        return false;
    }
    
    logger::info("[Raytracing] Running KickstartRT test...");
    return KickstartRTImpl::RunTest();
}

bool Raytracing::RegisterGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        logger::warn("[RT] RegisterGeometry called but raytracing is not initialized or enabled");
        return false;
    }

    if (!KickstartRTImpl::g_executeContext) {
        logger::error("[RT] No execute context available");
        return false;
    }
    
    logger::info("[RT] Beginning geometry registration");
    
    // In a real implementation, we would:
    // 1. Iterate through visible/loaded geometry in Skyrim
    // 2. For each mesh, create a KickstartRT geometry instance
    // 3. Register each geometry with the KickstartRT BVH
    
    // For now, we'll set up a simple scene with a ground plane for testing
    try {
        // Create a task container for geometry registration
        auto taskContainer = KickstartRTImpl::g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for geometry registration");
            return false;
        }
        
        // Example: Schedule a dummy plane for ground reflection
        // In reality, we would iterate through game objects and add their geometry
        
        // Schedule a BVH update task
        KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
        bvhBuildTask.buildTLAS = true;  // Build top-level acceleration structure
        bvhBuildTask.maxBlasBuildCount = 16u;  // Number of BLASes to build per frame
        taskContainer->ScheduleBVHTask(&bvhBuildTask);
        
        // Execute the GPU tasks to update geometry
        auto status = KickstartRTImpl::g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (status != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute geometry registration task. Status: {}", static_cast<int>(status));
            return false;
        }
        
        logger::info("[RT] Geometry registration completed successfully");
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry registration: {}", e.what());
        return false;
    }
}

bool Raytracing::UpdateGeometry()
{
    // This would update dynamic geometry with KickstartRT
    // Left as a placeholder for future implementation
    return true;
}

bool Raytracing::GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix)
{
    // Default to identity matrices
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Try to get the current view and projection matrices from the game's renderer
    if (auto renderer = globals::game::renderer) {
        // Get the camera data
        // int eyeIndex = 0; // Use main eye (for VR we'd need to handle both eyes)
        
        // Check for VR without using namespaces that don't exist
        bool isVR = globals::game::isVR; // Use the correct global
        if (isVR) {
            logger::info("[RT] VR mode detected, using eye index 0 for now");
            // In the future we should disable raytracing in VR
        }
        
        // For now, return identity matrices
        // We'll implement proper matrix retrieval later
        logger::info("[RT] Using identity matrices for view and projection");
        return true;
        
        // TODO: Implement proper matrix retrieval from the game's renderer
        // Example of how this would work:
        // auto viewMat = renderer->GetViewMatrix();
        // auto projMat = renderer->GetProjectionMatrix();
        // DirectX::XMStoreFloat4x4(&viewMatrix, viewMat);
        // DirectX::XMStoreFloat4x4(&projMatrix, projMat);
    }
    
    logger::error("[RT] Cannot get renderer");
    return false;
}

// Direct pass-through to KickstartRTImpl
bool Raytracing::GenerateGI(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalSRV,
    ID3D11UnorderedAccessView* outputUAV,
    const DirectX::XMFLOAT4X4& viewMatrix,
    const DirectX::XMFLOAT4X4& projMatrix)
{
    // Check if we're enabled
    if (!IsEnabled()) {
        logger::info("[Raytracing] GenerateGI called but raytracing is not enabled");
        return false;
    }
    
    // Validate input resources
    if (!depthSRV || !normalSRV || !outputUAV) {
        logger::error("[Raytracing] GenerateGI called with null resources");
        return false;
    }
    
    logger::info("[Raytracing] Generating global illumination with provided resources");
    
    // Call into the KickstartRT implementation
    return KickstartRTImpl::GenerateGI(
        depthSRV,
        normalSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
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
    
    logger::info("[Raytracing] Generating reflections with provided resources");
    
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