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
        
        // Check if we already have a handle for this geometry
        auto it = g_geometryHandles.find(name);
        if (it != g_geometryHandles.end()) {
            *outHandle = it->second;
            return true;
        }
        
        try {
            // Get the runtime data from the BSTriShape
            auto& runtimeData = triShape->GetTrishapeRuntimeData();
            auto& geometryData = triShape->GetGeometryRuntimeData();
            
            // Need the renderer data for vertex/index buffers
            auto rendererData = geometryData.rendererData;
            if (!rendererData) {
                logger::error("[RT] Failed to get renderer data from BSTriShape");
                return false;
            }
            
            // Get vertex and index counts
            uint32_t vertexCount = runtimeData.vertexCount;
            uint32_t triangleCount = runtimeData.triangleCount;
            
            if (vertexCount == 0 || triangleCount == 0) {
                logger::warn("[RT] BSTriShape has no vertices or triangles, skipping");
                return false;
            }
            
            // Create D3D11 vertex buffer
            D3D11_BUFFER_DESC vbDesc = {};
            vbDesc.ByteWidth = rendererData->vertexDesc.GetSize() * vertexCount;
            vbDesc.Usage = D3D11_USAGE_DEFAULT;
            vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vbDesc.CPUAccessFlags = 0;
            vbDesc.StructureByteStride = rendererData->vertexDesc.GetSize();
            vbDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Required for KickstartRT
            
            D3D11_SUBRESOURCE_DATA vbData = {};
            vbData.pSysMem = rendererData->rawVertexData;
            
            // Create index buffer description
            D3D11_BUFFER_DESC ibDesc = {};
            ibDesc.ByteWidth = sizeof(uint16_t) * triangleCount * 3; // 3 indices per triangle
            ibDesc.Usage = D3D11_USAGE_DEFAULT;
            ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            ibDesc.CPUAccessFlags = 0;
            ibDesc.StructureByteStride = sizeof(uint16_t);
            ibDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Required for KickstartRT
            
            D3D11_SUBRESOURCE_DATA ibData = {};
            ibData.pSysMem = rendererData->rawIndexData;
            
            // Get D3D11 device
            ID3D11Device* device = globals::d3d::device;
            if (!device) {
                logger::error("[RT] Failed to get D3D11 device");
                return false;
            }
            
            // Create the vertex buffer
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
            HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vertexBuffer.GetAddressOf());
            if (FAILED(hr)) {
                logger::error("[RT] Failed to create vertex buffer for Skyrim mesh. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            // Create the index buffer
            Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
            hr = device->CreateBuffer(&ibDesc, &ibData, indexBuffer.GetAddressOf());
            if (FAILED(hr)) {
                logger::error("[RT] Failed to create index buffer for Skyrim mesh. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                return false;
            }
            
            logger::info("[RT] Created vertex and index buffers for Skyrim mesh '{}'", name);
            
            // Register with KickstartRT using our existing function
            return RegisterGeometryWithKickstartRT(vertexBuffer.Get(), indexBuffer.Get(), name, outHandle);
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
     * @param updateDynamicOnly Whether to only update dynamic objects
     * @return The number of geometries successfully registered
     */
    int CollectSceneGeometry(bool updateDynamicOnly)
    {
        // Track registered geometries
        int registeredCount = 0;
        int processedCount = 0;
        
        // Get the root node of the scene using the correct method
        auto worldRoot = RE::Main::WorldRootNode();
        if (!worldRoot) {
            logger::error("[RT] Failed to get world root node");
            return 0;
        }
        
        logger::debug("[RT] Starting scene geometry collection...");
        
        // Get player position for distance-based culling
        RE::NiPoint3 playerPosition;
        float maxDistance = 2048.0f; // Reasonable culling distance
        
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            playerPosition = player->GetPosition();
        }
        
        // Process only a limited number of objects per frame
        const int MAX_PROCESSED_PER_FRAME = 1;
        const int MAX_REGISTERED_TOTAL = 1; // Lower limit for dynamic updates
        
        // Create a timestamp string to uniquely identify this collection pass
        static uint32_t collectionCounter = 0;
        std::string timeStamp = std::to_string(++collectionCounter) + "_" + 
                               (updateDynamicOnly ? "dyn" : "full");
        
        // Keep track of what's been processed this frame
        static std::unordered_set<RE::BSGeometry*> processedThisFrame;
        processedThisFrame.clear();
        
        // Process actors (dynamic objects) first if requested
        if (updateDynamicOnly) {
            auto processManager = RE::ProcessLists::GetSingleton();
            if (processManager) {
                // Process high priority actors (closer to player)
                for (auto& actorHandle : processManager->highActorHandles) {
                    if (auto actor = actorHandle.get()) {
                        if (processedCount >= MAX_PROCESSED_PER_FRAME) break;
                        
                        // Check if the actor is close enough to be worth processing
                        float distance = (actor->GetPosition() - playerPosition).Length();
                        if (distance > maxDistance * 0.5f) continue; // Use a tighter constraint for actors
                        
                        // Get the actor's 3D
                        if (auto actorRoot = actor->Get3D()) {
                            // Process this actor's geometry
                            RE::BSVisit::TraverseScenegraphGeometries(actorRoot, [&](RE::BSGeometry* geometry) {
                                // Skip if we've already processed this geometry
                                if (processedThisFrame.count(geometry) > 0) {
                                    return RE::BSVisit::BSVisitControl::kContinue;
                                }
                                
                                processedThisFrame.insert(geometry);
                                processedCount++;
                                
                                // Skip invalid or small geometries
                                if (!geometry || geometry->worldBound.radius < 5.0f) {
                                    return RE::BSVisit::BSVisitControl::kContinue;
                                }
                                
                                // Convert to BSTriShape if possible
                                auto triShape = geometry->AsTriShape();
                                if (!triShape) {
                                    return RE::BSVisit::BSVisitControl::kContinue;
                                }
                                
                                // Create a unique name for this geometry 
                                // Include actor ID for better identification
                                std::string name = "Actor_" + std::to_string(actor->formID) + "_" + 
                                                 std::to_string(registeredCount) + "_" + timeStamp;
                                
                                // The rest of processing is the same...
                                KickstartRT::D3D11::GeometryHandle handle;
                                if (RegisterSkyrimMesh(triShape, name, &handle)) {
                                    registeredCount++;
                                    
                                    // Get the world transform from the geometry
                                    DirectX::XMFLOAT4X4 worldTransform;
                                    
                                    // Access transform directly from NiAVObject's world transform
                                    const RE::NiTransform& transform = geometry->world;
                                    
                                    // NiMatrix3 to upper 3x3 of XMFLOAT4X4
                                    worldTransform._11 = transform.rotate.entry[0][0];
                                    worldTransform._12 = transform.rotate.entry[0][1];
                                    worldTransform._13 = transform.rotate.entry[0][2];
                                    worldTransform._14 = 0.0f;
                                    
                                    worldTransform._21 = transform.rotate.entry[1][0];
                                    worldTransform._22 = transform.rotate.entry[1][1];
                                    worldTransform._23 = transform.rotate.entry[1][2];
                                    worldTransform._24 = 0.0f;
                                    
                                    worldTransform._31 = transform.rotate.entry[2][0];
                                    worldTransform._32 = transform.rotate.entry[2][1];
                                    worldTransform._33 = transform.rotate.entry[2][2];
                                    worldTransform._34 = 0.0f;
                                    
                                    // Position to translation component (last row)
                                    worldTransform._41 = transform.translate.x;
                                    worldTransform._42 = transform.translate.y;
                                    worldTransform._43 = transform.translate.z;
                                    worldTransform._44 = 1.0f;
                                    
                                    // Create an instance with the world transform
                                    KickstartRT::D3D11::InstanceHandle instanceHandle;
                                    if (CreateInstance(handle, worldTransform, name + "_Instance", &instanceHandle)) {
                                        // Reduced logging to debug only
                                        logger::debug("[RT] Created instance for dynamic actor geometry: {}", name);
                                    }
                                    
                                    // Check if we've hit our limit
                                    if (registeredCount >= MAX_REGISTERED_TOTAL) {
                                        return RE::BSVisit::BSVisitControl::kStop;
                                    }
                                }
                                
                                return RE::BSVisit::BSVisitControl::kContinue;
                            });
                        }
                    }
                }
            }
            
            // Log only in debug mode
            logger::debug("[RT] Dynamic object collection complete. Registered {} objects (processed {})", 
                registeredCount, processedCount);
            return registeredCount;
        }
        
        // For full updates, process the entire scene graph as before
        // Use BSVisit to traverse the scene graph
        RE::BSVisit::TraverseScenegraphGeometries(reinterpret_cast<RE::NiAVObject*>(worldRoot), [&](RE::BSGeometry* geometry) {
            // Skip if we've already processed this geometry
            if (processedThisFrame.count(geometry) > 0) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            
            processedThisFrame.insert(geometry);
            
            // Skip invalid geometries
            if (!geometry) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            
            // Early exit if we're hitting our per-frame processing limit
            processedCount++;
            if (processedCount > MAX_PROCESSED_PER_FRAME) {
                logger::debug("[RT] Reached per-frame processing limit ({}), stopping collection", MAX_PROCESSED_PER_FRAME);
                return RE::BSVisit::BSVisitControl::kStop;
            }
            
            // Filter out small geometries
            if (geometry->worldBound.radius < 10.0f) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            
            // Distance-based culling
            float distance = (geometry->world.translate - playerPosition).Length();
            if (distance > maxDistance) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            
            // Convert to BSTriShape if possible
            auto triShape = geometry->AsTriShape();
            if (!triShape) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }
            
            // Create a unique name for this geometry
            std::string name = "SceneObj_" + std::to_string(registeredCount) + "_" + timeStamp;
            
            // Register the mesh
            KickstartRT::D3D11::GeometryHandle handle;
            if (RegisterSkyrimMesh(triShape, name, &handle)) {
                registeredCount++;
                
                // Get the world transform from the geometry
                DirectX::XMFLOAT4X4 worldTransform;
                
                // Access transform directly from NiAVObject's world member
                const RE::NiTransform& transform = geometry->world;
                
                // NiMatrix3 to upper 3x3 of XMFLOAT4X4
                worldTransform._11 = transform.rotate.entry[0][0];
                worldTransform._12 = transform.rotate.entry[0][1];
                worldTransform._13 = transform.rotate.entry[0][2];
                worldTransform._14 = 0.0f;
                
                worldTransform._21 = transform.rotate.entry[1][0];
                worldTransform._22 = transform.rotate.entry[1][1];
                worldTransform._23 = transform.rotate.entry[1][2];
                worldTransform._24 = 0.0f;
                
                worldTransform._31 = transform.rotate.entry[2][0];
                worldTransform._32 = transform.rotate.entry[2][1];
                worldTransform._33 = transform.rotate.entry[2][2];
                worldTransform._34 = 0.0f;
                
                // Position to translation component (last row)
                worldTransform._41 = transform.translate.x;
                worldTransform._42 = transform.translate.y;
                worldTransform._43 = transform.translate.z;
                worldTransform._44 = 1.0f;
                
                // Create an instance with the world transform
                KickstartRT::D3D11::InstanceHandle instanceHandle;
                if (CreateInstance(handle, worldTransform, name + "_Instance", &instanceHandle)) {
                    // Reduced logging to debug only for better performance
                    logger::debug("[RT] Created instance for geometry: {}", name);
                }
                
                // Limit to a reasonable number of objects for performance
                if (registeredCount >= MAX_REGISTERED_TOTAL) {
                    logger::debug("[RT] Reached geometry limit ({}), stopping collection", MAX_REGISTERED_TOTAL);
                    return RE::BSVisit::BSVisitControl::kStop;
                }
            }
            
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        
        // Reduced logging to info level (not spamming every frame)
        logger::info("[RT] Scene geometry collection complete. Registered {} objects (processed {})", 
            registeredCount, processedCount);
        return registeredCount;
    }
    
    /**
     * Overloaded version that defaults to full updates
     */
    int CollectSceneGeometry()
    {
        return CollectSceneGeometry(false);
    }
} // namespace KickstartRTImpl 