#include "../include/KickstartRTImpl.h"
#include <DirectXMath.h>
#include <iostream>

// KickstartRT log callback
void KickstartRTImpl::LogCallback(KickstartRT::Log::Severity severity, const char* msg) {
    const char* severityStr = "UNKNOWN";
    switch (severity) {
    case KickstartRT::Log::Severity::Debug:
        severityStr = "DEBUG";
        break;
    case KickstartRT::Log::Severity::Info:
        severityStr = "INFO";
        break;
    case KickstartRT::Log::Severity::Warning:
        severityStr = "WARNING";
        break;
    case KickstartRT::Log::Severity::Error:
        severityStr = "ERROR";
        break;
    }
    std::cout << "[KickstartRT] " << severityStr << ": " << msg << std::endl;
}

KickstartRTImpl::KickstartRTImpl() {}

KickstartRTImpl::~KickstartRTImpl() {
    Shutdown();
}

bool KickstartRTImpl::Initialize(ID3D11Device* device) {
    if (m_initialized) {
        return true;
    }

    // Set up logging
    KickstartRT::Log::SetMinSeverity(KickstartRT::Log::Severity::Info);
    KickstartRT::Log::SetCallback(&KickstartRTImpl::LogCallback);

    // Initialize KickstartRT ExecuteContext
    KickstartRT::D3D11::ExecuteContext_InitSettings settings = {};
    settings.supportedWorkingsets = 4; // Number of command lists that can be in-flight
    settings.supportedTextureSRVCount = 256; // Maximum number of texture SRVs
    settings.supportedBufferSRVCount = 256; // Maximum number of buffer SRVs
    settings.supportedRTVCount = 16; // Maximum number of RTVs
    settings.supportedDSVCount = 16; // Maximum number of DSVs
    settings.supportedUAVCount = 64; // Maximum number of UAVs
    settings.deviceCreateContextFlags = 0; // Flags for device creation
    
    // Create the ExecuteContext
    KickstartRT::Status status = KickstartRT::D3D11::ExecuteContext::Create(
        &m_executeContext,
        device,
        &settings,
        KickstartRT::Version::SDKVersion);

    if (status != KickstartRT::Status::OK) {
        std::cout << "Failed to create KickstartRT::D3D11::ExecuteContext" << std::endl;
        return false;
    }

    m_initialized = true;
    return true;
}

void KickstartRTImpl::Shutdown() {
    if (!m_initialized) {
        return;
    }

    CleanupResources();

    // Delete the ExecuteContext
    if (m_executeContext) {
        delete m_executeContext;
        m_executeContext = nullptr;
    }

    m_initialized = false;
}

bool KickstartRTImpl::RegisterGeometry(
    const void* vertexBuffer, uint32_t vertexCount, uint32_t vertexStride,
    const void* indexBuffer, uint32_t indexCount, uint32_t indexStride,
    const DirectX::XMFLOAT4X4& transform,
    KickstartRT::BVHTask::GeometryHandle& outGeometryHandle,
    KickstartRT::BVHTask::InstanceHandle& outInstanceHandle) {
    
    if (!m_initialized || !m_executeContext) {
        return false;
    }
    
    // Create a KickstartRT task container
    KickstartRT::TaskContainer* taskContainer = nullptr;
    m_executeContext->CreateTaskContainer(&taskContainer);
    
    if (!taskContainer) {
        return false;
    }
    
    // Create the geometry input for KickstartRT
    KickstartRT::BVHTask::GeometryInput geoInput = {};
    
    // Only supporting triangles for now
    geoInput.primCount = indexCount / 3;
    
    // Set up a single geometry component with our vertex/index data
    KickstartRT::BVHTask::GeometryComponent component = {};
    component.indexBuffer = indexBuffer;
    component.indexCount = indexCount;
    component.vertexBuffer = vertexBuffer;
    component.vertexCount = vertexCount;
    component.vertexPositionStrideInBytes = vertexStride;
    component.indexStrideInBytes = indexStride;
    component.indexFormat = (indexStride == 2) ? 
        KickstartRT::BVHTask::GeometryComponent::IndexFormat::UInt16 : 
        KickstartRT::BVHTask::GeometryComponent::IndexFormat::UInt32;
    
    // Add the component to the geometry input
    geoInput.components.push_back(component);
    
    // Set the flags
    geoInput.allowUpdate = false; // Static geometry 
    geoInput.allowCompaction = true; // Allow BVH compaction
    geoInput.allowLightTransferTarget = true; // Allow light transfer
    
    // Create a geometry handle
    KickstartRT::Status status = m_executeContext->CreateGeometryHandle(
        &outGeometryHandle,
        &geoInput);
        
    if (status != KickstartRT::Status::OK) {
        delete taskContainer;
        return false;
    }
    
    // Register the geometry
    KickstartRT::BVHTask::GeometryTask geoTask = {};
    geoTask.type = KickstartRT::BVHTask::Task::Type::BuildGeometry;
    geoTask.geometry.handle = outGeometryHandle;
    geoTask.geometry.input = &geoInput;
    
    taskContainer->ScheduleBVHTask(&geoTask);
    
    // Create an instance and add it to the scene
    KickstartRT::BVHTask::InstanceInput instInput = {};
    instInput.geometryHandle = outGeometryHandle;
    
    // Convert transform to KickstartRT format
    DirectX::XMMATRIX mat = DirectX::XMLoadFloat4x4(&transform);
    DirectX::XMFLOAT3X4 float3x4;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            float3x4.m[row][col] = transform.m[row][col];
        }
    }
    
    KickstartRT::Math::Float3x4 ksTransform;
    memcpy(&ksTransform, &float3x4, sizeof(float3x4));
    instInput.transform = ksTransform;
    
    // Set mask bits for what rays this geometry should be visible to
    instInput.inclusionMask = 
        KickstartRT::BVHTask::InstanceInclusionMask::All;
    
    // Create an instance handle
    status = m_executeContext->CreateInstanceHandle(
        &outInstanceHandle,
        &instInput);
    
    if (status != KickstartRT::Status::OK) {
        delete taskContainer;
        return false;
    }
    
    // Add the instance to the scene
    KickstartRT::BVHTask::InstanceTask instTask = {};
    instTask.type = KickstartRT::BVHTask::Task::Type::BuildInstance;
    instTask.instance.handle = outInstanceHandle;
    instTask.instance.input = &instInput;
    
    taskContainer->ScheduleBVHTask(&instTask);
    
    // Add a BVH build task to build the acceleration structure
    KickstartRT::BVHTask::BVHBuildTask buildTask = {};
    buildTask.type = KickstartRT::BVHTask::Task::Type::BuildBVH;
    buildTask.buildBVH.buildTLAS = true;
    buildTask.buildBVH.buildBLAS = true;
    buildTask.buildBVH.forceRebuildBLAS = true;
    buildTask.buildBVH.updateGeometryInPlace = false;
    buildTask.buildBVH.instanceInclusionMask = 
        KickstartRT::BVHTask::InstanceInclusionMask::All;
        
    taskContainer->ScheduleBVHTask(&buildTask);
    
    // Build the GPU task
    KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
    taskInput.commandListDependency = nullptr; // No dependent command list
    
    status = m_executeContext->InvokeGPUTask(taskContainer, &taskInput);
    
    // The task container gets consumed by InvokeGPUTask, so no need to delete it
    
    return (status == KickstartRT::Status::OK);
}

bool KickstartRTImpl::UpdateInstanceTransform(
    KickstartRT::BVHTask::InstanceHandle instanceHandle,
    const DirectX::XMFLOAT4X4& transform) {
    
    if (!m_initialized || !m_executeContext) {
        return false;
    }
    
    // Create a task container
    KickstartRT::TaskContainer* taskContainer = nullptr;
    m_executeContext->CreateTaskContainer(&taskContainer);
    
    if (!taskContainer) {
        return false;
    }
    
    // Create the instance input
    KickstartRT::BVHTask::InstanceInput instInput = {};
    
    // Convert transform to KickstartRT format
    DirectX::XMFLOAT3X4 float3x4;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            float3x4.m[row][col] = transform.m[row][col];
        }
    }
    
    KickstartRT::Math::Float3x4 ksTransform;
    memcpy(&ksTransform, &float3x4, sizeof(float3x4));
    instInput.transform = ksTransform;
    
    // Update instance transform
    KickstartRT::BVHTask::InstanceTask instTask = {};
    instTask.type = KickstartRT::BVHTask::Task::Type::UpdateInstance;
    instTask.instance.handle = instanceHandle;
    instTask.instance.input = &instInput;
    
    taskContainer->ScheduleBVHTask(&instTask);
    
    // Add a BVH build task to rebuild the acceleration structure
    KickstartRT::BVHTask::BVHBuildTask buildTask = {};
    buildTask.type = KickstartRT::BVHTask::Task::Type::BuildBVH;
    buildTask.buildBVH.buildTLAS = true;
    buildTask.buildBVH.buildBLAS = false; // BLAS doesn't need to be rebuilt for transform update
    buildTask.buildBVH.forceRebuildBLAS = false;
    buildTask.buildBVH.updateGeometryInPlace = false;
    buildTask.buildBVH.instanceInclusionMask = 
        KickstartRT::BVHTask::InstanceInclusionMask::All;
        
    taskContainer->ScheduleBVHTask(&buildTask);
    
    // Build the GPU task
    KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
    taskInput.commandListDependency = nullptr; // No dependent command list
    
    KickstartRT::Status status = m_executeContext->InvokeGPUTask(taskContainer, &taskInput);
    
    // The task container gets consumed by InvokeGPUTask, so no need to delete it
    
    return (status == KickstartRT::Status::OK);
}

bool KickstartRTImpl::InjectLighting(
    ID3D11ShaderResourceView* lightingBufferSRV,
    ID3D11ShaderResourceView* depthBufferSRV,
    ID3D11ShaderResourceView* normalBufferSRV,
    const DirectX::XMFLOAT4X4& viewMatrix,
    const DirectX::XMFLOAT4X4& projMatrix) {
    
    if (!m_initialized || !m_executeContext) {
        return false;
    }
    
    // Create a task container
    KickstartRT::TaskContainer* taskContainer = nullptr;
    m_executeContext->CreateTaskContainer(&taskContainer);
    
    if (!taskContainer) {
        return false;
    }
    
    // Set up light injection task
    KickstartRT::RenderTask::DirectLightingInjectionTask injectTask = {};
    injectTask.type = KickstartRT::RenderTask::Task::Type::DirectLightingInjection;
    
    // Input textures
    injectTask.input.directLighting.resource = lightingBufferSRV;
    injectTask.input.directLighting.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    injectTask.input.directLighting.format = KickstartRT::RenderTask::ShaderResourceTex::Format::RGBA32F;
    
    injectTask.input.depth.resource = depthBufferSRV;
    injectTask.input.depth.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    injectTask.input.depth.format = KickstartRT::RenderTask::ShaderResourceTex::Format::R32F;
    
    injectTask.input.normal.resource = normalBufferSRV;
    injectTask.input.normal.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    injectTask.input.normal.format = KickstartRT::RenderTask::ShaderResourceTex::Format::RGBA32F;
    
    // Convert view matrix to KickstartRT format
    DirectX::XMFLOAT4X4 invView;
    DirectX::XMMATRIX invViewMat = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&viewMatrix));
    DirectX::XMStoreFloat4x4(&invView, invViewMat);
    
    KickstartRT::Math::Float4x4 ksViewMatrix;
    memcpy(&ksViewMatrix, &viewMatrix, sizeof(viewMatrix));
    
    KickstartRT::Math::Float4x4 ksProjMatrix;
    memcpy(&ksProjMatrix, &projMatrix, sizeof(projMatrix));
    
    injectTask.input.viewMatrix = ksViewMatrix;
    injectTask.input.projMatrix = ksProjMatrix;
    
    // Configure injection parameters
    injectTask.injectionResolutionStride = 1; // Full resolution
    
    // Schedule the task
    taskContainer->ScheduleRenderTask(&injectTask);
    
    // Build the GPU task
    KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
    taskInput.commandListDependency = nullptr; // No dependent command list
    
    KickstartRT::Status status = m_executeContext->InvokeGPUTask(taskContainer, &taskInput);
    
    // The task container gets consumed by InvokeGPUTask, so no need to delete it
    
    return (status == KickstartRT::Status::OK);
}

bool KickstartRTImpl::GenerateReflections(
    ID3D11ShaderResourceView* depthBufferSRV,
    ID3D11ShaderResourceView* normalBufferSRV,
    ID3D11ShaderResourceView* roughnessBufferSRV,
    ID3D11UnorderedAccessView* outputUAV,
    const DirectX::XMFLOAT4X4& viewMatrix,
    const DirectX::XMFLOAT4X4& projMatrix) {
    
    if (!m_initialized || !m_executeContext) {
        return false;
    }
    
    // Get current viewport dimensions
    D3D11_TEXTURE2D_DESC depthDesc;
    ID3D11Resource* depthResource = nullptr;
    depthBufferSRV->GetResource(&depthResource);
    if (depthResource) {
        ID3D11Texture2D* depthTexture = nullptr;
        if (SUCCEEDED(depthResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&depthTexture))) {
            depthTexture->GetDesc(&depthDesc);
            depthTexture->Release();
            
            // Check if we need to recreate the denoising context
            if (m_width != depthDesc.Width || m_height != depthDesc.Height) {
                m_width = depthDesc.Width;
                m_height = depthDesc.Height;
                
                // Destroy old context if it exists
                if (m_denoisingContextHandle.handle != 0) {
                    m_executeContext->DestroyDenoisingContextHandle(m_denoisingContextHandle);
                    m_denoisingContextHandle = {};
                }
                
                // Create new denoising context
                KickstartRT::RenderTask::DenoisingContextInput denoisingInput = {};
                denoisingInput.width = m_width;
                denoisingInput.height = m_height;
                denoisingInput.halfResolution = false; // Full resolution denoising
                
                KickstartRT::Status status = m_executeContext->CreateDenoisingContextHandle(
                    &m_denoisingContextHandle,
                    &denoisingInput);
                    
                if (status != KickstartRT::Status::OK) {
                    depthResource->Release();
                    return false;
                }
            }
        }
        depthResource->Release();
    }
    
    // Check if denoising context exists
    if (m_denoisingContextHandle.handle == 0) {
        return false;
    }
    
    // Create a task container
    KickstartRT::TaskContainer* taskContainer = nullptr;
    m_executeContext->CreateTaskContainer(&taskContainer);
    
    if (!taskContainer) {
        return false;
    }
    
    // Convert matrices to KickstartRT format
    KickstartRT::Math::Float4x4 ksViewMatrix;
    memcpy(&ksViewMatrix, &viewMatrix, sizeof(viewMatrix));
    
    KickstartRT::Math::Float4x4 ksProjMatrix;
    memcpy(&ksProjMatrix, &projMatrix, sizeof(projMatrix));
    
    // Set up the trace specular task first
    KickstartRT::RenderTask::TraceSpecularTask traceTask = {};
    traceTask.type = KickstartRT::RenderTask::Task::Type::TraceSpecular;
    
    // Input textures
    traceTask.input.depth.resource = depthBufferSRV;
    traceTask.input.depth.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    traceTask.input.depth.format = KickstartRT::RenderTask::ShaderResourceTex::Format::R32F;
    
    traceTask.input.normal.resource = normalBufferSRV;
    traceTask.input.normal.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    traceTask.input.normal.format = KickstartRT::RenderTask::ShaderResourceTex::Format::RGBA32F;
    
    traceTask.input.roughness.resource = roughnessBufferSRV;
    traceTask.input.roughness.type = KickstartRT::RenderTask::ShaderResourceTex::ResourceType::SRV;
    traceTask.input.roughness.format = KickstartRT::RenderTask::ShaderResourceTex::Format::R8G8B8A8_UNORM;
    
    // Set matrices
    traceTask.input.viewMatrix = ksViewMatrix;
    traceTask.input.projMatrix = ksProjMatrix;
    
    // Output texture (this will be denoised later)
    traceTask.output.resource = outputUAV;
    traceTask.output.format = KickstartRT::RenderTask::UnorderedAccessTex::Format::RGBA16F;
    
    // Configure ray tracing parameters
    traceTask.instanceInclusionMask = KickstartRT::BVHTask::InstanceInclusionMask::All;
    traceTask.maxRayLength = 100.0f; // Maximum ray length in world units
    traceTask.samplesPerPixel = 1; // Start with 1 sample per pixel
    
    // Schedule the trace task
    taskContainer->ScheduleRenderTask(&traceTask);
    
    // Set up denoising task for specular reflections
    KickstartRT::RenderTask::DenoiseSpecularTask denoiseTask = {};
    denoiseTask.type = KickstartRT::RenderTask::Task::Type::DenoiseSpecular;
    
    // Configure denoising
    denoiseTask.historyLength = 4; // Number of frames for temporal denoising
    denoiseTask.disocclusionThreshold = 0.05f; // Threshold for detecting disocclusion
    denoiseTask.contextHandle = m_denoisingContextHandle;
    
    // Input textures (same as trace task)
    denoiseTask.input.depth = traceTask.input.depth;
    denoiseTask.input.normal = traceTask.input.normal;
    denoiseTask.input.roughness = traceTask.input.roughness;
    denoiseTask.input.viewMatrix = ksViewMatrix;
    denoiseTask.input.projMatrix = ksProjMatrix;
    
    // Previous frame matrices (use current frame for first frame)
    static DirectX::XMFLOAT4X4 prevView = viewMatrix;
    static DirectX::XMFLOAT4X4 prevProj = projMatrix;
    
    KickstartRT::Math::Float4x4 ksPrevViewMatrix;
    memcpy(&ksPrevViewMatrix, &prevView, sizeof(prevView));
    
    KickstartRT::Math::Float4x4 ksPrevProjMatrix;
    memcpy(&ksPrevProjMatrix, &prevProj, sizeof(prevProj));
    
    denoiseTask.input.prevViewMatrix = ksPrevViewMatrix;
    denoiseTask.input.prevProjMatrix = ksPrevProjMatrix;
    
    // Update matrices for next frame
    prevView = viewMatrix;
    prevProj = projMatrix;
    
    // Input noisy specular texture
    denoiseTask.input.inSpecular = traceTask.output;
    
    // Output denoised texture (same as traceTask output)
    denoiseTask.output.outSpecular = traceTask.output;
    
    // Schedule the denoise task
    taskContainer->ScheduleRenderTask(&denoiseTask);
    
    // Build the GPU task
    KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
    taskInput.commandListDependency = nullptr; // No dependent command list
    
    KickstartRT::Status status = m_executeContext->InvokeGPUTask(taskContainer, &taskInput);
    
    // The task container gets consumed by InvokeGPUTask, so no need to delete it
    
    return (status == KickstartRT::Status::OK);
}

void KickstartRTImpl::CleanupResources() {
    if (!m_initialized || !m_executeContext) {
        return;
    }
    
    // Destroy denoising context
    if (m_denoisingContextHandle.handle != 0) {
        m_executeContext->DestroyDenoisingContextHandle(m_denoisingContextHandle);
        m_denoisingContextHandle = {};
    }
    
    // Release any pending resources
    m_executeContext->ReleaseDeviceResourcesImmediately();
} 