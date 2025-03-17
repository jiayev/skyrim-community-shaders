#include "KickstartRTImpl.h"
#include "Globals.h"

namespace KickstartRTImpl
{
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
            KickstartRT::D3D11::BuildGPUTaskInput taskInput1 = {};
            taskInput1.geometryTaskFirst = true;
            taskInput1.maxBlasBuildCount = 16u;
            
            // Add synchronization fences if available
            if (g_renderFence) {
                taskInput1.signalFence = g_renderFence.Get();
                taskInput1.signalFenceValue = g_fenceValue++;
            }
            
            auto execStatus1 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput1);
            if (execStatus1 != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute geometry registration task. Status: {}", static_cast<int>(execStatus1));
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
            KickstartRT::D3D11::BuildGPUTaskInput taskInput2 = {};
            taskInput2.geometryTaskFirst = true;
            taskInput2.maxBlasBuildCount = 16u;
            
            // Add synchronization fences if available
            if (g_renderFence) {
                taskInput2.signalFence = g_renderFence.Get();
                taskInput2.signalFenceValue = g_fenceValue++;
            }
            
            auto execStatus2 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput2);
            if (execStatus2 != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute instance creation task. Status: {}", static_cast<int>(execStatus2));
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
            KickstartRT::D3D11::BuildGPUTaskInput taskInput3 = {};
            taskInput3.geometryTaskFirst = true;
            taskInput3.maxBlasBuildCount = 16u;
            
            // Add synchronization fences if available
            if (g_renderFence) {
                taskInput3.signalFence = g_renderFence.Get();
                taskInput3.signalFenceValue = g_fenceValue++;
            }
            
            auto execStatus3 = g_executeContext->InvokeGPUTask(taskContainer, &taskInput3);
            if (execStatus3 != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute transform update task. Status: {}", static_cast<int>(execStatus3));
                return false;
            }
            
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during transform update: {}", e.what());
            return false;
        }
    }
} // namespace KickstartRTImpl 