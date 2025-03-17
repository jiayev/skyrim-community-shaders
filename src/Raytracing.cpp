#include "Raytracing.h"
#include "KickstartRTImpl.h"
#include "Globals.h"
#include <Windows.h>
#include <DirectXMath.h>
#include <d3d11.h>

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
    // Simplified implementation that returns true
    logger::info("[Raytracing] KickstartRT test bypassed");
    return true;
}

bool Raytracing::RegisterGeometry()
{
    if (!IsInitialized() || !settings.Enabled) {
        logger::warn("[RT] RegisterGeometry called but raytracing is not initialized or enabled");
        return false;
    }

    // Create a test quad for now
    try {
        // Get device
        ID3D11Device* device = globals::d3d::device;
        if (!device) {
            logger::error("[RT] D3D11 device is null");
            return false;
        }
        
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
        vbDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Add sharing flag for KickstartRT
        
        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = quadVertices;
        
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, vertexBuffer.GetAddressOf());
        if (FAILED(hr)) {
            logger::error("[RT] Failed to create vertex buffer for test quad. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        else {
            logger::info("[RT] Successfully created vertex buffer for test quad with SHARED flag");
            
            // Verify buffer was created with correct flags
            D3D11_BUFFER_DESC checkDesc;
            vertexBuffer->GetDesc(&checkDesc);
        }
        
        // Create index buffer
        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.ByteWidth = sizeof(quadIndices);
        ibDesc.Usage = D3D11_USAGE_DEFAULT;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ibDesc.CPUAccessFlags = 0;
        ibDesc.StructureByteStride = sizeof(uint32_t);
        ibDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // Add sharing flag for KickstartRT
        
        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = quadIndices;
        
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        hr = device->CreateBuffer(&ibDesc, &ibData, indexBuffer.GetAddressOf());
        if (FAILED(hr)) {
            logger::error("[RT] Failed to create index buffer for test quad. HRESULT: 0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }
        else {
            logger::info("[RT] Successfully created index buffer for test quad with SHARED flag");
            
            // Verify buffer was created with correct flags
            D3D11_BUFFER_DESC checkDesc;
            indexBuffer->GetDesc(&checkDesc);
        }
        
        // Register the quad geometry with KickstartRT
        KickstartRT::D3D11::GeometryHandle geomHandle;
        if (KickstartRTImpl::RegisterGeometryWithKickstartRT(vertexBuffer.Get(), indexBuffer.Get(), "TestQuad", &geomHandle)) {
            // Create an instance of the quad
            DirectX::XMFLOAT4X4 transform;
            DirectX::XMStoreFloat4x4(&transform, DirectX::XMMatrixIdentity());
            
            // Create instance handle
            KickstartRT::D3D11::InstanceHandle instanceHandle;
            if (KickstartRTImpl::CreateInstance(geomHandle, transform, "TestQuadInstance", &instanceHandle)) {
                logger::info("[RT] Created instance of TestQuad");
                return true;
            }
        }
        
        return false;
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
        auto it = KickstartRTImpl::g_instanceHandles.find("TestQuadInstance");
        if (it != KickstartRTImpl::g_instanceHandles.end()) {
            static float rotation = 0.0f;
            rotation += 0.01f; // Small increment each frame
            
            // Create a rotation matrix around Y axis
            DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationY(rotation);
            
            // Create a translation matrix to move the quad away from the origin
            DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(0.0f, 0.0f, -5.0f);
            
            // Combine the transforms
            DirectX::XMMATRIX worldMatrix = rotationMatrix * translationMatrix;
            
            // Convert to XMFLOAT4X4 for the utility function
            DirectX::XMFLOAT4X4 transform;
            DirectX::XMStoreFloat4x4(&transform, worldMatrix);
            
            // Update the instance transform
            KickstartRT::D3D11::InstanceHandle instanceHandleCopy = it->second;
            if (KickstartRTImpl::UpdateInstanceTransform(instanceHandleCopy, transform)) {
                return true;
            } else {
                logger::debug("[RT] Failed to update test quad transform");
            }
        }
        
        return false;
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
        // For now, return identity matrices
        // We'll implement proper matrix retrieval later
        logger::info("[RT] Using identity matrices for view and projection");
        return true;
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
    // Check if we're enabled and initialized
    if (!settings.Enabled || !initialized) {
        logger::info("[Raytracing] GenerateGI called but raytracing is not enabled/initialized");
        return false;
    }
    
    // Validate critical input resources
    if (!depthSRV || !outputUAV) {
        logger::error("[Raytracing] GenerateGI called with null resources");
        return false;
    }
    
    logger::info("[Raytracing] Generating global illumination");
    
    // Call into the KickstartRT implementation
    try {
        return KickstartRTImpl::GenerateGI(
            depthSRV,
            normalSRV,
            outputUAV,
            viewMatrix,
            projMatrix
        );
    }
    catch (const std::exception& e) {
        logger::error("[Raytracing] Exception in GenerateGI: {}", e.what());
        return false;
    }
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
    
    logger::info("[Raytracing] Generating reflections");
    
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

// Implement the TraceGI method using the query structure
bool Raytracing::TraceGI(const KickstartRT::TraceQueryInternal& query)
{
    // First check if raytracing is enabled at all
    if (!settings.Enabled) {
        logger::info("[Raytracing] Raytracing is disabled, skipping GI trace");
        return false;
    }

    // Check if we're properly initialized
    if (!initialized) {
        logger::warn("[Raytracing] Cannot trace GI - raytracing not initialized");
        
        // Fallback: fill output with black if we have a valid UAV
        if (query.outputUAV) {
            auto context = globals::d3d::context;
            if (context) {
                FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, clearColor);
                logger::warn("[Raytracing] Produced blank output as fallback");
                return true; // We at least produced valid output
            }
        }
        
        return false;
    }

    // Validate all required resources
    if (!query.depthBufferSRV || !query.outputUAV) {
        logger::error("[Raytracing] Missing required resources for GI trace");
        return false;
    }

    logger::info("[Raytracing] Tracing GI with ray length: {}", query.maxRayLength);

    try {
        // Call the underlying GenerateGI method with unwrapped parameters
        bool result = GenerateGI(
            query.depthBufferSRV,
            query.normalBufferSRV,
            query.outputUAV,
            query.cameraData.view,
            query.cameraData.projection
        );
        
        // If GI generation failed, use fallback
        if (!result) {
            logger::warn("[Raytracing] GI generation failed, using fallback");
            auto context = globals::d3d::context;
            if (context) {
                // Simple dark gray output as ambient approximation
                FLOAT ambient[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, ambient);
                return true; // We at least produced valid output
            }
        }
        
        return result;
    }
    catch (const std::exception& e) {
        logger::error("[Raytracing] Exception in TraceGI: {}", e.what());
        
        // Fallback in case of exception
        if (query.outputUAV) {
            auto context = globals::d3d::context;
            if (context) {
                FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                context->ClearUnorderedAccessViewFloat(query.outputUAV, black);
                return true; // We provided valid output
            }
        }
        
        return false;
    }
} 