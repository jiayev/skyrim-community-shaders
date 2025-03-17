#include "KickstartRTImpl.h"
#include "Globals.h"

namespace KickstartRTImpl
{
    // Core rendering functions - simplified implementations for now
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
            logger::error("[KickstartRTImpl] GenerateGI called without initialized context");
            return false;
        }

        // Check for required resources
        if (!depthSRV || !outputUAV) {
            logger::error("[KickstartRTImpl] GenerateGI called with null resources");
            return false;
        }

        // Log what we're doing
        logger::info("[KickstartRTImpl] Running GenerateGI with provided resources");

        try {
            // Get the device context from globals
            auto d3dContext = globals::d3d::context;
            if (!d3dContext) {
                logger::error("[KickstartRTImpl] D3D11 device context not available");
                return false;
            }

            // Create a task container with proper synchronization setup
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[KickstartRTImpl] Failed to create task container for GI");
                return false;
            }

            // Schedule BVH Build task - this is required before any rendering tasks
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;  // Build the top-level acceleration structure
            bvhBuildTask.maxBlasBuildCount = 4u;  // Process up to 4 bottom-level acceleration structures
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Set up diffuse GI tracing
            KickstartRT::D3D11::RenderTask::TraceDiffuseTask traceTask;
            
            // Configure input buffers - note that the KickstartRT API requires specific setup
            // Get resources from SRVs
            ID3D11Resource* depthResource = nullptr;
            ID3D11Resource* normalResource = nullptr;
            ID3D11Resource* outputResource = nullptr;
            
            // Extract the underlying resources from the views
            if (depthSRV) {
                depthSRV->GetResource(&depthResource);
                traceTask.common.depth.tex.resource = depthResource;
                // Shader resource view description - required for KickstartRT to correctly access the texture
                traceTask.common.depth.tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                traceTask.common.depth.tex.srvDesc.Texture2D.MipLevels = 1;
                traceTask.common.depth.tex.srvDesc.Texture2D.MostDetailedMip = 0;
            }
            
            if (normalSRV) {
                normalSRV->GetResource(&normalResource);
                traceTask.common.normal.tex.resource = normalResource;
                // Shader resource view description
                traceTask.common.normal.tex.srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                traceTask.common.normal.tex.srvDesc.Texture2D.MipLevels = 1;
                traceTask.common.normal.tex.srvDesc.Texture2D.MostDetailedMip = 0;
            }
            
            // Configure output buffer
            if (outputUAV) {
                outputUAV->GetResource(&outputResource);
                traceTask.out.resource = outputResource;
                // Unordered access view description
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
            
            // Release resources - we need to release any resources we acquired
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (outputResource) outputResource->Release();
            
            // Execute GPU task - using proper fence synchronization
            ID3D11CommandList* commandList = nullptr;
            d3dContext->FinishCommandList(false, &commandList);

            if (commandList) {
                KickstartRT::D3D11::BuildGPUTaskInput taskInput4 = {};
                taskInput4.geometryTaskFirst = true;
                taskInput4.maxBlasBuildCount = 16u;
                
                // Add synchronization fences if available
                if (g_renderFence) {
                    taskInput4.signalFence = g_renderFence.Get();
                    taskInput4.signalFenceValue = g_fenceValue++;
                }
                
                auto status4 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput4);
                commandList->Release();
                
                if (status4 == KickstartRT::Status::OK) {
                    logger::info("[KickstartRTImpl] Successfully generated GI");
                    return true;
                } else {
                    logger::error("[KickstartRTImpl] Failed to execute GI task: {}", static_cast<int>(status4));
                    return false;
                }
            } else {
                // Fall back to direct execution without command lists if we couldn't create one
                logger::warn("[KickstartRTImpl] Could not create command list, executing directly");
                KickstartRT::D3D11::BuildGPUTaskInput taskInput5 = {};
                taskInput5.geometryTaskFirst = true;
                taskInput5.maxBlasBuildCount = 16u;
                
                // Add synchronization fences if available
                if (g_renderFence) {
                    taskInput5.signalFence = g_renderFence.Get();
                    taskInput5.signalFenceValue = g_fenceValue++;
                }
                
                auto status5 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput5);
                
                if (status5 == KickstartRT::Status::OK) {
                    logger::info("[KickstartRTImpl] Successfully generated GI (direct execution)");
                    return true;
                } else {
                    logger::error("[KickstartRTImpl] Failed to execute GI task: {}", static_cast<int>(status5));
                    return false;
                }
            }
        } catch (const std::exception& e) {
            logger::error("[KickstartRTImpl] Exception in GenerateGI: {}", e.what());
            return false;
        } catch (...) {
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
            
            // Release resources - we need to release any resources we acquired
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (roughnessResource) roughnessResource->Release();
            if (outputResource) outputResource->Release();
            
            // Execute GPU task - this is where KickstartRT actually processes all scheduled tasks
            KickstartRT::D3D11::BuildGPUTaskInput taskInput6 = {};
            taskInput6.geometryTaskFirst = true;
            taskInput6.maxBlasBuildCount = 16u;
            
            // Add synchronization fences if available
            if (g_renderFence) {
                taskInput6.signalFence = g_renderFence.Get();
                taskInput6.signalFenceValue = g_fenceValue++;
            }
            
            auto status6 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput6);
            
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