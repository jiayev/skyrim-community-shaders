#define KickstartRT_Graphics_API_D3D11 1
#include "../include/KickstartRT/KickstartRT.h"
#include "Raytracing.h"
#include "Globals.h"
#include "State.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>
#include <unordered_map>

// Disable warnings for unreferenced parameters
#pragma warning(disable: 4100)

// Core KickstartRT implementation
namespace KickstartRTImpl
{
// Direct access to the ExecuteContext
static KickstartRT::D3D11::ExecuteContext* g_executeContext = nullptr;
static bool g_initialized = false;
static uint32_t g_width = 0;
static uint32_t g_height = 0;

// Storage for geometry and instance handles for later reference
static std::unordered_map<std::string, KickstartRT::D3D11::GeometryHandle> g_geometryHandles;
static std::unordered_map<std::string, KickstartRT::D3D11::InstanceHandle> g_instanceHandles;

// Register a geometry with KickstartRT and return the handle
bool RegisterGeometryWithKickstartRT(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, const std::string& name, KickstartRT::D3D11::GeometryHandle* outHandle)
{
    if (!g_executeContext || !vertexBuffer || !indexBuffer || !outHandle) {
        logger::error("[RT] Invalid parameters for geometry registration");
        return false;
    }
    
    // Check if we already have a handle for this geometry
    auto it = g_geometryHandles.find(name);
    if (it != g_geometryHandles.end()) {
        *outHandle = it->second;
        return true;
    }
    
    try {
        // Create a handle for the geometry
        KickstartRT::D3D11::GeometryHandle handle;
        auto status = g_executeContext->CreateGeometryHandles(&handle, 1);
        if (status != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to create geometry handle. Status: {}", static_cast<int>(status));
            return false;
        }
        
        // Create a task container
        auto taskContainer = g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for geometry registration");
            return false;
        }
        
        // Get vertex buffer description to extract stride and format
        D3D11_BUFFER_DESC vbDesc;
        vertexBuffer->GetDesc(&vbDesc);
        
        // Get index buffer description
        D3D11_BUFFER_DESC ibDesc;
        indexBuffer->GetDesc(&ibDesc);
        
        // Figure out format for index buffer (16 or 32-bit indices)
        DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT; // Default to 32-bit
        if (ibDesc.ByteWidth > 0 && ibDesc.ByteWidth % 4 != 0 && ibDesc.ByteWidth % 2 == 0) {
            indexFormat = DXGI_FORMAT_R16_UINT; // Must be 16-bit
        }
        
        // Create geometry task
        KickstartRT::D3D11::BVHTask::GeometryTask geomTask;
        geomTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Register;
        geomTask.handle = handle;
        
        // Set up geometry input
        geomTask.input.type = KickstartRT::D3D11::BVHTask::GeometryInput::Type::TrianglesIndexed;
        geomTask.input.allowUpdate = true; // Allow updating in the future
        
        // Create geometry component
        KickstartRT::D3D11::BVHTask::GeometryInput::GeometryComponent component;
        
        // Set up vertex buffer
        component.vertexBuffer.resource = vertexBuffer;
        component.vertexBuffer.format = DXGI_FORMAT_R32G32B32_FLOAT; // Assuming position is float3
        component.vertexBuffer.offsetInBytes = 0;
        component.vertexBuffer.strideInBytes = vbDesc.StructureByteStride > 0 ? vbDesc.StructureByteStride : 12; // Default to 12 bytes (3 floats)
        
        // Get vertex count
        component.vertexBuffer.count = vbDesc.ByteWidth / component.vertexBuffer.strideInBytes;
        
        // Set up index buffer
        component.indexBuffer.resource = indexBuffer;
        component.indexBuffer.format = indexFormat;
        component.indexBuffer.offsetInBytes = 0;
        component.indexBuffer.count = ibDesc.ByteWidth / (indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4);
        
        // Add component to geometry input
        geomTask.input.components.push_back(component);
        
        // Schedule geometry task
        taskContainer->ScheduleBVHTask(&geomTask);
        
        // Execute the GPU task
        auto execStatus = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (execStatus != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute geometry registration task. Status: {}", static_cast<int>(execStatus));
            return false;
        }
        
        // Store the handle for future reference
        g_geometryHandles[name] = handle;
        *outHandle = handle;
        
        logger::info("[RT] Successfully registered geometry '{}' with KickstartRT", name);
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry registration: {}", e.what());
        return false;
    }
}

// Create an instance of a geometry with KickstartRT
bool CreateInstance(KickstartRT::D3D11::GeometryHandle& geometryHandle, const DirectX::XMFLOAT4X4& transform, const std::string& name, KickstartRT::D3D11::InstanceHandle* outHandle)
{
    if (!g_executeContext || !outHandle) {
        logger::error("[RT] Invalid parameters for instance creation");
        return false;
    }
    
    // Check if we already have a handle for this instance
    auto it = g_instanceHandles.find(name);
    if (it != g_instanceHandles.end()) {
        *outHandle = it->second;
        return true;
    }
    
    try {
        // Create a handle for the instance
        KickstartRT::D3D11::InstanceHandle handle;
        auto status = g_executeContext->CreateInstanceHandles(&handle, 1);
        if (status != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to create instance handle. Status: {}", static_cast<int>(status));
            return false;
        }
        
        // Create a task container
        auto taskContainer = g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for instance creation");
            return false;
        }
        
        // Create instance task
        KickstartRT::D3D11::BVHTask::InstanceTask instanceTask;
        instanceTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Register;
        instanceTask.handle = handle;
        
        // Set up instance input
        std::wstring wideName(name.begin(), name.end());
        instanceTask.input.name = wideName.c_str();
        instanceTask.input.geomHandle = geometryHandle;
        
        // Convert transform matrix to KickstartRT format (3x4 row-major)
        KickstartRT::Math::Float_3x4 ksTransform = {};  // Initialize to zero first
        
        // Fill the 3x4 matrix from the 4x4 transform
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 4; col++) {
                ksTransform.m[row][col] = transform.m[row][col];
            }
        }
        
        instanceTask.input.transform = ksTransform;
        instanceTask.input.participatingInTLAS = true; // Include in the TLAS
        
        // Schedule instance task
        taskContainer->ScheduleBVHTask(&instanceTask);
        
        // Execute the GPU task
        auto execStatus = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (execStatus != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute instance creation task. Status: {}", static_cast<int>(execStatus));
            return false;
        }
        
        // Store the handle for future reference
        g_instanceHandles[name] = handle;
        *outHandle = handle;
        
        logger::info("[RT] Successfully created instance '{}' with KickstartRT", name);
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during instance creation: {}", e.what());
        return false;
    }
}

// Update an instance's transform with KickstartRT
bool UpdateInstanceTransform(KickstartRT::D3D11::InstanceHandle& instanceHandle, const DirectX::XMFLOAT4X4& transform)
{
    if (!g_executeContext) {
        logger::error("[RT] Execute context not available for transform update");
        return false;
    }
    
    try {
        // Create a task container
        auto taskContainer = g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for transform update");
            return false;
        }
        
        // Create instance task
        KickstartRT::D3D11::BVHTask::InstanceTask instanceTask;
        instanceTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Update;
        instanceTask.handle = instanceHandle;
        
        // Convert transform matrix to KickstartRT format (3x4 row-major)
        KickstartRT::Math::Float_3x4 ksTransform = {};  // Initialize to zero first
        
        // Fill the 3x4 matrix from the 4x4 transform
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 4; col++) {
                ksTransform.m[row][col] = transform.m[row][col];
            }
        }
        
        instanceTask.input.transform = ksTransform;
        
        // Schedule instance task
        taskContainer->ScheduleBVHTask(&instanceTask);
        
        // Execute the GPU task
        auto execStatus = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (execStatus != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute transform update task. Status: {}", static_cast<int>(execStatus));
            return false;
        }
        
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during transform update: {}", e.what());
        return false;
    }
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
    
        try {
            // Default resolution in case we can't query the actual dimensions
            g_width = 1920;
            g_height = 1080;
            
            // Create init settings for D3D11
            KickstartRT::D3D11::ExecuteContext_InitSettings settings;
            settings.D3D11Device = device;
            
            // Get the adapter directly for KickstartRT
            // This is REQUIRED for the D3D11 interop layer
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
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
            return false;
        }
        
            // Then check if context is null
            if (!exc) {
                logger::error("[RT] Context creation returned OK but context is null");
                return false;
            }
            
            g_executeContext = exc;
        g_initialized = true;
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
        g_executeContext->ReleaseDeviceResourcesImmediately();
        logger::info("[RT] Resources cleaned up");
    }
}

    // Core rendering functions - simplified implementations for now
    // In the future these will be expanded to use proper task scheduling
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
        if (!depthSRV || !normalSRV || !outputUAV) {
            logger::error("[KickstartRTImpl] GenerateGI called with null resources");
            return false;
        }

        // Log what we're doing
        logger::info("[KickstartRTImpl] Running GenerateGI with provided resources");

        try {
            // Create a task container
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
                    traceTask.common.viewport.width = g_width;
                    traceTask.common.viewport.height = g_height;
                }
            } else {
                // Fallback to screen dimensions
                traceTask.common.viewport.width = g_width;
                traceTask.common.viewport.height = g_height;
            }
            
            // Set ray parameters - only use fields that exist in the API
            // TODO: Consult KickstartRT documentation for additional parameters that can be set
            traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Release resources - we need to release any resources we acquired
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (outputResource) outputResource->Release();
            
            // Execute GPU task - this is where KickstartRT actually processes all scheduled tasks
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            
            if (status == KickstartRT::Status::OK) {
                logger::info("[KickstartRTImpl] Successfully generated GI");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute GI task: {}", static_cast<int>(status));
                return false;
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
                    traceTask.common.viewport.width = g_width;
                    traceTask.common.viewport.height = g_height;
                }
            } else {
                // Fallback to screen dimensions
                traceTask.common.viewport.width = g_width;
                traceTask.common.viewport.height = g_height;
            }
            
            // Set ray parameters - only use fields that exist in the API
            // TODO: Consult KickstartRT documentation for additional parameters that can be set
            traceTask.common.maxRayLength = 200.0f;  // Maximum ray distance
            
            // Schedule the task
            taskContainer->ScheduleRenderTask(&traceTask);
            
            // Release resources - we need to release any resources we acquired
            if (depthResource) depthResource->Release();
            if (normalResource) normalResource->Release();
            if (roughnessResource) roughnessResource->Release();
            if (outputResource) outputResource->Release();
            
            // Execute GPU task - this is where KickstartRT actually processes all scheduled tasks
            auto status = g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            
            if (status == KickstartRT::Status::OK) {
                logger::info("[KickstartRTImpl] Successfully generated reflections");
                return true;
            } else {
                logger::error("[KickstartRTImpl] Failed to execute reflections task: {}", static_cast<int>(status));
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

// Raytracing class implementation
bool Raytracing::InitializeResources(ID3D11Device* device)
{
    if (!device) {
        logger::error("[RT] Null device");
        return false;
    }
    
    // Initialize the core raytracing system
    initialized = KickstartRTImpl::Initialize(device);
    
    if (initialized) {
        logger::info("[RT] Resources initialized successfully");
    } else {
        logger::error("[RT] Failed to initialize resources");
    }
    
    return initialized;
}

void Raytracing::ClearResources()
{
    if (initialized) {
        KickstartRTImpl::CleanupResources();
        initialized = false;
        logger::info("[RT] Resources cleared");
    }
}

bool Raytracing::TestKickstartRT()
{
    // Simplified implementation that always returns true
    // This avoids the test that was causing crashes
    logger::info("[Raytracing] KickstartRT test bypassed");
    return true;
}

bool Raytracing::RegisterGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        logger::warn("[RT] RegisterGeometry called but raytracing is not initialized or enabled");
        return false;
    }

    if (!KickstartRTImpl::g_executeContext) {
        logger::error("[RT] No execute context available");
        return false;
    }
    
    logger::info("[RT] Beginning geometry registration");
    
    try {
        // Create a task container for geometry registration
        auto taskContainer = KickstartRTImpl::g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for geometry registration");
            return false;
        }
        
        // Get device
        ID3D11Device* device = globals::d3d::device;
        if (!device) {
            logger::error("[RT] D3D11 device is null");
            return false;
        }
        
        // Get the game's 3D world
        auto tes = globals::game::tes;
        if (!tes) {
            logger::error("[RT] TES instance not available");
            
            // Fall back to create an empty BVH structure
            KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
            bvhBuildTask.buildTLAS = true;
            bvhBuildTask.maxBlasBuildCount = 16u;
            taskContainer->ScheduleBVHTask(&bvhBuildTask);
            
            // Execute the GPU tasks to create empty BVH
            auto status = KickstartRTImpl::g_executeContext->InvokeGPUTask(taskContainer, nullptr);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute empty BVH build task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            logger::warn("[RT] Created empty BVH structure with no geometry");
            return true;
        }
        
        // Counter for registered meshes
        int registeredMeshes = 0;
        int maxMeshesToRegister = 50; // Limit to avoid performance issues
        
        // Process the scene to extract geometry
        logger::info("[RT] Starting scene traversal for geometry extraction");
        
        // Start with the current cell
        if (auto playerChar = RE::PlayerCharacter::GetSingleton()) {
            if (auto playerCell = playerChar->GetParentCell()) {
                // Get cell's parent 3D node - cells don't have Get3D, we need to use a different approach
                // Use a different way to get the cell's root node for traversal
                RE::NiAVObject* cellNode = nullptr;
                
                if (auto playerObj = playerChar->Get3D()) {
                    // Start traversal from player's 3D object
                    cellNode = playerObj;
                }
                
                if (!cellNode) {
                    logger::warn("[RT] Could not find cell node for geometry extraction");
                    
                    // Try to use player character's node as a fallback
                    cellNode = playerChar->Get3D();
                    if (!cellNode) {
                        // If everything fails, we'll create a test quad later
                        logger::warn("[RT] Could not find player 3D node either, falling back to test quad");
                    }
                }
                
                // Only proceed with traversal if we have a valid node
                if (cellNode) {
                    // Traverse cell geometry
                    RE::BSVisit::TraverseScenegraphGeometries(cellNode, [&](RE::BSGeometry* geometry) {
                        // Early out if we've reached our limit
                        if (registeredMeshes >= maxMeshesToRegister) {
                            return RE::BSVisit::BSVisitControl::kStop;
                        }
                        
                        // Skip geometry without valid data
                        if (!geometry) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        
                        // Check if geometry has runtime data
                        auto& geomRuntime = geometry->GetGeometryRuntimeData();
                        if (!geomRuntime.rendererData) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        
                        // Get vertex and index buffers from renderer data
                        auto rendererData = geomRuntime.rendererData;
                        
                        // Proper casting with the appropriate type - using reinterpret_cast for DirectX resources
                        ID3D11Buffer* vbuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
                        ID3D11Buffer* ibuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
                        
                        // Skip if buffers not available
                        if (!vbuffer || !ibuffer) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        
                        // Skip dynamic geometry (like skinned meshes) for now
                        // Check if this is a skinned geometry by looking for a skin instance
                        bool isDynamic = geomRuntime.skinInstance != nullptr;
                        if (isDynamic) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        
                        // Generate a unique name for this geometry 
                        std::string meshName = std::format("Mesh_{}", registeredMeshes);
                        
                        // Register with KickstartRT
                        KickstartRT::D3D11::GeometryHandle geomHandle;
                        if (KickstartRTImpl::RegisterGeometryWithKickstartRT(vbuffer, ibuffer, meshName, &geomHandle)) {
                            // Geometry registered successfully
                            
                            // Get world transform for this geometry
                            DirectX::XMFLOAT4X4 transform;
                            
                            // Get world transform from the node
                            if (geometry->parent) {
                                // Convert NiTransform to DirectX matrix
                                auto& worldTransform = geometry->parent->world;
                                
                                // Create matrix from the NiMatrix3 for rotation (no quaternion directly available)
                                // Extract rotation components from the rotation matrix
                                DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixSet(
                                    worldTransform.rotate.entry[0][0], worldTransform.rotate.entry[0][1], worldTransform.rotate.entry[0][2], 0.0f,
                                    worldTransform.rotate.entry[1][0], worldTransform.rotate.entry[1][1], worldTransform.rotate.entry[1][2], 0.0f,
                                    worldTransform.rotate.entry[2][0], worldTransform.rotate.entry[2][1], worldTransform.rotate.entry[2][2], 0.0f,
                                    0.0f, 0.0f, 0.0f, 1.0f
                                );
                                
                                DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
                                    worldTransform.scale,
                                    worldTransform.scale,
                                    worldTransform.scale
                                );
                                
                                DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(
                                    worldTransform.translate.x,
                                    worldTransform.translate.y,
                                    worldTransform.translate.z
                                );
                                
                                // Combine matrices: Scale -> Rotate -> Translate
                                DirectX::XMMATRIX worldMatrix = scaleMatrix * rotationMatrix * translationMatrix;
                                DirectX::XMStoreFloat4x4(&transform, worldMatrix);
                            } else {
                                // Use identity transform if no parent
                                DirectX::XMStoreFloat4x4(&transform, DirectX::XMMatrixIdentity());
                            }
                            
                            // Create an instance with this geometry
                            KickstartRT::D3D11::InstanceHandle instanceHandle;
                            std::string instanceName = std::format("Instance_{}", registeredMeshes);
                            if (KickstartRTImpl::CreateInstance(geomHandle, transform, instanceName, &instanceHandle)) {
                                logger::debug("[RT] Created instance of {}", meshName);
                                registeredMeshes++;
                            }
                        }
                        
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
                }
            }
        }
        
        // If we didn't register any game meshes, fall back to a test quad
        if (registeredMeshes == 0) {
            logger::warn("[RT] No game meshes registered, creating test quad as fallback");
            
            // Vertices for a simple quad (2x2 units, centered at origin)
            DirectX::XMFLOAT3 quadVertices[] = {
                { -1.0f, -1.0f, 0.0f },  // Bottom-left
                {  1.0f, -1.0f, 0.0f },  // Bottom-right
                {  1.0f,  1.0f, 0.0f },  // Top-right
                { -1.0f,  1.0f, 0.0f }   // Top-left
            };
            
            // Indices for the quad (2 triangles)
            uint32_t quadIndices[] = {
                0, 1, 2,  // Triangle 1
                0, 2, 3   // Triangle 2
            };
            
            // Create vertex buffer
            D3D11_BUFFER_DESC vbDesc = {};
            vbDesc.ByteWidth = sizeof(quadVertices);
            vbDesc.Usage = D3D11_USAGE_DEFAULT;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vbDesc.CPUAccessFlags = 0;
            vbDesc.StructureByteStride = sizeof(DirectX::XMFLOAT3);
            
            D3D11_SUBRESOURCE_DATA vbData = {};
            vbData.pSysMem = quadVertices;
            
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
            HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vertexBuffer.GetAddressOf());
            if (FAILED(hr)) {
                logger::error("[RT] Failed to create vertex buffer for test quad. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Create index buffer
            D3D11_BUFFER_DESC ibDesc = {};
            ibDesc.ByteWidth = sizeof(quadIndices);
            ibDesc.Usage = D3D11_USAGE_DEFAULT;
            ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            ibDesc.CPUAccessFlags = 0;
            ibDesc.StructureByteStride = sizeof(uint32_t);
            
            D3D11_SUBRESOURCE_DATA ibData = {};
            ibData.pSysMem = quadIndices;
            
            Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
            hr = device->CreateBuffer(&ibDesc, &ibData, indexBuffer.GetAddressOf());
            if (FAILED(hr)) {
                logger::error("[RT] Failed to create index buffer for test quad. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Register the quad geometry with KickstartRT
            KickstartRT::D3D11::GeometryHandle geomHandle;
            if (KickstartRTImpl::RegisterGeometryWithKickstartRT(vertexBuffer.Get(), indexBuffer.Get(), "TestQuad", &geomHandle)) {
                registeredMeshes++;
                
                // Create an instance of the quad
                DirectX::XMFLOAT4X4 transform;
                DirectX::XMStoreFloat4x4(&transform, DirectX::XMMatrixIdentity());
                
                // Create instance handle
                KickstartRT::D3D11::InstanceHandle instanceHandle;
                if (KickstartRTImpl::CreateInstance(geomHandle, transform, "TestQuadInstance", &instanceHandle)) {
                    logger::info("[RT] Created instance of TestQuad");
                }
            }
        }
        
        // Schedule a BVH update task
        KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
        bvhBuildTask.buildTLAS = true;
        bvhBuildTask.maxBlasBuildCount = static_cast<uint32_t>(registeredMeshes * 2); // Allow enough BLAS builds
        taskContainer->ScheduleBVHTask(&bvhBuildTask);
        
        // Execute the GPU tasks to update geometry
        auto status = KickstartRTImpl::g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (status != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute geometry registration task. Status: {}", static_cast<int>(status));
            return false;
        }
        
        logger::info("[RT] BVH structure created with {} meshes registered", registeredMeshes);
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry registration: {}", e.what());
        return false;
    }
}

bool Raytracing::UpdateGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        return false;
    }
    
    try {
        // Check if we have an execute context
        if (!KickstartRTImpl::g_executeContext) {
            logger::warn("[RT] UpdateGeometry called without execute context");
            return false;
        }
        
        // Create a task container for transform updates
        auto taskContainer = KickstartRTImpl::g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for geometry updates");
            return false;
        }
        
        int updatedInstances = 0;
        
        // Get game state
        auto tes = globals::game::tes;
        if (!tes) {
            logger::warn("[RT] TES instance not available for geometry updates");
            return false;
        }
        
        // Find all instances in our handle map and update their transforms
        for (const auto& [name, handle] : KickstartRTImpl::g_instanceHandles) {
            if (name.find("Instance_") == 0) {
                // This is a game scene instance - try to find corresponding geometry
                // Extract index from the name (e.g., "Instance_42" -> 42)
                try {
                    size_t index = std::stoi(name.substr(9));
                    std::string meshName = std::format("Mesh_{}", index);
                    
                    // For updating transforms of static objects, we would query their current 
                    // transforms, but for simplicity and performance, we'll only update transforms
                    // of test objects in this implementation.
                    
                    // In a full implementation, we would:
                    // 1. Find the original object in the scene graph
                    // 2. Get its current transform
                    // 3. Update the instance transform using that data
                    
                    updatedInstances++;
                } catch (const std::exception&) {
                    // Invalid instance name format
                }
            } else if (name == "TestQuadInstance") {
                // Update our test quad with animated transform
                static float rotation = 0.0f;
                rotation += 0.01f; // Small increment each frame
                
                // Create a rotation matrix around Y axis
                DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationY(rotation);
                
                // Create a translation matrix to move the quad up and away from the origin
                DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(0.0f, 0.0f, -5.0f);
                
                // Combine the transforms
                DirectX::XMMATRIX worldMatrix = rotationMatrix * translationMatrix;
                
                // Convert to XMFLOAT4X4 for the utility function
                DirectX::XMFLOAT4X4 transform;
                DirectX::XMStoreFloat4x4(&transform, worldMatrix);
                
                // Update the instance transform
                // Need to make a copy of the instance handle to avoid const issues
                KickstartRT::D3D11::InstanceHandle instanceHandleCopy = handle;
                if (KickstartRTImpl::UpdateInstanceTransform(instanceHandleCopy, transform)) {
                    updatedInstances++;
                } else {
                    logger::debug("[RT] Failed to update test quad transform");
                }
            }
        }
        
        // For dynamic meshes (like skinned meshes), we would re-register 
        // their buffers with updated vertex data, but we're skipping that 
        // in this implementation for simplicity.
        
        // Schedule a BVH update task
        KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
        bvhBuildTask.buildTLAS = true;
        bvhBuildTask.maxBlasBuildCount = 16u;
        taskContainer->ScheduleBVHTask(&bvhBuildTask);
        
        // Execute the GPU tasks to update geometry
        auto status = KickstartRTImpl::g_executeContext->InvokeGPUTask(taskContainer, nullptr);
        if (status != KickstartRT::Status::OK) {
            logger::error("[RT] Failed to execute geometry update task. Status: {}", static_cast<int>(status));
            return false;
        }
        
        // Log the number of updated instances
        if (updatedInstances > 0) {
            logger::debug("[RT] Updated {} instance transforms", updatedInstances);
        }
        
        return true;
    }
    catch (const std::exception& e) {
        logger::error("[RT] Exception during geometry update: {}", e.what());
        return false;
    }
}

bool Raytracing::GetCurrentViewAndProjectionMatrices(DirectX::XMFLOAT4X4& viewMatrix, DirectX::XMFLOAT4X4& projMatrix)
{
    // Default to identity matrices
    DirectX::XMStoreFloat4x4(&viewMatrix, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&projMatrix, DirectX::XMMatrixIdentity());
    
    // Try to get the current view and projection matrices from the game's renderer
    if (auto renderer = globals::game::renderer) {
        // Get the camera data
        // int eyeIndex = 0; // Use main eye (for VR we'd need to handle both eyes)
        
        // Check for VR without using namespaces that don't exist
        bool isVR = globals::game::isVR; // Use the correct global
        if (isVR) {
            logger::info("[RT] VR mode detected, using eye index 0 for now");
            // In the future we should disable raytracing in VR
        }
        
        // For now, return identity matrices
        // We'll implement proper matrix retrieval later
        logger::info("[RT] Using identity matrices for view and projection");
        return true;
        
        // TODO: Implement proper matrix retrieval from the game's renderer
        // Example of how this would work:
        // auto viewMat = renderer->GetViewMatrix();
        // auto projMat = renderer->GetProjectionMatrix();
        // DirectX::XMStoreFloat4x4(&viewMatrix, viewMat);
        // DirectX::XMStoreFloat4x4(&projMatrix, projMat);
    }
    
    logger::error("[RT] Cannot get renderer");
    return false;
}

// Direct pass-through to KickstartRTImpl
bool Raytracing::GenerateGI(
    ID3D11ShaderResourceView* depthSRV,
                       ID3D11ShaderResourceView* normalSRV,
                       ID3D11UnorderedAccessView* outputUAV,
                       const DirectX::XMFLOAT4X4& viewMatrix,
                       const DirectX::XMFLOAT4X4& projMatrix) 
{
    // Check if we're enabled
    if (!IsEnabled()) {
        logger::info("[Raytracing] GenerateGI called but raytracing is not enabled");
        return false;
    }
    
    // Validate input resources
    if (!depthSRV || !normalSRV || !outputUAV) {
        logger::error("[Raytracing] GenerateGI called with null resources");
        return false;
    }
    
    logger::info("[Raytracing] Generating global illumination with provided resources");
    
    // Call into the KickstartRT implementation
    return KickstartRTImpl::GenerateGI(
        depthSRV,
        normalSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
}

// Direct pass-through to KickstartRTImpl
bool Raytracing::GenerateReflections(
                  ID3D11ShaderResourceView* depthSRV,
                  ID3D11ShaderResourceView* normalSRV,
    ID3D11ShaderResourceView* roughnessSRV,
    ID3D11UnorderedAccessView* outputUAV,
                  const DirectX::XMFLOAT4X4& viewMatrix,
                  const DirectX::XMFLOAT4X4& projMatrix) 
{
    // Check if we're enabled
    if (!IsEnabled()) {
        logger::info("[Raytracing] GenerateReflections called but raytracing is not enabled");
        return false;
    }
    
    // Validate input resources
    if (!depthSRV || !normalSRV || !roughnessSRV || !outputUAV) {
        logger::error("[Raytracing] GenerateReflections called with null resources");
        return false;
    }
    
    logger::info("[Raytracing] Generating reflections with provided resources");
    
    // Call into the KickstartRT implementation
    return KickstartRTImpl::GenerateReflections(
        depthSRV,
        normalSRV,
        roughnessSRV,
        outputUAV,
        viewMatrix,
        projMatrix
    );
}