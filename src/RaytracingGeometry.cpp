#include "KickstartRTImpl.h"
#include "Globals.h"
#include "State.h"

namespace KickstartRTImpl
{
    namespace
    {
        // Resource cache to prevent recreating the same buffers repeatedly
        struct BufferCacheEntry {
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            size_t hash;
            uint64_t lastUsedFrame;
        };
        
        // Forward declaration of function defined outside the anonymous namespace
        void DestroyUnusedHandles(uint64_t currentFrame);
        
        // Maximum number of buffers to cache
        constexpr size_t MAX_BUFFER_CACHE_SIZE = 1024;

        std::vector<BufferCacheEntry> g_vertexBufferCache;
        std::vector<BufferCacheEntry> g_indexBufferCache;
        // Limit cache size to prevent memory growth
        
        // Add a static counter as a replacement for frameCount
        static uint64_t g_frameCounter = 0;
        
        // Function to get and increment the frame counter
        uint64_t GetFrameCount() {
            return g_frameCounter++;
        }
        
        /**
         * Try to find a cached buffer that matches the given data
         * 
         * @param cache The buffer cache to search
         * @param size Size of the data in bytes
         * @param data Pointer to the data
         * @return Cached buffer if found, nullptr otherwise
         */
        ID3D11Buffer* FindCachedBuffer(std::vector<BufferCacheEntry>& cache, size_t size, const void* data) {
            // Simple hash function for buffer data
            size_t hash = 0;
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            // Only hash a subset of bytes to avoid performance issues with large buffers
            const size_t sampleSize = std::min(size, size_t(1024));
            const size_t stride = size / sampleSize;
            
            for (size_t i = 0; i < sampleSize; i++) {
                hash = hash * 31 + bytes[i * stride];
            }
            
            // Add buffer size to hash to differentiate same content with different sizes
            hash = hash * 31 + size;
            
            // Find matching buffer in cache
            for (auto& entry : cache) {
                if (entry.hash == hash) {
                    // Update last used frame
                    entry.lastUsedFrame = GetFrameCount();
                    return entry.buffer.Get();
                }
            }
            
            return nullptr;
        }
        
        /**
         * Add a buffer to the cache
         * 
         * @param cache The buffer cache to add to
         * @param buffer The buffer to add
         * @param size Size of the data in bytes
         * @param data Pointer to the data
         */
        void AddBufferToCache(std::vector<BufferCacheEntry>& cache, ID3D11Buffer* buffer, size_t size, const void* data) {
            // Simple hash function for buffer data
            size_t hash = 0;
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            // Only hash a subset of bytes to avoid performance issues with large buffers
            const size_t sampleSize = std::min(size, size_t(1024));
            const size_t stride = size / sampleSize;
            
            for (size_t i = 0; i < sampleSize; i++) {
                hash = hash * 31 + bytes[i * stride];
            }
            
            // Add buffer size to hash to differentiate same content with different sizes
            hash = hash * 31 + size;
            
            // If cache is full, remove least recently used entry
            if (cache.size() >= MAX_BUFFER_CACHE_SIZE) {
                size_t lruIndex = 0;
                uint64_t lruFrame = UINT64_MAX;
                
                for (size_t i = 0; i < cache.size(); i++) {
                    if (cache[i].lastUsedFrame < lruFrame) {
                        lruFrame = cache[i].lastUsedFrame;
                        lruIndex = i;
                    }
                }
                
                cache[lruIndex] = {Microsoft::WRL::ComPtr<ID3D11Buffer>(buffer), hash, GetFrameCount()};
            } else {
                cache.push_back({Microsoft::WRL::ComPtr<ID3D11Buffer>(buffer), hash, GetFrameCount()});
            }
        }

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

        /**
         * Destroy geometry and instance handles that are no longer needed
         * 
         * This function identifies handles for objects that haven't been seen recently
         * and destroys them to free up GPU resources.
         * 
         * @param currentFrame The current frame number for timing
         */
        void DestroyUnusedHandles(uint64_t currentFrame)
        {
            if (!g_executeContext) {
                return;
            }
            
            // Wait for GPU to complete all pending operations first
            if (g_renderFence) {
                try {
                    auto context = globals::d3d::context;
                    if (context) {
                        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
                        HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), &context4);
                        if (SUCCEEDED(hr) && g_fenceValue > 0) {
                            context4->Wait(g_renderFence.Get(), g_fenceValue - 1);
                            logger::debug("[RT] Successfully waited for all GPU operations to complete before handle cleanup");
                        }
                    }
                } catch (const std::exception& e) {
                    logger::warn("[RT] Exception during fence wait for cleanup: {}", e.what());
                    return; // Skip cleanup if we can't wait for GPU
                }
            }
            
            // Track object usage by frame number
            static std::unordered_map<std::string, uint64_t> lastSeenFrame;
            
            // First update the lastSeenFrame for all currently active objects
            // This will be called during scene geometry collection, so active objects are marked
            for (const auto& [name, handle] : g_instanceHandles) {
                lastSeenFrame[name] = currentFrame;
                
                // If this instance references a geometry, mark that as used too
                // This ensures geometries used by active instances aren't deleted
                size_t sepPos = name.find("_inst_");
                if (sepPos != std::string::npos) {
                    std::string geomName = name.substr(0, sepPos);
                    if (g_geometryHandles.find(geomName) != g_geometryHandles.end()) {
                        lastSeenFrame[geomName] = currentFrame;
                    }
                }
            }
            
            // Clean up instances that haven't been seen recently
            std::vector<std::string> instancesToRemove;
            for (const auto& [name, handle] : g_instanceHandles) {
                auto it = lastSeenFrame.find(name);
                
                // If not found or not seen for a while
                if (it == lastSeenFrame.end() || (currentFrame - it->second > 300)) {
                    // Queue for removal - can't remove during iteration
                    instancesToRemove.push_back(name);
                }
            }
            
            // Now destroy the instances
            for (const auto& name : instancesToRemove) {
                auto it = g_instanceHandles.find(name);
                if (it != g_instanceHandles.end()) {
                    logger::debug("[RT] Destroying unused instance handle: {}", name);
                    
                    // Destroy the instance in KickstartRT
                    g_executeContext->DestroyInstanceHandle(it->second);
                    
                    // Remove from our map
                    g_instanceHandles.erase(it);
                    
                    // Also remove from lastSeenFrame
                    lastSeenFrame.erase(name);
                }
            }
            
            // Clean up geometries that haven't been seen recently and aren't used by any instance
            std::vector<std::string> geometriesToRemove;
            for (const auto& [name, handle] : g_geometryHandles) {
                auto it = lastSeenFrame.find(name);
                
                // If not found or not seen for a while
                if (it == lastSeenFrame.end() || (currentFrame - it->second > 300)) {
                    // Check if any active instance uses this geometry
                    bool usedByInstance = false;
                    for (const auto& [instName, instHandle] : g_instanceHandles) {
                        if (instName.find(name) != std::string::npos) {
                            usedByInstance = true;
                            break;
                        }
                    }
                    
                    if (!usedByInstance) {
                        // Queue for removal - can't remove during iteration
                        geometriesToRemove.push_back(name);
                    }
                }
            }
            
            // Now destroy the geometries
            for (const auto& name : geometriesToRemove) {
                auto it = g_geometryHandles.find(name);
                if (it != g_geometryHandles.end()) {
                    logger::debug("[RT] Destroying unused geometry handle: {}", name);
                    
                    // Destroy the geometry in KickstartRT
                    g_executeContext->DestroyGeometryHandle(it->second);
                    
                    // Remove from our map
                    g_geometryHandles.erase(it);
                    
                    // Also remove from lastSeenFrame
                    lastSeenFrame.erase(name);
                }
            }
            
            // Only call ReleaseDeviceResourcesImmediately if we actually destroyed some handles
            if (!instancesToRemove.empty() || !geometriesToRemove.empty()) {
                // According to the documentation, this releases any device resources that
                // were previously marked for destruction but are now safe to release
                g_executeContext->ReleaseDeviceResourcesImmediately();
                
                logger::info("[RT] Released device resources after destroying {} instances and {} geometries",
                          instancesToRemove.size(), geometriesToRemove.size());
            }
            
            logger::debug("[RT] Handle cleanup complete. Remaining: {} geometries, {} instances",
                        g_geometryHandles.size(), g_instanceHandles.size());
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
     * Modified to accept a task container rather than creating a new one.
     * 
     * @param triShape Skyrim mesh to register
     * @param name Name identifier for the geometry
     * @param outHandle Pointer to receive the geometry handle
     * @param taskContainer Existing task container to schedule tasks
     * @return True if registration was successful, false otherwise
     */
    bool RegisterSkyrimMeshBatched(RE::BSTriShape* triShape, const std::string& name, 
                                 KickstartRT::D3D11::GeometryHandle* outHandle,
                                 KickstartRT::D3D11::TaskContainer* taskContainer)
    {
        if (!g_executeContext || !triShape || !outHandle || !taskContainer) {
            logger::error("[RT] Invalid parameters for batched Skyrim mesh registration");
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
            
            // Get D3D11 device
            ID3D11Device* device = globals::d3d::device;
            if (!device) {
                logger::error("[RT] Failed to get D3D11 device");
                return false;
            }
            
            // Try to find cached buffers first
            ID3D11Buffer* vertexBuffer = FindCachedBuffer(
                g_vertexBufferCache, 
                rendererData->vertexDesc.GetSize() * vertexCount, 
                rendererData->rawVertexData
            );
            
            ID3D11Buffer* indexBuffer = FindCachedBuffer(
                g_indexBufferCache, 
                sizeof(uint16_t) * triangleCount * 3, 
                rendererData->rawIndexData
            );
            
            // Create the vertex buffer if not found in cache
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBufferPtr;
            if (!vertexBuffer) {
                D3D11_BUFFER_DESC vbDesc = {};
                vbDesc.ByteWidth = rendererData->vertexDesc.GetSize() * vertexCount;
                vbDesc.Usage = D3D11_USAGE_DEFAULT;
                vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                vbDesc.CPUAccessFlags = 0;
                vbDesc.StructureByteStride = rendererData->vertexDesc.GetSize();
                vbDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Required for KickstartRT
                
                D3D11_SUBRESOURCE_DATA vbData = {};
                vbData.pSysMem = rendererData->rawVertexData;
                
                HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vertexBufferPtr.GetAddressOf());
                if (FAILED(hr)) {
                    logger::error("[RT] Failed to create vertex buffer for Skyrim mesh. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                    return false;
                }
                
                vertexBuffer = vertexBufferPtr.Get();
                
                // Add to cache for future reuse
                AddBufferToCache(
                    g_vertexBufferCache, 
                    vertexBuffer, 
                    rendererData->vertexDesc.GetSize() * vertexCount, 
                    rendererData->rawVertexData
                );
                
                logger::debug("[RT] Created new vertex buffer for Skyrim mesh '{}'", name);
            } else {
                vertexBufferPtr = Microsoft::WRL::ComPtr<ID3D11Buffer>(vertexBuffer);
                logger::debug("[RT] Reusing cached vertex buffer for Skyrim mesh '{}'", name);
            }
            
            // Create the index buffer if not found in cache
            Microsoft::WRL::ComPtr<ID3D11Buffer> indexBufferPtr;
            if (!indexBuffer) {
                D3D11_BUFFER_DESC ibDesc = {};
                ibDesc.ByteWidth = sizeof(uint16_t) * triangleCount * 3; // 3 indices per triangle
                ibDesc.Usage = D3D11_USAGE_DEFAULT;
                ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
                ibDesc.CPUAccessFlags = 0;
                ibDesc.StructureByteStride = sizeof(uint16_t);
                ibDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Required for KickstartRT
                
                D3D11_SUBRESOURCE_DATA ibData = {};
                ibData.pSysMem = rendererData->rawIndexData;
                
                HRESULT hr = device->CreateBuffer(&ibDesc, &ibData, indexBufferPtr.GetAddressOf());
                if (FAILED(hr)) {
                    logger::error("[RT] Failed to create index buffer for Skyrim mesh. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
                    return false;
                }
                
                indexBuffer = indexBufferPtr.Get();
                
                // Add to cache for future reuse
                AddBufferToCache(
                    g_indexBufferCache, 
                    indexBuffer, 
                    sizeof(uint16_t) * triangleCount * 3, 
                    rendererData->rawIndexData
                );
                
                logger::debug("[RT] Created new index buffer for Skyrim mesh '{}'", name);
            } else {
                indexBufferPtr = Microsoft::WRL::ComPtr<ID3D11Buffer>(indexBuffer);
                logger::debug("[RT] Reusing cached index buffer for Skyrim mesh '{}'", name);
            }
            
            // Create a handle for the geometry
            KickstartRT::D3D11::GeometryHandle handle;
            auto status = g_executeContext->CreateGeometryHandles(&handle, 1);
            if (status != KickstartRT::Status::OK) {
                logger::error("[RT] Failed to create geometry handle. Status: {}", static_cast<int>(status));
                return false;
            }
            
            // Setup geometry task
            KickstartRT::D3D11::BVHTask::GeometryTask geomTask;
            geomTask.taskOperation = KickstartRT::D3D11::BVHTask::TaskOperation::Register;
            geomTask.handle = handle;
            geomTask.input.type = KickstartRT::D3D11::BVHTask::GeometryInput::Type::TrianglesIndexed;
            
            // Determine if this is a dynamic (skinned) mesh by checking if it's part of an actor
            std::string nameStr = name;
            bool isDynamic = nameStr.find("Actor_") != std::string::npos;
            
            // Only allow updates for dynamic meshes to conserve memory
            // See doc2.md: "For static geometry, the buffer is released when its BLAS has been built, 
            // but for dynamic geometry is not released even after the BLAS is built"
            geomTask.input.allowUpdate = isDynamic;
            
            // Add geometry component
            KickstartRT::D3D11::BVHTask::GeometryInput::GeometryComponent component;
            
            // Configure vertex buffer
            component.vertexBuffer.resource = vertexBuffer;
            component.vertexBuffer.format = DXGI_FORMAT_R32G32B32_FLOAT;
            component.vertexBuffer.offsetInBytes = 0;
            component.vertexBuffer.strideInBytes = rendererData->vertexDesc.GetSize();
            component.vertexBuffer.count = vertexCount;
            
            // Configure index buffer
            component.indexBuffer.resource = indexBuffer;
            component.indexBuffer.format = DXGI_FORMAT_R16_UINT;
            component.indexBuffer.offsetInBytes = 0;
            component.indexBuffer.count = triangleCount * 3;
            
            // Add component to geometry task
            geomTask.input.components.push_back(component);
            
            // Schedule the task in the provided container
            taskContainer->ScheduleBVHTask(&geomTask);
            
            // Store the handle and return it - we'll store permanently after successful execution
            g_geometryHandles[name] = handle;
            *outHandle = handle;
            
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during batched geometry registration: {}", e.what());
            return false;
        }
        
        return false;
    }

    /**
     * Create an instance of a geometry in the scene, using a shared task container
     * 
     * @param geometryHandle Handle to the registered geometry
     * @param transform Transformation matrix (position, rotation, scale)
     * @param name Name identifier for the instance
     * @param outHandle Pointer to receive the instance handle
     * @param taskContainer Existing task container to schedule tasks
     * @return True if instance creation was successful, false otherwise
     */
    bool CreateInstanceBatched(KickstartRT::D3D11::GeometryHandle& geometryHandle, 
                              const DirectX::XMFLOAT4X4& transform, 
                              const std::string& name, 
                              KickstartRT::D3D11::InstanceHandle* outHandle,
                              KickstartRT::D3D11::TaskContainer* taskContainer)
    {
        if (!g_executeContext || !outHandle || !taskContainer) {
            logger::error("[RT] Invalid parameters for batched instance creation");
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
            
            // Schedule task in the provided container
            taskContainer->ScheduleBVHTask(&instanceTask);
            
            // Store the handle - we'll store permanently after successful execution
            g_instanceHandles[name] = handle;
            *outHandle = handle;
            
            return true;
        }
        catch (const std::exception& e) {
            logger::error("[RT] Exception during batched instance creation: {}", e.what());
            return false;
        }
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
        
        // Create a single task container for all operations this frame
        auto taskContainer = g_executeContext->CreateTaskContainer();
        if (!taskContainer) {
            logger::error("[RT] Failed to create task container for scene geometry collection");
            return 0;
        }
        
        // Get the root node of the scene using the correct method
        auto worldRoot = RE::Main::WorldRootNode();
        if (!worldRoot) {
            logger::error("[RT] Failed to get world root node");
            return 0;
        }
        
        logger::debug("[RT] Starting scene geometry collection...");
        
        // Wait for previous GPU operations to complete before starting new ones
        // This helps avoid fence timeout/stall issues
        if (g_renderFence) {
            try {
                auto context = globals::d3d::context;
                if (context) {
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
                    HRESULT hr = context->QueryInterface(__uuidof(ID3D11DeviceContext4), &context4);
                    if (SUCCEEDED(hr)) {
                        // Wait for most recent fence before collecting new geometry
                        // This ensures previous submissions are fully processed
                        if (g_fenceValue > 1) {
                            context4->Wait(g_renderFence.Get(), g_fenceValue - 1);
                            logger::debug("[RT] Successfully waited for previous GPU operations, fence value: {}", g_fenceValue - 1);
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                logger::warn("[RT] Exception during fence wait: {}", e.what());
            }
        }
        
        // Get player position for distance-based culling
        RE::NiPoint3 playerPosition;
        float maxDistance = 512.0f; // Reduced from 2048.0f for better performance
        
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            playerPosition = player->GetPosition();
        }
        
        // Process only a limited number of objects per frame
        const int MAX_PROCESSED_PER_FRAME = updateDynamicOnly ? 20 : 100; // Reduced for better performance
        const int MAX_REGISTERED_TOTAL = updateDynamicOnly ? 10 : 20;     // Significantly reduced
        
        // Create a timestamp string to uniquely identify this collection pass
        static uint32_t collectionCounter = 0;
        std::string timeStamp = std::to_string(++collectionCounter) + "_" + 
                               (updateDynamicOnly ? "dyn" : "full");
        
        // Keep track of what's been processed this frame
        static std::unordered_set<RE::BSGeometry*> processedThisFrame;
        processedThisFrame.clear();
        
        // Structure to hold geometry candidates with their importance metrics
        struct GeometryCandidate {
            RE::BSTriShape* triShape;
            float distance;
            float radius;
            float importance; // Higher is more important
            std::string name;
            bool isDynamic;
        };
        
        // Vector to collect candidates before sorting by importance
        std::vector<GeometryCandidate> candidates;
        candidates.reserve(MAX_PROCESSED_PER_FRAME);
        
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
                                
                                // Early exit if we're hitting our per-frame processing limit
                                if (processedCount > MAX_PROCESSED_PER_FRAME) {
                                    return RE::BSVisit::BSVisitControl::kStop;
                                }
                                
                                // Skip invalid or small geometries
                                if (!geometry || geometry->worldBound.radius < 5.0f) {
                                    return RE::BSVisit::BSVisitControl::kContinue;
                                }
                                
                                // Convert to BSTriShape if possible
                                auto triShape = geometry->AsTriShape();
                                if (!triShape) {
                                    return RE::BSVisit::BSVisitControl::kContinue;
                                }

                                // Calculate distance from player
                                float distance = (geometry->world.translate - playerPosition).Length();
                                
                                // Use size/distance ratio as importance - larger and closer objects are more important
                                float importance = geometry->worldBound.radius / (distance + 1.0f);
                                
                                // Create a unique name for this geometry 
                                std::string name = "Actor_" + std::to_string(actor->formID) + "_" + 
                                                 std::to_string(candidates.size()) + "_" + timeStamp;
                                
                                // Add to candidates
                                candidates.push_back({
                                    triShape,
                                    distance,
                                    geometry->worldBound.radius,
                                    importance,
                                    name,
                                    true // Actor geometries are considered dynamic
                                });
                                
                                return RE::BSVisit::BSVisitControl::kContinue;
                            });
                        }
                    }
                }
            }
        } else {
            // For full updates, process the entire scene graph with prioritization  
            // Use BSVisit to traverse the scene graph and collect candidates
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
                
                // Progressive level of detail - scale minimum object size with distance
                float distance = (geometry->world.translate - playerPosition).Length();
                float minObjectSize = 10.0f + (distance * 0.02f);
                
                // Filter out small geometries
                if (geometry->worldBound.radius < minObjectSize) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                
                // Distance-based culling
                if (distance > maxDistance) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                
                // Convert to BSTriShape if possible
                auto triShape = geometry->AsTriShape();
                if (!triShape) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                
                // Use size/distance ratio as importance - larger and closer objects are more important
                float importance = geometry->worldBound.radius / (distance + 1.0f);
                
                // Create a unique name for this geometry
                std::string name = "SceneObj_" + std::to_string(candidates.size()) + "_" + timeStamp;
                
                // Add to candidates
                candidates.push_back({
                    triShape,
                    distance,
                    geometry->worldBound.radius,
                    importance,
                    name,
                    false // Static scene objects are not considered dynamic
                });
                
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        }
        
        // Sort candidates by importance (highest first)
        std::sort(candidates.begin(), candidates.end(), 
                  [](const GeometryCandidate& a, const GeometryCandidate& b) {
                      return a.importance > b.importance;
                  });
        
        // Register only the most important candidates up to the limit
        size_t registerCount = std::min(candidates.size(), static_cast<size_t>(MAX_REGISTERED_TOTAL));
        
        logger::info("[RT] Collected {} potential objects, registering top {} by importance", 
                    candidates.size(), registerCount);
        
        // ===== Based on KickstartRT documentation =====
        // First schedule a BVH build task with optimal settings
        KickstartRT::D3D11::BVHTask::BVHBuildTask bvhBuildTask;
        bvhBuildTask.buildTLAS = true; // Build top-level acceleration structure
        
        // Limit the number of BLAS builds per frame to avoid stalls
        // From the docs: "By generating a copy of the vertex buffer, the SDK can control the number of BLASs 
        // created per BVHBuildTask in order to adjust the BLAS creation load."
        bvhBuildTask.maxBlasBuildCount = updateDynamicOnly ? 4u : 8u;
        
        // Schedule the BVH build task
        taskContainer->ScheduleBVHTask(&bvhBuildTask);
        
        // Register the candidates - now using the batched approach
        for (size_t i = 0; i < registerCount; i++) {
            const auto& candidate = candidates[i];
            
            KickstartRT::D3D11::GeometryHandle handle;
            if (RegisterSkyrimMeshBatched(candidate.triShape, candidate.name, &handle, taskContainer)) {
                // Get the world transform from the geometry
                DirectX::XMFLOAT4X4 worldTransform;
                
                // Access transform directly from NiAVObject's world member
                const RE::NiTransform& transform = candidate.triShape->world;
                
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
                
                // Create an instance with the world transform - also batched
                KickstartRT::D3D11::InstanceHandle instanceHandle;
                if (CreateInstanceBatched(handle, worldTransform, candidate.name + "_Instance", &instanceHandle, taskContainer)) {
                    registeredCount++;
                    
                    // Log prioritization info for debugging
                    logger::debug("[RT] Scheduled registration for object: radius={:.1f}, dist={:.1f}, importance={:.4f}, dynamic={}", 
                        candidate.radius, candidate.distance, candidate.importance, candidate.isDynamic ? "yes" : "no");
                }
            }
        }
        
        // Execute the single task container with all operations
        if (registeredCount > 0) {
            logger::info("[RT] Executing batch of {} registration operations", registeredCount);
            
            // Use optimal parameters for ExecuteTaskContainer based on KickstartRT docs
            if (ExecuteTaskContainer(taskContainer, true, updateDynamicOnly ? 4u : 8u)) {
                logger::info("[RT] Successfully executed all scheduled geometry tasks");
            } else {
                logger::error("[RT] Failed to execute geometry tasks");
                // If execution failed, clear the temporary handles we added
                for (const auto& candidate : candidates) {
                    g_geometryHandles.erase(candidate.name);
                    g_instanceHandles.erase(candidate.name + "_Instance");
                }
                registeredCount = 0;
            }
        } else {
            // Clean up the task container if we didn't use it
            logger::debug("[RT] No objects to register, skipping execution");
        }
        
        // Clean up old buffers from cache periodically
        static uint64_t lastCleanupFrame = 0;
        uint64_t currentFrame = GetFrameCount();
        if (currentFrame - lastCleanupFrame > 120) { // ~ every 2 seconds at 60 fps
            size_t numRemoved = 0;
            
            // Remove buffers that haven't been used in the last 300 frames (~ 5 seconds at 60 fps)
            auto cleanupCache = [currentFrame, &numRemoved](std::vector<BufferCacheEntry>& cache) {
                auto it = cache.begin();
                while (it != cache.end()) {
                    if (currentFrame - it->lastUsedFrame > 300) {
                        it = cache.erase(it);
                        numRemoved++;
                    } else {
                        ++it;
                    }
                }
            };
            
            cleanupCache(g_vertexBufferCache);
            cleanupCache(g_indexBufferCache);
            
            if (numRemoved > 0) {
                logger::debug("[RT] Cleaned up {} unused buffers from cache", numRemoved);
            }
            
            lastCleanupFrame = currentFrame;
        }
        
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
    
    /**
     * Clean up all cached resources
     * This should be called when unloading areas or during shutdown
     */
    void CleanupGeometryResources()
    {
        static uint64_t lastCleanupFrame = 0;
        
        uint64_t currentFrame = GetFrameCount();
        
        // Only clean up occasionally to avoid overhead
        if (currentFrame - lastCleanupFrame > 120) { // ~ every 2 seconds at 60 fps
            
            // Record that we performed cleanup
            lastCleanupFrame = currentFrame;
            
            // Clean up cached buffers that haven't been used recently
            if (!g_vertexBufferCache.empty() || !g_indexBufferCache.empty()) {
                
                logger::debug("[RT] Cleaning up {} vertex and {} index buffer cache entries", 
                              g_vertexBufferCache.size(), g_indexBufferCache.size());
                
                // Clean vertex buffer cache
                auto it = g_vertexBufferCache.begin();
                while (it != g_vertexBufferCache.end()) {
                    if (currentFrame - it->lastUsedFrame > 300) {
                        logger::trace("[RT] Removing old vertex buffer cache entry");
                        it = g_vertexBufferCache.erase(it);
                    } else {
                        ++it;
                    }
                }
                
                // Clean index buffer cache
                it = g_indexBufferCache.begin();
                while (it != g_indexBufferCache.end()) {
                    if (currentFrame - it->lastUsedFrame > 300) {
                        logger::trace("[RT] Removing old index buffer cache entry");
                        it = g_indexBufferCache.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
            // Destroy handles for objects no longer in the scene
            DestroyUnusedHandles(currentFrame);
        }
    }
} // namespace KickstartRTImpl 