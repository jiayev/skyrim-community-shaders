#pragma once

#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>

// Forward declarations for KickstartRT
namespace KickstartRT
{
    namespace D3D11
    {
        class ExecuteContext;
    }

    enum struct Status : uint32_t
    {
        OK = 0,
        ERROR_FAILED = 1,
        // Add other error codes if needed
    };
}

/**
 * @brief Simplified implementation of KickstartRT raytracing for the Community Shaders project
 */
class KickstartRTImpl {
public:
    KickstartRTImpl();
    ~KickstartRTImpl();

    // Initialize KickstartRT with a D3D11 device
    bool Initialize(ID3D11Device* device);
    
    // Shutdown and cleanup
    void Shutdown();
    
    // Simple test to check if KickstartRT is working
    bool RunTest();
    
    // Clean up resources (call when changing scenes)
    void CleanupResources();

    // Check if initialized
    bool IsInitialized() const { return m_initialized; }

private:
    // KickstartRT implementation details are hidden
    void* m_executeContext = nullptr;
    
    // Flag to check if initialized
    bool m_initialized = false;
    
    // Current viewport dimensions
    uint32_t m_width = 0;
    uint32_t m_height = 0;
}; 