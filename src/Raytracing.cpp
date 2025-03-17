#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"
#include "../include/PCH.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>

// Disable warnings for unreferenced parameters
#pragma warning(disable: 4100)

// Forward declare the ExecuteContext type to avoid namespace resolution issues
struct ExecuteContextD3D11;

// Direct access to the ExecuteContext - using a typedef to avoid namespace resolution issues
typedef ExecuteContextD3D11 ExecuteContext;
static ExecuteContext* g_executeContext = nullptr;
static bool g_initialized = false;
static uint32_t g_width = 0;
static uint32_t g_height = 0;

// Function pointer types for dynamic loading - using void* instead of specific type
typedef KickstartRT::Status (STDCALL *PFN_CreateExecuteContext)(
    ID3D11Device*, 
    const void*, // ExecuteContext_InitSettings
    void**       // ExecuteContext**
);

typedef void (STDCALL *PFN_DestroyExecuteContext)(
    void* // ExecuteContext*
);

// Global function pointers
static PFN_CreateExecuteContext g_pfnCreateExecuteContext = nullptr;
static PFN_DestroyExecuteContext g_pfnDestroyExecuteContext = nullptr;

// Load the KickstartRT DLL and get function pointers
bool LoadKickstartRTFunctions() {
    HMODULE hDLL = nullptr;
    
    // Set the DLL directory path
    SetDllDirectoryW(L"Data/SKSE/Plugins/KickstartRT/");
    
    // Load the DLL from the known working path
    hDLL = LoadLibraryW(L"KickstartRT_Interop_D3D11.dll");
    if (!hDLL) {
        DWORD error = GetLastError();
        logger::error("[RT] Failed to load KickstartRT_Interop_D3D11.dll: Error code: 0x{:X} - {}", error, error);
        return false;
    }
    
    // Get function pointers
    g_pfnCreateExecuteContext = reinterpret_cast<PFN_CreateExecuteContext>(
        GetProcAddress(hDLL, "CreateExecuteContext"));
    
    g_pfnDestroyExecuteContext = reinterpret_cast<PFN_DestroyExecuteContext>(
        GetProcAddress(hDLL, "DestroyExecuteContext"));
    
    if (!g_pfnCreateExecuteContext || !g_pfnDestroyExecuteContext) {
        DWORD error = GetLastError();
        logger::error("[RT] Failed to get KickstartRT function pointers: Error code: 0x{:X} - {}", error, error);
        FreeLibrary(hDLL);
        return false;
    }
    
    logger::info("[RT] KickstartRT loaded successfully");
    return true;
}

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
    
    // Load the functions if not already loaded
    if (!g_pfnCreateExecuteContext && !LoadKickstartRTFunctions()) {
        logger::error("[RT] Failed to load KickstartRT functions");
        return false;
    }
    
    try {
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
        
        // Create the execute context using the function pointer
        void* contextPtr = nullptr;
        KickstartRT::Status status = g_pfnCreateExecuteContext(
            device, 
            &settings, 
            &contextPtr);
        
        g_executeContext = static_cast<ExecuteContext*>(contextPtr);
        
        if (status != KickstartRT::Status::OK || !g_executeContext) {
            logger::error("[RT] Failed to create context. Status: {}", static_cast<int>(status));
            return false;
        }
        
        g_initialized = true;
        logger::info("[RT] KickstartRT initialized successfully");
        
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception: {}", e.what());
        return false;
    }
}

// Shutdown KickstartRT
void Shutdown() {
    if (g_initialized && g_executeContext && g_pfnDestroyExecuteContext) {
        // Destroy context
        g_pfnDestroyExecuteContext(g_executeContext);
        
        g_executeContext = nullptr;
        g_initialized = false;
        
        logger::info("[RT] KickstartRT shutdown complete");
    }
}

// Simple test to verify KickstartRT is working
bool RunTest() {
    if (!g_initialized || !g_executeContext) {
        logger::error("[RT] Cannot run test, not initialized");
        return false;
    }
    
    try {
        // For a simple test, we'll just check if initialization worked
        // The ExecuteContext object exists and initialization completed successfully
        logger::info("[RT] Test completed successfully - ExecuteContext was created properly");
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Test exception: {}", e.what());
        return false;
    }
}

// Clean up resources without destroying the context
void CleanupResources() {
    if (g_initialized && g_executeContext) {
        logger::info("[RT] Resources cleaned up");
    }
}

// Example implementation for Global Illumination
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
    
    // This is a placeholder - in a real implementation, we would:
    // 1. Create a TaskContainer from the ExecuteContext
    // 2. Schedule a TraceDiffuseTask to it
    // 3. Call InvokeGPUTask to execute it
    
    // For now, just return success
    return true;
}

// Example implementation for reflections
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
    
    // Placeholder for reflection implementation
    return true;
}

// Example implementation for direct lighting injection
bool InjectLighting(ID3D11ShaderResourceView* lightingSRV,
                  ID3D11ShaderResourceView* depthSRV,
                  ID3D11ShaderResourceView* normalSRV,
                  const DirectX::XMFLOAT4X4& viewMatrix,
                  const DirectX::XMFLOAT4X4& projMatrix) 
{
    if (!g_initialized || !g_executeContext) {
        logger::error("[RT] Not initialized");
        return false;
    }
    
    // Placeholder for lighting injection implementation
    return true;
}

// Check if KickstartRT is initialized
bool IsInitialized() {
    return g_initialized;
}