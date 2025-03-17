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
    bool GenerateGI(ID3D11ShaderResourceView* depthSRV, 
                   ID3D11ShaderResourceView* normalSRV,
                   ID3D11UnorderedAccessView* outputUAV,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   const DirectX::XMFLOAT4X4& projMatrix) 
    {
        if (!g_initialized || !g_executeContext) {
            logger::error("[RT] Not initialized");
            return false;
        }
        
        if (!depthSRV || !normalSRV || !outputUAV) {
            logger::error("[RT] Missing required buffers for GI generation");
            return false;
        }
        
        try {
            logger::info("[RT] Generating GI");
            
            // Create a task container for GI
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container for GI");
                return false;
            }
            
            // 1. Schedule BVH Build task - this ensures the acceleration structure is up to date
            {
                KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
                bvhBuildTask.buildTLAS = true;  // Build top-level acceleration structure
                bvhBuildTask.maxBlasBuildCount = 4u;  // Number of BLASes to build per frame
                taskContainer->ScheduleBVHTask(&bvhBuildTask);
            }
            
            // 2. Schedule Diffuse GI tracing task
            {
                KickstartRT::D3D11::RenderTask::TraceDiffuseTask traceTask;
                
                // Fill common trace task parameters
                if (depthSRV) {
                    ID3D11Resource* depthResource = nullptr;
                    depthSRV->GetResource(&depthResource);
                    traceTask.common.depth.tex.resource = depthResource;
                }
                
                if (normalSRV) {
                    ID3D11Resource* normalResource = nullptr;
                    normalSRV->GetResource(&normalResource);
                    traceTask.common.normal.tex.resource = normalResource;
                }
                
                // Set view and projection matrices
                // Matrix conversion from DirectXMath to KickstartRT format
                // Note: KickstartRT expects row-major matrices
                traceTask.common.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&viewMatrix);
                traceTask.common.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&projMatrix);
                
                // Set ray parameters
                traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
                
                // Set output
                if (outputUAV) {
                    outputUAV->GetResource(&traceTask.out.resource);
                }
                
                // Schedule the task
                taskContainer->ScheduleRenderTask(&traceTask);
            }
            
            // 3. Execute the GPU tasks
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute GI task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            logger::info("[RT] Successfully executed GI generation");
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] GI generation exception: {}", e.what());
            return false;
        }
    }

    bool GenerateReflections(ID3D11ShaderResourceView* depthSRV, 
                           ID3D11ShaderResourceView* normalSRV, 
                           ID3D11ShaderResourceView* roughnessSRV,
                           ID3D11UnorderedAccessView* outputUAV,
                           const DirectX::XMFLOAT4X4& viewMatrix,
                           const DirectX::XMFLOAT4X4& projMatrix) 
    {
        if (!g_initialized || !g_executeContext) {
            logger::error("[RT] Not initialized");
            return false;
        }
        
        if (!depthSRV || !normalSRV || !roughnessSRV || !outputUAV) {
            logger::error("[RT] Missing required buffers for reflection generation");
            return false;
        }
        
        try {
            logger::info("[RT] Generating reflections");
            
            // Create a task container for reflections
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container for reflections");
                return false;
            }
            
            // BVH Build task
            {
                KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
                bvhBuildTask.buildTLAS = true;
                bvhBuildTask.maxBlasBuildCount = 4u;
                taskContainer->ScheduleBVHTask(&bvhBuildTask);
            }
            
            // Schedule specular reflection tracing task
            {
                KickstartRT::D3D11::RenderTask::TraceSpecularTask traceTask;
                
                // Fill common trace task parameters
                if (depthSRV) {
                    ID3D11Resource* depthResource = nullptr;
                    depthSRV->GetResource(&depthResource);
                    traceTask.common.depth.tex.resource = depthResource;
                }
                
                if (normalSRV) {
                    ID3D11Resource* normalResource = nullptr;
                    normalSRV->GetResource(&normalResource);
                    traceTask.common.normal.tex.resource = normalResource;
                }
                
                if (roughnessSRV) {
                    ID3D11Resource* roughnessResource = nullptr;
                    roughnessSRV->GetResource(&roughnessResource);
                    traceTask.common.roughness.tex.resource = roughnessResource;
                }
                
                // Set view and projection matrices
                traceTask.common.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&viewMatrix);
                traceTask.common.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&projMatrix);
                
                // Set ray parameters
                traceTask.common.maxRayLength = 200.0f;
                
                // Set output
                if (outputUAV) {
                    outputUAV->GetResource(&traceTask.out.resource);
                }
                
                // Schedule the task
                taskContainer->ScheduleRenderTask(&traceTask);
            }
            
            // Execute the GPU tasks
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute reflections task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            logger::info("[RT] Successfully executed reflections generation");
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Reflection generation exception: {}", e.what());
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
    return KickstartRTImpl::RunTest();
}

bool Raytracing::RegisterGeometry()
{
    // This would register scene geometry with KickstartRT
    // Left as a placeholder for future implementation
    return true;
}

bool Raytracing::UpdateGeometry()
{
    // This would update dynamic geometry with KickstartRT
    // Left as a placeholder for future implementation
    return true;
}

bool Raytracing::ApplyGlobalIllumination(float intensity, float distance, float saturation)
{
    if (!IsInitialized() || !settings.Enabled) {
        return false;
    }
    
    // Get matrices from the current rendering context
    DirectX::XMFLOAT4X4 viewMatrix, projMatrix;
    if (!GetCurrentViewAndProjectionMatrices(viewMatrix, projMatrix)) {
        logger::error("[RT] Failed to get current view and projection matrices");
        return false;
    }
    
    // Get required buffers
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11ShaderResourceView* normalSRV = nullptr;
    ID3D11UnorderedAccessView* outputUAV = nullptr;
    
    if (!GetRequiredBuffersForGI(depthSRV, normalSRV, outputUAV)) {
        logger::error("[RT] Failed to get required buffers for GI");
        return false;
    }
    
    // Call the low-level implementation
    logger::info("[RT] Applying global illumination with intensity={}, distance={}, saturation={}", 
                intensity, distance, saturation);
                
    return KickstartRTImpl::GenerateGI(
        depthSRV,
        normalSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
}

bool Raytracing::ApplyReflections(float intensity, float roughness, float distance)
{
    if (!IsInitialized() || !settings.Enabled) {
        return false;
    }
    
    // Get matrices from the current rendering context
    DirectX::XMFLOAT4X4 viewMatrix, projMatrix;
    if (!GetCurrentViewAndProjectionMatrices(viewMatrix, projMatrix)) {
        logger::error("[RT] Failed to get current view and projection matrices");
        return false;
    }
    
    // Get required buffers
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11ShaderResourceView* normalSRV = nullptr;
    ID3D11ShaderResourceView* roughnessSRV = nullptr;
    ID3D11UnorderedAccessView* outputUAV = nullptr;
    
    if (!GetRequiredBuffersForReflections(depthSRV, normalSRV, roughnessSRV, outputUAV)) {
        logger::error("[RT] Failed to get required buffers for reflections");
        return false;
    }
    
    // Call the low-level implementation
    logger::info("[RT] Applying reflections with intensity={}, roughness={}, distance={}", 
                intensity, roughness, distance);
                
    return KickstartRTImpl::GenerateReflections(
        depthSRV,
        normalSRV,
        roughnessSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
}

bool Raytracing::GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix)
{
    // Default to identity matrices
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Try to get the current view and projection matrices from the game's renderer
    if (auto renderer = globals::game::renderer) {
        // For now, log that we're using default matrices until we implement proper fetching
        logger::warn("[RT] Using default view and projection matrices (not implemented yet)");
        return true;
    }
    
    logger::error("[RT] Cannot get renderer");
    return false;
}

bool Raytracing::GetRequiredBuffersForGI(ID3D11ShaderResourceView*& depthSRV, ID3D11ShaderResourceView*& normalSRV, ID3D11UnorderedAccessView*& outputUAV)
{
    // Set default values
    depthSRV = nullptr;
    normalSRV = nullptr;
    outputUAV = nullptr;
    
    // Check if renderer is available
    if (auto renderer = globals::game::renderer) {
        // For now, log that proper buffer fetching isn't implemented yet
        logger::warn("[RT] Buffer fetching for GI not implemented yet");
        
        // Return true for now to allow development without actual buffer references
        return true;
    }
    
    logger::error("[RT] Cannot get renderer for GI buffers");
    return false;
}

bool Raytracing::GetRequiredBuffersForReflections(ID3D11ShaderResourceView*& depthSRV, ID3D11ShaderResourceView*& normalSRV, ID3D11ShaderResourceView*& roughnessSRV, ID3D11UnorderedAccessView*& outputUAV)
{
    // Set default values
    depthSRV = nullptr;
    normalSRV = nullptr;
    roughnessSRV = nullptr;
    outputUAV = nullptr;
    
    // Check if renderer is available
    if (auto renderer = globals::game::renderer) {
        // For now, log that proper buffer fetching isn't implemented yet
        logger::warn("[RT] Buffer fetching for reflections not implemented yet");
        
        // Return true for now to allow development without actual buffer references
        return true;
    }
    
    logger::error("[RT] Cannot get renderer for reflection buffers");
    return false;
}