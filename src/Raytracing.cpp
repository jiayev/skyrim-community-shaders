#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"
#include "Raytracing.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Logger.h"
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
            // Set DLL directory to ensure KickstartRT DLL can be found
            #ifdef KICKSTART_RT_DLL_PATH
            // Store the original DLL directory
            WCHAR originalPath[MAX_PATH];
            GetDllDirectory(MAX_PATH, originalPath);
            
            // Set the DLL directory to our KickstartRT directory
            logger::info("[RT] Setting DLL directory to {}", KICKSTART_RT_DLL_PATH);
            if (!SetDllDirectory(TEXT(KICKSTART_RT_DLL_PATH))) {
                logger::error("[RT] Failed to set DLL directory. Error: {}", GetLastError());
                // Continue anyway, the DLL might be in the system path
            }
            #endif
            
            // Get dimensions from device
            g_width = 1920;  // Default resolution
            g_height = 1080;
            
            // Get adapter associated with device to query output information
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    // Get primary output (monitor)
                    IDXGIOutput* output = nullptr;
                    if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
                        DXGI_OUTPUT_DESC desc;
                        if (SUCCEEDED(output->GetDesc(&desc))) {
                            RECT r = desc.DesktopCoordinates;
                            g_width = r.right - r.left;
                            g_height = r.bottom - r.top;
                        }
                        output->Release();
                    }
                    adapter->Release();
                }
                dxgiDevice->Release();
            }
            
            // Create init settings for D3D11
            KickstartRT::D3D11::ExecuteContext_InitSettings settings;
            settings.D3D11Device = device;
            settings.usingCommandQueue = KickstartRT::D3D11::ExecuteContext_InitSettings::UsingCommandQueue::Direct;
            settings.supportedWorkingSet = 4u;
            settings.descHeapSize = 8192u;
            settings.uploadHeapSizeForVolatileConstantBuffers = 64u * 1024u;
            
            // Create the execute context - directly use the KickstartRT API
            KickstartRT::D3D11::ExecuteContext* exc = nullptr;
            
            logger::info("[RT] Initializing KickstartRT");
            KickstartRT::Status status = KickstartRT::D3D11::ExecuteContext::Init(
                &settings, 
                &exc,
                KickstartRT::Version());
            
            // Restore the original DLL directory
            #ifdef KICKSTART_RT_DLL_PATH
            SetDllDirectory(originalPath);
            #endif
                
            if (status != KickstartRT::Status::OK || !exc) {
                logger::error("[RT] Failed to create context. Status: {}", static_cast<int>(status));
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
            
            // Schedule an empty task to execute
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

    // Core rendering functions
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
        
        if (depthSRV && normalSRV && outputUAV) {
            logger::info("[RT] Generating GI");
            // Create a task container for GI
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container for GI");
                return false;
            }
            
            // In a real implementation, we would:
            // 1. Schedule a TraceDiffuseTask to the container
            // 2. Call InvokeGPUTask to execute it
            
            // Execute and let KickstartRT manage the task container
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute GI task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            return true;
        }
        
        return false;
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
        
        if (depthSRV && normalSRV && roughnessSRV && outputUAV) {
            logger::info("[RT] Generating reflections");
            // Create a task container for reflections
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container for reflections");
                return false;
            }
            
            // In a real implementation, we would:
            // 1. Schedule a TraceSpecularTask to the container
            // 2. Call InvokeGPUTask to execute it
            
            // Execute and let KickstartRT manage the task container
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute reflections task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            return true;
        }
        
        return false;
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