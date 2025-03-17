#include "KickstartRTImpl.h"
#include "Globals.h"

namespace KickstartRTImpl
{
    namespace
    {
        /**
         * Helper function to execute a task container with proper fence synchronization
         * 
         * @param taskContainer The task container to execute
         * @param geometryTaskFirst Whether geometry tasks should be processed first
         * @param maxBlasBuildCount Maximum number of BLAS builds to perform
         * @return True if task execution was successful, false otherwise
         */
        bool ExecuteTaskContainer(
            KickstartRT::D3D11::TaskContainer* taskContainer,
            bool geometryTaskFirst = true,
            uint32_t maxBlasBuildCount = 16u)
        {
            if (!g_executeContext || !taskContainer) {
                logger::error("[RT] Cannot execute task container: invalid context or container");
                return false;
            }
            
            KickstartRT::D3D11::BuildGPUTaskInput taskInput = {};
            taskInput.geometryTaskFirst = geometryTaskFirst;
            taskInput.maxBlasBuildCount = maxBlasBuildCount;
            
            // Configure fence synchronization
            if (!g_renderFence) {
                logger::error("[RT] Cannot execute GPU task without a valid fence for synchronization");
                return false;
            }
            
            // Set both wait and signal fences
            taskInput.waitFence = g_renderFence.Get();
            taskInput.waitFenceValue = g_fenceValue;
            taskInput.signalFence = g_renderFence.Get();
            taskInput.signalFenceValue = g_fenceValue + 1;
            g_fenceValue++; // Increment for next use
            
            // Execute the GPU task
            auto status = g_executeContext->InvokeGPUTask(taskContainer, &taskInput);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to execute task. Status: {}", static_cast<int>(status));
                return false;
            }
            
            return true;
        }
    }
    
    /**
     * Register geometry with KickstartRT
     * 
     * This function takes vertex and index buffers and registers them with KickstartRT's
     * raytracing acceleration structure system.
     * 
     * @param vertexBuffer The buffer containing vertex positions
     * @param indexBuffer The buffer containing triangle indices
     * @param name Name identifier for the geometry
     * @param outHandle Pointer to receive the geometry handle
     * @return True if registration was successful, false otherwise
     */
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
                logger::error("[RT] Failed to create task container");
                return false;
            }
            
            // Get buffer descriptions
            D3D11_BUFFER_DESC vbDesc, ibDesc;
            vertexBuffer->GetDesc(&vbDesc);
            indexBuffer->GetDesc(&ibDesc);
            
            // Determine index format
            DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
            if (ibDesc.ByteWidth > 0 && ibDesc.ByteWidth % 4 != 0 && ibDesc.ByteWidth % 2 == 0) {
                indexFormat = DXGI_FORMAT_R16_UINT;
            }
            
            // Setup geometry task
            KickstartRT::D3D11::BVHTask::GeometryTask geomTask;
            geomTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Register;
            geomTask.handle = handle;
            geomTask.input.type = KickstartRT::D3D11::BVHTask::GeometryInput::Type::TrianglesIndexed;
            geomTask.input.allowUpdate = true;
            
            // Add geometry component
            KickstartRT::D3D11::BVHTask::GeometryInput::GeometryComponent component;
            
            // Configure vertex buffer
            component.vertexBuffer.resource = vertexBuffer;
            component.vertexBuffer.format = DXGI_FORMAT_R32G32B32_FLOAT;
            component.vertexBuffer.offsetInBytes = 0;
            component.vertexBuffer.strideInBytes = vbDesc.StructureByteStride > 0 ? 
                vbDesc.StructureByteStride : 12;
            component.vertexBuffer.count = vbDesc.ByteWidth / component.vertexBuffer.strideInBytes;
            
            // Configure index buffer
            component.indexBuffer.resource = indexBuffer;
            component.indexBuffer.format = indexFormat;
            component.indexBuffer.offsetInBytes = 0;
            component.indexBuffer.count = ibDesc.ByteWidth / (indexFormat == DXGI_FORMAT_R16_UINT ? 2 : 4);
            
            // Add component to geometry task
            geomTask.input.components.push_back(component);
            
            // Schedule the task
            taskContainer->ScheduleBVHTask(&geomTask);
            
            // Execute the task container
            if (!ExecuteTaskContainer(taskContainer)) {
                return false;
            }
            
            // Store the handle and return it
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

    /**
     * Create an instance of a geometry in the scene
     * 
     * This function takes a registered geometry and creates an instance of it with a 
     * specific transformation matrix.
     * 
     * @param geometryHandle Handle to the registered geometry
     * @param transform Transformation matrix (position, rotation, scale)
     * @param name Name identifier for the instance
     * @param outHandle Pointer to receive the instance handle
     * @return True if instance creation was successful, false otherwise
     */
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
            // Create instance handle
            KickstartRT::D3D11::InstanceHandle handle;
            auto status = g_executeContext->CreateInstanceHandles(&handle, 1);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to create instance handle. Status: {}", static_cast<int>(status));
                return false;
            }
            
            // Create task container
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container");
                return false;
            }
            
            // Create instance task
            KickstartRT::D3D11::BVHTask::InstanceTask instanceTask;
            instanceTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Register;
            instanceTask.handle = handle;
            
            // Set up instance data
            std::wstring wideName(name.begin(), name.end());
            instanceTask.input.name = wideName.c_str();
            instanceTask.input.geomHandle = geometryHandle;
            instanceTask.input.participatingInTLAS = true;
            
            // Convert transform matrix
            KickstartRT::Math::Float_3x4 ksTransform = {};
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 4; col++) {
                    ksTransform.m[row][col] = transform.m[row][col];
                }
            }
            instanceTask.input.transform = ksTransform;
            
            // Schedule task
            taskContainer->ScheduleBVHTask(&instanceTask);
            
            // Execute task container
            if (!ExecuteTaskContainer(taskContainer)) {
                return false;
            }
            
            // Store and return handle
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

    /**
     * Update an instance's transform 
     * 
     * This updates the transformation matrix of an existing instance, used for moving objects.
     * 
     * @param instanceHandle Handle to the instance to update
     * @param transform New transformation matrix
     * @return True if update was successful, false otherwise
     */
    bool UpdateInstanceTransform(KickstartRT::D3D11::InstanceHandle& instanceHandle, const DirectX::XMFLOAT4X4& transform)
    {
        if (!g_executeContext) {
            logger::error("[RT] Execute context not available for transform update");
            return false;
        }
        
        try {
            // Create task container
            auto taskContainer = g_executeContext->CreateTaskContainer();
            if (!taskContainer) {
                logger::error("[RT] Failed to create task container");
                return false;
            }
            
            // Create instance task
            KickstartRT::D3D11::BVHTask::InstanceTask instanceTask;
            instanceTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Update;
            instanceTask.handle = instanceHandle;
            
            // Convert transform matrix
            KickstartRT::Math::Float_3x4 ksTransform = {};
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 4; col++) {
                    ksTransform.m[row][col] = transform.m[row][col];
                }
            }
            instanceTask.input.transform = ksTransform;
            
            // Schedule task
            taskContainer->ScheduleBVHTask(&instanceTask);
            
            // Execute task container
            return ExecuteTaskContainer(taskContainer);
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during transform update: {}", e.what());
            return false;
        }
    }
    
    /**
     * Register a Skyrim mesh for raytracing
     * 
     * Takes a Skyrim BSTriShape and registers it with KickstartRT.
     * 
     * @param triShape Skyrim mesh to register
     * @param name Name identifier for the geometry
     * @param outHandle Pointer to receive the geometry handle
     * @return True if registration was successful, false otherwise
     */
    bool RegisterSkyrimMesh(RE::BSTriShape* triShape, const std::string& name, KickstartRT::D3D11::GeometryHandle* outHandle)
    {
        if (!g_executeContext || !triShape || !outHandle) {
            logger::error("[RT] Invalid parameters for Skyrim mesh registration");
            return false;
        }
        
        try {
            // Extract the DirectX vertex and index buffers from BSTriShape
            // This would need to be implemented based on how Skyrim's rendering system works
            // Placeholder implementation - these would need to be obtained from BSTriShape
            ID3D11Buffer* vertexBuffer = nullptr; // Get from triShape
            ID3D11Buffer* indexBuffer = nullptr;  // Get from triShape
            
            // Then use the regular registration function
            if (vertexBuffer && indexBuffer) {
                return RegisterGeometryWithKickstartRT(vertexBuffer, indexBuffer, name, outHandle);
            }
            else {
                logger::error("[RT] Failed to extract vertex/index buffers from BSTriShape");
                return false;
            }
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during Skyrim mesh registration: {}", e.what());
            return false;
        }
        
        return false;
    }
    
    /**
     * Collect and register visible geometry from the Skyrim scene
     * 
     * This uses Skyrim's scene traversal to gather meshes for raytracing.
     * 
     * @return The number of geometries successfully registered
     */
    int CollectSceneGeometry()
    {
        // This function would traverse Skyrim's scene graph and collect visible geometry
        // It would use RE::BSVisit::TraverseScenegraphGeometries to access all BSGeometry objects
        
        // Placeholder implementation
        int registeredCount = 0;
        
        // Example of how to traverse the scene:
        /*
        auto sceneRoot = RE::TES::GetSingleton()->GetRootNode();
        if (sceneRoot) {
            RE::BSVisit::TraverseScenegraphGeometries(sceneRoot, [&](RE::BSGeometry* geometry) {
                // Filter geometry based on visibility, distance, etc.
                
                // Cast to BSTriShape when possible
                if (auto triShape = geometry->AsTriShape()) {
                    std::string name = "SceneObj_" + std::to_string(registeredCount);
                    
                    KickstartRT::D3D11::GeometryHandle handle;
                    if (RegisterSkyrimMesh(triShape, name, &handle)) {
                        registeredCount++;
                        
                        // Create an instance with the object's world transform
                        DirectX::XMFLOAT4X4 worldTransform;
                        // Convert from NiMatrix to XMFLOAT4X4
                        
                        KickstartRT::D3D11::InstanceHandle instanceHandle;
                        CreateInstance(handle, worldTransform, name + "_Instance", &instanceHandle);
                    }
                }
                
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        */
        
        return registeredCount;
    }
} // namespace KickstartRTImpl 