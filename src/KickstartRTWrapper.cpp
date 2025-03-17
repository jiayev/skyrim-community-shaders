#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"

// Explicitly export the KickstartRT functions to resolve linking issues
extern "C" {
    // These are the functions we need to export
    __declspec(dllexport) KickstartRT::Status STDCALL KickstartRT_D3D11_ExecuteContext_Init(
        const KickstartRT::D3D11::ExecuteContext_InitSettings* settings,
        KickstartRT::D3D11::ExecuteContext** exc,
        const KickstartRT::Version version)
    {
        return KickstartRT::D3D11::ExecuteContext::Init(settings, exc, version);
    }

    __declspec(dllexport) KickstartRT::Status STDCALL KickstartRT_D3D11_ExecuteContext_Destruct(
        KickstartRT::D3D11::ExecuteContext* exc)
    {
        return KickstartRT::D3D11::ExecuteContext::Destruct(exc);
    }
}