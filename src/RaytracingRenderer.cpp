#include "KickstartRTImpl.h"
#include "Globals.h"
#include "State.h"

namespace KickstartRTImpl
{
    // Core rendering functions - simplified implementations for now
    /**
     * Inject Direct Lighting into KickstartRT's cache
     * 
     * This function creates a DirectLightingInjectionTask to store direct lighting 
     * information in the KickstartRT cache, which is essential for GI and reflections.
     * 
     * @param directLightingSRV Shader resource view for the direct lighting buffer
     * @param depthSRV Shader resource view for the depth buffer
     * @param viewMatrix Current view matrix
     * @param projMatrix Current projection matrix
     * @return true if successful, false otherwise
     */
    bool InjectDirectLighting(
        ID3D11ShaderResourceView* directLightingSRV,
        ID3D11ShaderResourceView* depthSRV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix)
    {
        // Check if we have an initialized context
        if (!g_executeContext) {
            logger::debug("[KickstartRTImpl] InjectDirectLighting called without initialized context");
            return false;
        }

        // Check for required resources
        if (!directLightingSRV || !depthSRV) {
            logger::error("[KickstartRTImpl] InjectDirectLighting called with null resources");
            return false;
        }

        try {
            // Create a task container for the lighting injection
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[KickstartRTImpl] Failed to create task container for direct lighting injection");
                return false;
            }

            // Setup the DirectLightingInjectionTask
            KickstartRT::D3D11::RenderTask::DirectLightingInjectionTask injectionTask;
            
            // Extract resources from the views
            ID3D11Resource* directLightingResource = nullptr;
            ID3D11Resource* depthResource = nullptr;
            
            // Get direct lighting resource
            if (directLightingSRV) {
                directLightingSRV->GetResource(&directLightingResource);
                if (directLightingResource) {
                    injectionTask.directLighting.resource = directLightingResource;
                    // Get SRV description
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                    directLightingSRV->GetDesc(&srvDesc);
                    injectionTask.directLighting.srvDesc = srvDesc;
                }
            }
            
            // Get depth resource
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                if (depthResource) {
                    injectionTask.depth.tex.resource = depthResource;
                    // Get SRV description
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                    depthSRV->GetDesc(&srvDesc);
                    injectionTask.depth.tex.srvDesc = srvDesc;
                }
            }
            
            // Setup view matrices - Convert view and projection matrices to the format expected by KickstartRT
            // Calculate the inverse projection matrix
            DirectX::XMMATRIX invProjMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&projMatrix));
            DirectX::XMFLOAT4X4 invProj;
            DirectX::XMStoreFloat4x4(&invProj, invProjMatrix);
            
            // Calculate the inverse view matrix
            DirectX::XMMATRIX invViewMatrix = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewMatrix));
            DirectX::XMFLOAT4X4 invView;
            DirectX::XMStoreFloat4x4(&invView, invViewMatrix);
            
            // Convert to KickstartRT format
            injectionTask.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invProj);
            injectionTask.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invView);
            
            // Set viewport dimensions from the depth resource
            if (depthResource) {
                ID3D11Texture2D* depthTex = nullptr;
                HRESULT hr = depthResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex));
                if (SUCCEEDED(hr) && depthTex) {
                    D3D11_TEXTURE2D_DESC depthDesc;
                    depthTex->GetDesc(&depthDesc);
                    injectionTask.viewport.width = depthDesc.Width;
                    injectionTask.viewport.height = depthDesc.Height;
                    depthTex->Release();
                }
                else {
                    // Fallback to default dimensions
                    injectionTask.viewport.width = 1920;
                    injectionTask.viewport.height = 1080;
                }
            }
            else {
                // Fallback to default dimensions
                injectionTask.viewport.width = 1920;
                injectionTask.viewport.height = 1080;
            }
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&injectionTask);
            
            // Setup proper synchronization for GPU task execution
            KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
            taskInput.geometryTaskFirst = true;       // Process geometry tasks first
            taskInput.maxBlasBuildCount = 4u;         // Limit BLAS builds per frame
            
            // Add synchronization fences if available
            if (g_renderFence) {
                // Wait for previous operations to complete
                taskInput.waitFence = g_renderFence.Get();
                taskInput.waitFenceValue = g_fenceValue;
                
                // Signal when our operations are done
                taskInput.signalFence = g_renderFence.Get();
                taskInput.signalFenceValue = g_fenceValue + 1;
                
                // Increment for next use
                g_fenceValue++;
            }
            else {
                logger::error("[KickstartRTImpl] Cannot execute GPU task without a valid fence for synchronization");
                return false;
            }
            
            // Execute the GPU task
            auto status = g_executeContext->InvokeGPUTask(taskContainer, &taskInput);
            
            // Only release resources after GPU task execution
            if (directLightingResource) directLightingResource->Release();
            if (depthResource) depthResource->Release();
            
            if (status == KickstartRT::Status::OK) {
                logger::debug("[KickstartRTImpl] Successfully injected direct lighting");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute direct lighting injection task: {}", static_cast<int>(status));
                return false;
            }
        } 
        catch (const std::exception& e) {
            logger::error("[KickstartRTImpl] Exception in InjectDirectLighting: {}", e.what());
            return false;
        } 
        catch (...) {
            logger::error("[KickstartRTImpl] Unknown exception in InjectDirectLighting");
            return false;
        }
    }

    /**
     * Generate Global Illumination using KickstartRT
     * 
     * This function creates a TraceDiffuseTask to generate global illumination effects
     * using ray tracing through the KickstartRT API.
     * 
     * @param depthSRV Shader resource view for the depth buffer
     * @param normalSRV Shader resource view for the normal buffer
     * @param outputUAV Unordered access view for the output buffer
     * @param viewMatrix Current view matrix
     * @param projMatrix Current projection matrix
     * @return true if successful, false otherwise
     */
    bool GenerateGI(
        ID3D11ShaderResourceView* depthSRV,
        ID3D11ShaderResourceView* normalSRV,
        ID3D11UnorderedAccessView* outputUAV,
        DirectX::XMFLOAT4X4 viewMatrix,
        DirectX::XMFLOAT4X4 projMatrix)
    {
        // Check if we have an initialized context
        if (!g_executeContext) {
            logger::debug("[KickstartRTImpl] GenerateGI called without initialized context");
            return false;
        }

        // Check for required resources
        if (!depthSRV || !outputUAV) {
            logger::error("[KickstartRTImpl] GenerateGI called with null resources");
            return false;
        }

        // Get the device context from globals
        auto d3dContext = globals::d3d::context;
        if (!d3dContext) {
            logger::error("[KickstartRTImpl] D3D11 device context not available");
            return false;
        }

        try {
            // Create a task container with proper synchronization setup
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[KickstartRTImpl] Failed to create task container for GI");
                return false;
            }

            // Schedule BVH Build task - this is required before any rendering tasks
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;  // Build the top-level acceleration structure
            bvhBuildTask.maxBlasBuildCount = 4u;  // Process up to 4 bottom-level acceleration structures per frame
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Set up diffuse GI tracing
            KickstartRT::D3D11::RenderTask::TraceDiffuseTask traceTask;
            
            // Extract resources from the views
            ID3D11Resource* depthResource = nullptr;
            ID3D11Resource* normalResource = nullptr;
            ID3D11Resource* outputResource = nullptr;
            
            // Get depth resource
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                if (depthResource) {
                    traceTask.common.depth.tex.resource = depthResource;
                    // Get SRV description
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                    depthSRV->GetDesc(&srvDesc);
                    traceTask.common.depth.tex.srvDesc = srvDesc;
                }
            }
            
            // Get normal resource if available
            if (normalSRV) {
                normalSRV->GetResource(&normalResource);
                if (normalResource) {
                    traceTask.common.normal.tex.resource = normalResource;
                    // Get SRV description
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
                    normalSRV->GetDesc(&srvDesc);
                    traceTask.common.normal.tex.srvDesc = srvDesc;
                }
            }
            
            // Get output resource
            if (outputUAV) {
                outputUAV->GetResource(&outputResource);
                if (outputResource) {
                    traceTask.out.resource = outputResource;
                    // Get UAV description
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc;
                    outputUAV->GetDesc(&uavDesc);
                    traceTask.out.uavDesc = uavDesc;
                }
            }
            
            // Setup view matrices - Convert view and projection matrices to the format expected by KickstartRT
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
            
            // Set viewport dimensions from the depth resource
            if (depthResource) {
                ID3D11Texture2D* depthTex = nullptr;
                HRESULT hr = depthResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex));
                if (SUCCEEDED(hr) && depthTex) {
                    D3D11_TEXTURE2D_DESC depthDesc;
                    depthTex->GetDesc(&depthDesc);
                    traceTask.common.viewport.width = depthDesc.Width;
                    traceTask.common.viewport.height = depthDesc.Height;
                    depthTex->Release();
                }
                else {
                    // Fallback to default dimensions
                    traceTask.common.viewport.width = 1920;
                    traceTask.common.viewport.height = 1080;
                }
            }
            else {
                // Fallback to default dimensions
                traceTask.common.viewport.width = 1920;
                traceTask.common.viewport.height = 1080;
            }
            
            // Configure GI ray tracing parameters
            traceTask.common.maxRayLength = 500.0f;  // Maximum ray distance for GI
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Setup proper synchronization for GPU task execution
            KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
            taskInput.geometryTaskFirst = true;       // Process geometry tasks first
            taskInput.maxBlasBuildCount = 4u;         // Limit BLAS builds per frame
            
            // Add synchronization fences if available
            if (g_renderFence) {
                // Wait for previous operations to complete
                taskInput.waitFence = g_renderFence.Get();
                taskInput.waitFenceValue = g_fenceValue;
                
                // Signal when our operations are done
                taskInput.signalFence = g_renderFence.Get();
                taskInput.signalFenceValue = g_fenceValue + 1;
                
                // Increment for next use
                g_fenceValue++;
            }
            else {
                logger::error("[KickstartRTImpl] Cannot execute GPU task without a valid fence for synchronization");
                return false;
            }
            
            // Execute the GPU task
            auto status = g_executeContext->InvokeGPUTask(taskContainer, &taskInput);
            
            // Only release resources after GPU task execution
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (outputResource) outputResource->Release();
            
            if (status == KickstartRT::Status::OK) {
                logger::debug("[KickstartRTImpl] Successfully generated GI");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute GI task: {}", static_cast<int>(status));
                return false;
            }
        } 
        catch (const std::exception& e) {
            logger::error("[KickstartRTImpl] Exception in GenerateGI: {}", e.what());
            return false;
        } 
        catch (...) {
            logger::error("[KickstartRTImpl] Unknown exception in GenerateGI");
            return false;
        }
    }

    /**
     * Generate reflections using KickstartRT
     * 
     * This function creates a TraceSpecularTask to generate reflection effects
     * using ray tracing through the KickstartRT API.
     * 
     * @param depthSRV Shader resource view for the depth buffer
     * @param normalSRV Shader resource view for the normal buffer
     * @param roughnessSRV Shader resource view for the roughness buffer
     * @param outputUAV Unordered access view for the output buffer
     * @param viewMatrix Current view matrix
     * @param projMatrix Current projection matrix
     * @return true if successful, false otherwise
     */
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
            if (!taskContainer) {
                logger::error("[KickstartRTImpl] Failed to create task container for reflections");
                return false;
            }

            // Schedule BVH Build task
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;
            bvhBuildTask.maxBlasBuildCount = 4u;
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Set up specular reflection tracing
            KickstartRT::D3D11::RenderTask::TraceSpecularTask traceTask;
            
            // Configure input buffers - note that the KickstartRT API requires specific setup
            // Get resources from SRVs
            ID3D11Resource* depthResource = nullptr;
            ID3D11Resource* normalResource = nullptr;
            ID3D11Resource* roughnessResource = nullptr;
            ID3D11Resource* outputResource = nullptr;
            
            // Extract the underlying resources from the views
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                traceTask.common.depth.tex.resource = depthResource;
                // Set view description - required for KickstartRT to correctly access the texture
                traceTask.common.depth.tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                traceTask.common.depth.tex.srvDesc.Texture2D.MipLevels = 1;
                traceTask.common.depth.tex.srvDesc.Texture2D.MostDetailedMip = 0;
            }
            
            if (normalSRV) {
                normalSRV->GetResource(&normalResource);
                traceTask.common.normal.tex.resource = normalResource;
                // Set view description - required for KickstartRT to correctly access the texture
                traceTask.common.normal.tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                traceTask.common.normal.tex.srvDesc.Texture2D.MipLevels = 1;
                traceTask.common.normal.tex.srvDesc.Texture2D.MostDetailedMip = 0;
            }
            
            if (roughnessSRV) {
                roughnessSRV->GetResource(&roughnessResource);
                traceTask.common.roughness.tex.resource = roughnessResource;
                // Set view description - required for KickstartRT to correctly access the texture
                traceTask.common.roughness.tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                traceTask.common.roughness.tex.srvDesc.Texture2D.MipLevels = 1;
                traceTask.common.roughness.tex.srvDesc.Texture2D.MostDetailedMip = 0;
            }
            
            // Configure output buffer
            if (outputUAV) {
                outputUAV->GetResource(&outputResource);
                traceTask.out.resource = outputResource;
                // Set view description - required for KickstartRT to correctly access the texture
                traceTask.out.uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                traceTask.out.uavDesc.Texture2D.MipSlice = 0;
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
            
            // Convert to KickstartRT format - this maps from clip space to view space and view space to world space
            traceTask.common.clipToViewMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invProj);
            traceTask.common.viewToWorldMatrix = *reinterpret_cast<const KickstartRT::Math::Float_4x4*>(&invView);
            
            // Get dimensions from the depth resource - needed for viewport setup
            D3D11_TEXTURE2D_DESC depthDesc;
            if (depthResource) {
                ID3D11Texture2D* depthTex = nullptr;
                HRESULT hr = depthResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTex));
                if (SUCCEEDED(hr) && depthTex) {
                    depthTex->GetDesc(&depthDesc);
                    traceTask.common.viewport.width = depthDesc.Width;
                    traceTask.common.viewport.height = depthDesc.Height;
                    depthTex->Release();
                } else {
                    // Fallback to screen dimensions
                    traceTask.common.viewport.width = 1920;
                    traceTask.common.viewport.height = 1080;
                }
            } else {
                // Fallback to screen dimensions
                traceTask.common.viewport.width = 1920;
                traceTask.common.viewport.height = 1080;
            }
            
            // Set ray parameters
            traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Execute GPU task - this is where KickstartRT actually processes all scheduled tasks
            KickstartRT::D3D11::BuildGPUTaskInput taskInput6 = {};
            taskInput6.geometryTaskFirst = true;
            taskInput6.maxBlasBuildCount = 16u;
            
            // Add synchronization fences if available
            if (g_renderFence) {
                taskInput6.waitFence = g_renderFence.Get();
                taskInput6.waitFenceValue = g_fenceValue;
                taskInput6.signalFence = g_renderFence.Get();
                taskInput6.signalFenceValue = g_fenceValue + 1;
                g_fenceValue++; // Increment for next use
            }
            else {
                logger::error("[RT] Cannot execute GPU task without a valid fence for synchronization");
                return false;
            }
            
            auto status6 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput6);
            
            // Only release resources after GPU task execution
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (roughnessResource) roughnessResource->Release();
            if (outputResource) outputResource->Release();
            
            if (status6 == KickstartRT::Status::OK) {
                logger::info("[KickstartRTImpl] Successfully generated reflections");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute reflections task: {}", static_cast<int>(status6));
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
} // namespace KickstartRTImpl 