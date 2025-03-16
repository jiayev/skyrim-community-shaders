#include "KickstartRTWrapper.h"
#include "State.h"

bool KickstartRTWrapper::LoadFunctions(HMODULE dllHandle) {
    if (!dllHandle) {
        logger::error("[KickstartRTWrapper] Invalid DLL handle provided");
        return false;
    }

    logger::info("[KickstartRTWrapper] Loading KickstartRT API functions");

    // Load function pointers from the DLL using ordinals
    // These ordinal values are placeholders and may need to be adjusted based on actual DLL exports
    
    // ExecuteContext functions
    CreateExecuteContext = reinterpret_cast<CreateExecuteContext_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x01)));
    DestroyExecuteContext = reinterpret_cast<DestroyExecuteContext_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x02)));
    
    // Task container functions
    CreateTaskContainer = reinterpret_cast<CreateTaskContainer_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x03)));
    DestroyTaskContainer = reinterpret_cast<DestroyTaskContainer_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x04)));
    
    // Geometry handle functions
    CreateGeometryHandle = reinterpret_cast<CreateGeometryHandle_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x05)));
    CreateGeometryHandles = reinterpret_cast<CreateGeometryHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x06)));
    DestroyGeometryHandle = reinterpret_cast<DestroyGeometryHandle_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x07)));
    DestroyGeometryHandles = reinterpret_cast<DestroyGeometryHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x08)));
    DestroyAllGeometryHandles = reinterpret_cast<DestroyAllGeometryHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x09)));
    
    // Instance handle functions
    CreateInstanceHandle = reinterpret_cast<CreateInstanceHandle_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0A)));
    CreateInstanceHandles = reinterpret_cast<CreateInstanceHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0B)));
    DestroyInstanceHandle = reinterpret_cast<DestroyInstanceHandle_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0C)));
    DestroyInstanceHandles = reinterpret_cast<DestroyInstanceHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0D)));
    DestroyAllInstanceHandles = reinterpret_cast<DestroyAllInstanceHandles_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0E)));
    
    // Use a safer approach for task scheduling functions
    // Define a fallback function that will log errors but won't crash
    auto fallbackFunc = [](void* tc, void* task) -> int {
        // Mark parameters as used to avoid compiler warnings
        (void)tc;
        (void)task;
        logger::error("[KickstartRT] Fallback scheduler called - no valid implementation available");
        return -1;
    };
    
    // Assign fallback implementations initially
    ScheduleBVHTask = fallbackFunc;
    ScheduleRenderTask = fallbackFunc;
    
    // Load auxiliary functions that will be necessary
    logger::info("[KickstartRT] Loading auxiliary functions from DLL");
    
    // Try to find the scheduling functions directly - these ordinals are just placeholders
    // from our original attempt but now we'll use them more carefully
    typedef int (__stdcall *RawScheduleFunc)(void*, void*);
    RawScheduleFunc rawScheduleRenderTask = reinterpret_cast<RawScheduleFunc>(
        GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x10)));
    
    RawScheduleFunc rawScheduleBVHTask = reinterpret_cast<RawScheduleFunc>(
        GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x0F)));
    
    // If functions were found, wrap them with enhanced error handling using only Windows SEH
    if (rawScheduleRenderTask) {
        logger::info("[KickstartRT] Found ScheduleRenderTask at ordinal 0x10");
        ScheduleRenderTask = [rawFunc = rawScheduleRenderTask](void* tc, void* task) -> int {
            if (!tc || !task) {
                logger::error("[KickstartRT] Invalid parameters to ScheduleRenderTask");
                return -1;
            }
            
            // Log task pointer for debugging
            logger::debug("[KickstartRT] Calling ScheduleRenderTask with task at {:p}", task);
            
            // Use structured exception handling for DLL calls
            int result = -1;
            __try {
                result = rawFunc(tc, task);
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                DWORD exceptionCode = GetExceptionCode();
                logger::error("[KickstartRT] Exception in ScheduleRenderTask: 0x{:X}", exceptionCode);
                
                // Check for common access violation errors
                if (exceptionCode == EXCEPTION_ACCESS_VIOLATION) {
                    logger::error("[KickstartRT] Access violation - likely a Vector memory layout issue");
                }
                return -1;
            }
            
            return result;
        };
    }
    else {
        logger::warn("[KickstartRT] Could not find ScheduleRenderTask function (ordinal 0x10)");
    }
    
    if (rawScheduleBVHTask) {
        logger::info("[KickstartRT] Found ScheduleBVHTask at ordinal 0x0F");
        ScheduleBVHTask = [rawFunc = rawScheduleBVHTask](void* tc, void* task) -> int {
            if (!tc || !task) {
                logger::error("[KickstartRT] Invalid parameters to ScheduleBVHTask");
                return -1;
            }
            
            // Log task pointer for debugging
            logger::debug("[KickstartRT] Calling ScheduleBVHTask with task at {:p}", task);
            
            // Use structured exception handling for DLL calls
            int result = -1;
            __try {
                result = rawFunc(tc, task);
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                DWORD exceptionCode = GetExceptionCode();
                logger::error("[KickstartRT] Exception in ScheduleBVHTask: 0x{:X}", exceptionCode);
                
                // Check for common access violation errors
                if (exceptionCode == EXCEPTION_ACCESS_VIOLATION) {
                    logger::error("[KickstartRT] Access violation - likely a Vector memory layout issue in GeometryInput");
                }
                return -1;
            }
            
            return result;
        };
    }
    else {
        logger::warn("[KickstartRT] Could not find ScheduleBVHTask function (ordinal 0x0F)");
    }
    
    // GPU task execution functions
    BuildGPUTask = reinterpret_cast<BuildGPUTask_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x11)));
    MarkGPUTaskAsCompleted = reinterpret_cast<MarkGPUTaskAsCompleted_t>(GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x12)));

    // Load denoising context functions
    CreateDenoisingContextFromD3D11Device = reinterpret_cast<CreateDenoisingContextFromD3D11Device_t>(
        GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x20)));
        
    DestroyDenoisingContext = reinterpret_cast<DestroyDenoisingContext_t>(
        GetProcAddress(dllHandle, MAKEINTRESOURCEA(0x21)));
        
    if (CreateDenoisingContextFromD3D11Device && DestroyDenoisingContext) {
        logger::info("[KickstartRT] Successfully loaded denoising context functions");
    } else {
        logger::warn("[KickstartRT] Could not find denoising context functions");
    }

    // For actual integration, the correct ordinals or function names would need to be determined
    // using tools like Dependency Walker or dumpbin
    
    // Check if all critical functions were loaded successfully
    bool coreFunctionsLoaded = CreateExecuteContext && DestroyExecuteContext && 
                               CreateTaskContainer && DestroyTaskContainer;
    
    // Check if BVH building functions were loaded
    bool bvhFunctionsLoaded = CreateGeometryHandles && DestroyGeometryHandles &&
                              CreateInstanceHandles && DestroyInstanceHandles &&
                              ScheduleBVHTask && BuildGPUTask && MarkGPUTaskAsCompleted;
    
    if (!coreFunctionsLoaded) {
        logger::error("[KickstartRTWrapper] Failed to load core functions");
        ResetFunctions();
        return false;
    }
    
    if (!bvhFunctionsLoaded) {
        logger::warn("[KickstartRTWrapper] Some BVH functions could not be loaded - limited functionality available");
    }
    
    logger::info("[KickstartRTWrapper] Successfully loaded KickstartRT API functions");
    return true;
}

void KickstartRTWrapper::ResetFunctions() {
    // ExecuteContext functions
    CreateExecuteContext = nullptr;
    DestroyExecuteContext = nullptr;
    
    // Task container functions
    CreateTaskContainer = nullptr;
    DestroyTaskContainer = nullptr;
    
    // Geometry handle functions
    CreateGeometryHandle = nullptr;
    CreateGeometryHandles = nullptr;
    DestroyGeometryHandle = nullptr;
    DestroyGeometryHandles = nullptr;
    DestroyAllGeometryHandles = nullptr;
    
    // Instance handle functions
    CreateInstanceHandle = nullptr;
    CreateInstanceHandles = nullptr;
    DestroyInstanceHandle = nullptr;
    DestroyInstanceHandles = nullptr;
    DestroyAllInstanceHandles = nullptr;
    
    // Task scheduling functions
    ScheduleBVHTask = nullptr;
    ScheduleRenderTask = nullptr;
    
    // GPU task execution functions
    BuildGPUTask = nullptr;
    MarkGPUTaskAsCompleted = nullptr;
    
    // Denoising context functions
    CreateDenoisingContextFromD3D11Device = nullptr;
    DestroyDenoisingContext = nullptr;
}

bool KickstartRTWrapper::Initialize() {
    // Check if already initialized
    if (Handle) {
        return true;
    }
    
    logger::info("[KickstartRTWrapper] Initializing KickstartRT wrapper");
    
    // Try to load the DLL
    Handle = LoadLibraryA("KickstartRT_Interop_D3D11.dll");
    if (!Handle) {
        DWORD error = GetLastError();
        logger::error("[KickstartRTWrapper] Failed to load KickstartRT_Interop_D3D11.dll, error: {}", error);
        return false;
    }
    
    // Load functions from the DLL
    if (!LoadFunctions(Handle)) {
        logger::error("[KickstartRTWrapper] Failed to load functions from KickstartRT_Interop_D3D11.dll");
        FreeLibrary(Handle);
        Handle = nullptr;
        return false;
    }
    
    logger::info("[KickstartRTWrapper] KickstartRT wrapper initialized successfully");
    return true;
} 