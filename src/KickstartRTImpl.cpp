#include "KickstartRTImpl.h"
#include "Globals.h"
#include <Windows.h>

namespace KickstartRTImpl
{
    // Global variables
    Microsoft::WRL::ComPtr<ID3D11Fence> g_renderFence = nullptr;
    uint64_t g_fenceValue = 1;
    KickstartRT::D3D11::ExecuteContext* g_executeContext = nullptr;
    std::unordered_map<std::string, KickstartRT::D3D11::GeometryHandle> g_geometryHandles;
    std::unordered_map<std::string, KickstartRT::D3D11::InstanceHandle> g_instanceHandles;
    bool g_initialized = false;
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
            
            // Create fence for synchronization
            ID3D11Device5* device5 = nullptr;
            HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device5), (void**)&device5);
            if (SUCCEEDED(hr) && device5) {
                hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_renderFence));
                if (FAILED(hr)) {
                    logger::warn("[RT] Failed to create fence for GPU synchronization. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                    // Continue without fence, not critical
                } else {
                    logger::info("[RT] Successfully created fence for GPU synchronization");
                    g_fenceValue = 1; // Start at 1
                }
                device5->Release();
            } else {
                logger::warn("[RT] D3D11Device5 interface not available. GPU synchronization will use legacy method.");
            }
            
            // Create init settings for D3D11
            KickstartRT::D3D11::ExecuteContext_InitSettings settings;
            settings.D3D11Device = device;
            
            // Get the adapter directly for KickstartRT
            // This is REQUIRED for the D3D11 interop layer
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
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
                
                // Try with default settings as a fallback
                logger::warn("[RT] Trying to initialize with default settings");
                KickstartRT::D3D11::ExecuteContext_InitSettings defaultSettings;
                defaultSettings.D3D11Device = device;
                defaultSettings.DXGIAdapter = adapter1;
                
                status = KickstartRT::D3D11::ExecuteContext::Init(
                    &defaultSettings, 
                    &exc,
                    KickstartRT::Version());
                    
                if (status != KickstartRT::Status::OK) {
                    logger::error("[RT] Failed to create context with default settings. Status: {}", static_cast<int>(status));
                    return false;
                }
            }
            
            // Then check if context is null
            if (!exc) {
                logger::error("[RT] Context creation returned OK but context is null");
                return false;
            }
            
            g_executeContext = exc;
            g_initialized = true;
            
            // Make sure we have a fence for synchronization
            if (!g_renderFence) {
                logger::warn("[RT] No fence was created earlier. Attempting to create one now.");
                device5 = nullptr;
                hr = device->QueryInterface(__uuidof(ID3D11Device5), (void**)&device5);
                if (SUCCEEDED(hr) && device5) {
                    hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_renderFence));
                    if (FAILED(hr)) {
                        logger::error("[RT] Failed to create fence as fallback. KickstartRT operations will fail. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                    } else {
                        logger::info("[RT] Successfully created fallback fence");
                        g_fenceValue = 1; // Start at 1
                    }
                    device5->Release();
                } else {
                    logger::error("[RT] Could not get D3D11Device5 interface. GPU synchronization will not work.");
                }
            }
            
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
            // Wait for any pending GPU operations to complete
            WaitForGPU();
            
            // Release fence objects
            g_renderFence = nullptr;
            g_fenceValue = 0;
            
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
            // Wait for any pending GPU operations to complete
            WaitForGPU();
            
            g_executeContext->ReleaseDeviceResourcesImmediately();
            logger::info("[RT] Resources cleaned up");
        }
    }

    // Wait for GPU tasks to complete
    bool WaitForGPU() {
        if (!g_renderFence || !g_initialized) {
            return false;
        }
        
        try {
            auto context = globals::d3d::context;
            if (!context) {
                return false;
            }
            
            // Get the ID3D11DeviceContext4 interface needed for fence operations
            Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
            HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), &context4);
            if (FAILED(hr)) {
                logger::error("[RT] Failed to get ID3D11DeviceContext4 interface. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Wait for the latest fence value
            context4->Wait(g_renderFence.Get(), g_fenceValue - 1);
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception in WaitForGPU: {}", e.what());
            return false;
        }
    }

    // The rest of the implementation will be in separate files
} 