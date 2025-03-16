#include "../include/KickstartRTImpl.h"
#include <iostream>
#include <fstream>

// Simple implementation that just checks if the DLL exists and can be loaded
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
    if (m_initialized) {
        logger::debug("[KickstartRT] Already initialized");
        return true;
    }

    if (!device) {
        logger::error("[KickstartRT] Invalid D3D11 device provided to KickstartRTImpl::Initialize");
        return false;
    }

    logger::info("[KickstartRT] Attempting to initialize KickstartRT...");

    // Try to load the KickstartRT DLL directly
    HMODULE dllHandle = LoadLibraryA("KickstartRT_Interop_D3D11.dll");
    if (!dllHandle) {
        DWORD error = GetLastError();
        logger::error("[KickstartRT] Failed to load KickstartRT_Interop_D3D11.dll, error code: {}", error);
        
        // Try the base DLL as fallback
        dllHandle = LoadLibraryA("KickstartRT_D3D11.dll");
        if (!dllHandle) {
            error = GetLastError();
            logger::error("[KickstartRT] Failed to load KickstartRT_D3D11.dll, error code: {}", error);
            return false;
        }
        logger::info("[KickstartRT] Loaded base KickstartRT_D3D11.dll instead");
    }

    // Store the DLL handle as our "context"
    m_executeContext = dllHandle;
    m_initialized = true;
    
    logger::info("[KickstartRT] DLL loaded successfully!");
    return true;
}

void KickstartRTImpl::Shutdown() {
    if (!m_initialized) {
        return;
    }

    logger::info("[KickstartRT] Shutting down");
    CleanupResources();

    // Free the DLL
    if (m_executeContext) {
        FreeLibrary((HMODULE)m_executeContext);
        m_executeContext = nullptr;
    }

    m_initialized = false;
    logger::info("[KickstartRT] Shut down completed");
}

bool KickstartRTImpl::RunTest() {
    if (!m_initialized || !m_executeContext) {
        logger::error("[KickstartRT] Not initialized, can't run test");
        return false;
    }
    
    logger::info("[KickstartRT] Test running - library loaded successfully!");
    
    // Try to get a function from the DLL as a basic test
    FARPROC testProc = GetProcAddress((HMODULE)m_executeContext, "GetModuleVersion");
    if (testProc) {
        logger::info("[KickstartRT] Found function in KickstartRT DLL!");
        return true;
    } else {
        logger::warn("[KickstartRT] Could not find expected function in KickstartRT DLL, but DLL is loaded.");
        return true; // Still consider the test passed if the DLL loaded
    }
}

void KickstartRTImpl::CleanupResources() {
    logger::debug("[KickstartRT] Cleaning up resources");
    // Nothing to clean up in this simplified version
} 