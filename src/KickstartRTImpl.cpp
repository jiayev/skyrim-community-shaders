#include "../include/KickstartRTImpl.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <DirectXMath.h>

// Enable KickstartRT by default
#ifndef ENABLE_KICKSTART_RT
#define ENABLE_KICKSTART_RT 1
#endif

// Define the D3D11 API for KickstartRT before including headers
#define KickstartRT_Graphics_API_D3D11 1

// Disable warnings for unreferenced parameters and struct vs class mismatch
#pragma warning(disable: 4100)
#pragma warning(disable: 4099)

#ifdef ENABLE_KICKSTART_RT
// Include actual KickstartRT headers - order matters
#include "../extern/KickstartRT/include/KickstartRT_common.h"
#include "../extern/KickstartRT/include/KickstartRT.h"
#include "../extern/KickstartRT/include/KickstartRT_Interop_layer_d3d11.h"

// Function pointer types for dynamic loading
typedef KickstartRT::Status (STDCALL *PFN_CreateExecuteContext)(
    ID3D11Device*, 
    const KickstartRT::D3D11::ExecuteContext_InitSettings*, 
    KickstartRT::D3D11::ExecuteContext**
);

typedef void (STDCALL *PFN_DestroyExecuteContext)(
    KickstartRT::D3D11::ExecuteContext*
);

// Global function pointers
PFN_CreateExecuteContext g_pfnCreateExecuteContext = nullptr;
PFN_DestroyExecuteContext g_pfnDestroyExecuteContext = nullptr;

// Helper to load the DLL and functions
bool LoadKickstartRTFunctions() {
    // Try several possible locations for the DLL
    std::vector<std::string> possiblePaths = {
        "KickstartRT_Interop_D3D11.dll",                      // Current directory
        "Data\\SKSE\\Plugins\\KickstartRT_Interop_D3D11.dll", // Game's plugin directory
    };

    // Get executable path
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDirectory = exePath;
    size_t lastSlash = exeDirectory.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDirectory = exeDirectory.substr(0, lastSlash + 1);
        // Add executable directory as a possible path
        possiblePaths.push_back(exeDirectory + "KickstartRT_Interop_D3D11.dll");
        // Also try the SKSE plugins directory relative to exe
        possiblePaths.push_back(exeDirectory + "Data\\SKSE\\Plugins\\KickstartRT_Interop_D3D11.dll");
    }

    HMODULE hDLL = NULL;
    std::string loadedPath;

    // Try each path until we find one that works
    for (const auto& path : possiblePaths) {
        logger::info("[KickstartRTImpl] Trying to load DLL from: {}", path);
        hDLL = LoadLibraryA(path.c_str());
        if (hDLL) {
            loadedPath = path;
            break;
        }
    }

    if (!hDLL) {
        logger::error("[KickstartRTImpl] Failed to load KickstartRT_Interop_D3D11.dll from any location");
        return false;
    }
    
    logger::info("[KickstartRTImpl] Successfully loaded DLL from: {}", loadedPath);
    
    // Get function pointers
    g_pfnCreateExecuteContext = reinterpret_cast<PFN_CreateExecuteContext>(
        GetProcAddress(hDLL, "CreateExecuteContext"));
    
    g_pfnDestroyExecuteContext = reinterpret_cast<PFN_DestroyExecuteContext>(
        GetProcAddress(hDLL, "DestroyExecuteContext"));
    
    if (!g_pfnCreateExecuteContext || !g_pfnDestroyExecuteContext) {
        logger::error("[KickstartRTImpl] Failed to get function pointers from KickstartRT_Interop_D3D11.dll");
        FreeLibrary(hDLL);
        return false;
    }
    
    logger::info("[KickstartRTImpl] Successfully loaded KickstartRT_Interop_D3D11.dll functions");
    return true;
}
#endif

// Simple implementation that checks if the DLL exists and can be loaded
KickstartRTImpl::KickstartRTImpl() 
    : m_executeContext(nullptr)
    , m_initialized(false)
    , m_width(0)
    , m_height(0)
{
    logger::info("[KickstartRT] Implementation created");
}

KickstartRTImpl::~KickstartRTImpl() {
    Shutdown();
}

bool KickstartRTImpl::Initialize(ID3D11Device* device) {
#ifdef ENABLE_KICKSTART_RT
    if (!device) {
        logger::error("[KickstartRTImpl] Null device provided to Initialize");
        return false;
    }
    
    // Already initialized
    if (m_initialized) {
        logger::warn("[KickstartRTImpl] Already initialized");
        return true;
    }
    
    // Load the functions if not already loaded
    if (!g_pfnCreateExecuteContext && !LoadKickstartRTFunctions()) {
        logger::error("[KickstartRTImpl] Failed to load KickstartRT functions");
        return false;
    }
    
    try {
        // Get dimensions from device
        m_width = 1920;  // Default resolution
        m_height = 1080;
        
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
                        m_width = r.right - r.left;
                        m_height = r.bottom - r.top;
                        logger::info("[KickstartRTImpl] Display dimensions: {}x{}", m_width, m_height);
                    }
                    output->Release();
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        
        // Initialize KickstartRT using the D3D11 interop layer
        logger::info("[KickstartRTImpl] Initializing KickstartRT with D3D11 interop layer...");
        
        // Create init settings for D3D11
        KickstartRT::D3D11::ExecuteContext_InitSettings settings;
        settings.D3D11Device = device;
        settings.usingCommandQueue = KickstartRT::D3D11::ExecuteContext_InitSettings::UsingCommandQueue::Direct;
        settings.supportedWorkingSet = 4u;
        settings.descHeapSize = 8192u;
        settings.uploadHeapSizeForVolatileConstantBuffers = 64u * 1024u;
        
        // Create the execute context using the function pointer
        KickstartRT::D3D11::ExecuteContext* context = nullptr;
        KickstartRT::Status status = g_pfnCreateExecuteContext(device, &settings, &context);
        
        if (status != KickstartRT::Status::OK || !context) {
            logger::error("[KickstartRTImpl] Failed to create KickstartRT execute context. Status: {}", static_cast<int>(status));
            return false;
        }
        
        m_executeContext = context;
        m_initialized = true;
        
        logger::info("[KickstartRTImpl] KickstartRT initialized successfully");
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[KickstartRTImpl] Exception during initialization: {}", e.what());
        return false;
    }
#else
    logger::warn("[KickstartRTImpl] KickstartRT not enabled in build");
    return false;
#endif
}

void KickstartRTImpl::Shutdown() {
#ifdef ENABLE_KICKSTART_RT
    if (m_initialized && m_executeContext && g_pfnDestroyExecuteContext) {
        logger::info("[KickstartRTImpl] Shutting down KickstartRT");
        
        // Clean up resources before destroying context
        CleanupResources();
        
        // Clean up and release the ExecuteContext using the function pointer
        auto context = static_cast<KickstartRT::D3D11::ExecuteContext*>(m_executeContext);
        g_pfnDestroyExecuteContext(context);
        
        m_executeContext = nullptr;
        m_initialized = false;
        
        logger::info("[KickstartRTImpl] KickstartRT shutdown complete");
    }
#endif
}

bool KickstartRTImpl::RunTest() {
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Cannot run test, not initialized");
        return false;
    }
    
    logger::info("[KickstartRTImpl] Running KickstartRT test");
    
    try {
        // Get the execute context
        auto context = static_cast<KickstartRT::D3D11::ExecuteContext*>(m_executeContext);
        
        // Try to do a simple operation like releasing resources
        KickstartRT::Status status = context->ReleaseDeviceResourcesImmediately();
        
        if (status == KickstartRT::Status::OK) {
            logger::info("[KickstartRTImpl] KickstartRT test successful");
            return true;
        } else {
            logger::error("[KickstartRTImpl] KickstartRT test failed. Status: {}", static_cast<int>(status));
            return false;
        }
    }
    catch (const std::exception& e) {
        logger::error("[KickstartRTImpl] Exception during test: {}", e.what());
        return false;
    }
#else
    logger::warn("[KickstartRTImpl] KickstartRT not enabled in build, test skipped");
    return false;
#endif
}

void KickstartRTImpl::CleanupResources() {
#ifdef ENABLE_KICKSTART_RT
    if (m_initialized && m_executeContext) {
        logger::info("[KickstartRTImpl] Cleaning up resources");
        
        auto context = static_cast<KickstartRT::D3D11::ExecuteContext*>(m_executeContext);
        context->ReleaseDeviceResourcesImmediately();
        
        logger::info("[KickstartRTImpl] Resource cleanup complete");
    }
#endif
}

// Simplified placeholder implementations for the remaining methods
// These will need to be replaced with actual implementations that match your KickstartRT version

bool KickstartRTImpl::RegisterGeometry(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, 
                                     uint32_t vertexCount, uint32_t indexCount, 
                                     uint32_t vertexStride, void** outGeoHandle) 
{
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Not initialized");
        return false;
    }
    
    // We're not actually using the context variable yet in this placeholder implementation
    // auto context = static_cast<KickstartRT::D3D11::ExecuteContext*>(m_executeContext);
    
    try {
        // Using uint64_t as a placeholder instead to avoid GeometryHandle issues
        uint64_t dummyHandle = reinterpret_cast<uint64_t>(vertexBuffer); // Just a dummy value
        
        // Store a simple value for the handle as a placeholder
        *outGeoHandle = new uint64_t(dummyHandle);
        
        logger::info("[KickstartRTImpl] Placeholder: RegisterGeometry created dummy handle");
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[KickstartRTImpl] Exception in RegisterGeometry: {}", e.what());
        return false;
    }
#else
    return false;
#endif
}

bool KickstartRTImpl::RegisterInstance(void* geoHandle, const DirectX::XMFLOAT4X4& transform, void** outInstHandle) 
{
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Not initialized");
        return false;
    }
    
    if (!geoHandle) {
        logger::error("[KickstartRTImpl] Null geometry handle");
        return false;
    }
    
    // We're not actually using the context variable yet in this placeholder implementation
    // auto context = static_cast<KickstartRT::D3D11::ExecuteContext*>(m_executeContext);
    
    try {
        // Using uint64_t as a placeholder to avoid InstanceHandle issues
        uint64_t dummyHandle = reinterpret_cast<uint64_t>(geoHandle) + 1; // Just a dummy value
        
        // Store a simple value for the handle
        *outInstHandle = new uint64_t(dummyHandle);
        
        logger::info("[KickstartRTImpl] Placeholder: RegisterInstance created dummy handle");
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[KickstartRTImpl] Exception in RegisterInstance: {}", e.what());
        return false;
    }
#else
    return false;
#endif
}

bool KickstartRTImpl::GenerateGI(ID3D11ShaderResourceView* depthSRV, 
                               ID3D11ShaderResourceView* normalSRV,
                               ID3D11UnorderedAccessView* outputUAV,
                               const DirectX::XMFLOAT4X4& viewMatrix,
                               const DirectX::XMFLOAT4X4& projMatrix)
{
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Not initialized");
        return false;
    }
    
    logger::info("[KickstartRTImpl] GenerateGI called (placeholder)");
    // In a real implementation, we would create a task container, schedule a TraceDiffuseTask,
    // and invoke the GPU task to generate global illumination
    return true;
#else
    return false;
#endif
}

bool KickstartRTImpl::GenerateReflections(ID3D11ShaderResourceView* depthSRV, 
                                        ID3D11ShaderResourceView* normalSRV, 
                                        ID3D11ShaderResourceView* roughnessSRV,
                                        ID3D11UnorderedAccessView* outputUAV,
                                        const DirectX::XMFLOAT4X4& viewMatrix,
                                        const DirectX::XMFLOAT4X4& projMatrix) 
{
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Not initialized");
        return false;
    }
    
    logger::info("[KickstartRTImpl] GenerateReflections called (placeholder)");
    // In a real implementation, we would create a task container, schedule a TraceSpecularTask,
    // and invoke the GPU task to generate reflections
    return true;
#else
    return false;
#endif
}

bool KickstartRTImpl::InjectLighting(ID3D11ShaderResourceView* lightingSRV,
                                   ID3D11ShaderResourceView* depthSRV,
                                   ID3D11ShaderResourceView* normalSRV,
                                   const DirectX::XMFLOAT4X4& viewMatrix,
                                   const DirectX::XMFLOAT4X4& projMatrix) 
{
#ifdef ENABLE_KICKSTART_RT
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRTImpl] Not initialized");
        return false;
    }
    
    logger::info("[KickstartRTImpl] InjectLighting called (placeholder)");
    // In a real implementation, we would create a task container, schedule a DirectLightingInjectionTask,
    // and invoke the GPU task to inject lighting into the direct lighting cache
    return true;
#else
    return false;
#endif
} 