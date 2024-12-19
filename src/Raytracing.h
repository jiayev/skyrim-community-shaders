#pragma once

#include <eastl/set.h>
#include <shared_mutex>
#include <unordered_set>

#include <d3d12.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>

#include "Buffer.h"
#include "State.h"
#include <FidelityFX/host/backends/dx12/d3dx12.h>
#include <RE/A/AnimationObjects.h>

interface DECLSPEC_UUID("9f251514-9d4d-4902-9d60-18988ab7d4b5") DECLSPEC_NOVTABLE

	IDXGraphicsAnalysis : public IUnknown
{
	STDMETHOD_(void, BeginCapture)
	() PURE;

	STDMETHOD_(void, EndCapture)
	() PURE;
};

// Brixelizer supports a maximum of 24 raw cascades
// In the sample each cascade level we build is created by building a static cascade,
// a dynamic cascade, and then merging those into a merged cascade. Hence we require
// 3 raw cascades per cascade level.
#define NUM_BRIXELIZER_CASCADES (FFX_BRIXELIZER_MAX_CASCADES / 3)
// Brixelizer makes use of a scratch buffer for calculating cascade updates. Hence in
// this sample we allocate a buffer to be used as scratch space. Here we have chosen
// a somewhat arbitrary large size for use as scratch space, in a real application this
// value should be tuned to what is required by Brixelizer.
constexpr UINT64 GPU_SCRATCH_BUFFER_SIZE = 1ull << 30;

class Raytracing
{
public:
	static Raytracing* GetSingleton()
	{
		static Raytracing singleton;
		return &singleton;
	}

	struct FrameBuffer
	{
		Matrix CameraView;
		Matrix CameraProj;
		Matrix CameraViewProj;
		Matrix CameraViewProjUnjittered;
		Matrix CameraPreviousViewProjUnjittered;
		Matrix CameraProjUnjittered;
		Matrix CameraProjUnjitteredInverse;
		Matrix CameraViewInverse;
		Matrix CameraViewProjInverse;
		Matrix CameraProjInverse;
		float4 CameraPosAdjust;
		float4 CameraPreviousPosAdjust;
		float4 FrameParams;
		float4 DynamicResolutionParams1;
		float4 DynamicResolutionParams2;
	};

	D3D11_MAPPED_SUBRESOURCE* mappedFrameBuffer = nullptr;
	FrameBuffer frameBufferCached{};

	void CacheFramebuffer();

	winrt::com_ptr<IDXGraphicsAnalysis> ga;
	bool debugAvailable = false;
	bool debugCapture = false;

	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<ID3D12CommandAllocator> commandAllocator;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList;

	winrt::com_ptr<ID3D12Fence> fence;
	UINT64 fenceValue = 0;
	HANDLE fenceEvent = nullptr;

	FfxBrixelizerContextDescription initializationParameters = {};
	FfxBrixelizerContext brixelizerContext = {};
	FfxBrixelizerBakedUpdateDescription brixelizerBakedUpdateDesc = {};
	FfxBrixelizerStats stats = {};
	FfxBrixelizerBakedUpdateDescription bakedUpdateDesc = {};

	winrt::com_ptr<ID3D12Resource> sdfAtlas;
	winrt::com_ptr<ID3D12Resource> brickAABBs;
	winrt::com_ptr<ID3D12Resource> gpuScratchBuffer;

	winrt::com_ptr<ID3D12Resource> cascadeAABBTrees[FFX_BRIXELIZER_MAX_CASCADES];
	winrt::com_ptr<ID3D12Resource> cascadeBrickMaps[FFX_BRIXELIZER_MAX_CASCADES];

	ID3D11Texture2D* debugRenderTargetd3d11;
	ID3D11ShaderResourceView* debugSRV;
	ID3D12Resource* debugRenderTarget;

	FfxBrixelizerTraceDebugModes m_DebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_GRAD;

	int m_StartCascadeIdx = 0;
	int m_EndCascadeIdx = NUM_BRIXELIZER_CASCADES - 1;

	float m_TMin = 0.0f;
	float m_TMax = 10000.0f;
	float m_SdfSolveEps = 0.5f;

	bool m_ShowStaticInstanceAABBs = false;
	bool m_ShowDynamicInstanceAABBs = false;
	bool m_ShowCascadeAABBs = false;
	int m_ShowAABBTreeIndex = -1;

	void DrawSettings();
	void InitD3D12(IDXGIAdapter* a_adapter);

	winrt::com_ptr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_RESOURCE_STATES resourceState, D3D12_RESOURCE_FLAGS flags);

	void InitBrixelizer();

	void OpenSharedHandles();

	struct Vertex
	{
		float4 position;
	};

	struct BufferData
	{
		winrt::com_ptr<ID3D12Resource> buffer;
		bool registered = false;
		uint width;
		uint index;
	};

	BufferData AllocateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData);

	eastl::hash_map<ID3D11Buffer*, BufferData> vertexBuffers;
	eastl::hash_map<ID3D11Buffer*, BufferData> indexBuffers;

	struct LODMode
	{
		std::uint8_t index: 7;
		bool singleLevel: 1;
	};

	bool visibleState = true;

	struct InstanceData
	{
		FfxBrixelizerInstanceID instanceID;
		bool state = false;
	};

	eastl::hash_set<RE::BSTriShape*> queuedInstances;
	eastl::hash_map<RE::BSTriShape*, InstanceData> instances;

	uint GetBufferIndex(BufferData& a_bufferData);

	void UpdateGeometry(RE::BSRenderPass* a_pass);

	void CheckInstance(RE::BSTriShape* geometry, InstanceData& instanceData);
	void AddInstance(RE::BSTriShape* geometry);
	void SeenInstance(RE::BSTriShape* geometry);
	void RemoveInstance(RE::BSTriShape* a_geometry);

	void RegisterVertexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer);
	void RegisterIndexBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer);

	void TransitionResources(D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter);

	void InitFenceAndEvent();
	void WaitForD3D11();
	void WaitForD3D12();

	FfxBrixelizerDebugVisualizationDescription GetDebugVisualization();

	void FrameUpdate();
	void PopulateCommandList();

	void RecurseSceneGraph(RE::NiAVObject* a_object);

	struct RenderTargetDataD3D12
	{
		// D3D12 Resource
		winrt::com_ptr<ID3D12Resource> d3d12Resource;
	};

	std::shared_mutex mutex;

	RenderTargetDataD3D12 renderTargetsD3D12[RE::RENDER_TARGET::kTOTAL];
	RenderTargetDataD3D12 ConvertD3D11TextureToD3D12(RE::BSGraphics::RenderTargetData* rtData);

	void BSTriShape_UpdateWorldData(RE::BSTriShape* This, RE::NiUpdateData* a_data);
	void BSTriShape_SetAppCulled(RE::BSTriShape* This, bool a_cull);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				func(This, Pass, RenderFlags);
				GetSingleton()->UpdateGeometry(Pass);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSTriShape_UpdateWorldData
		{
			static void thunk(RE::BSTriShape* This, RE::NiUpdateData* a_data)
			{
				GetSingleton()->BSTriShape_UpdateWorldData(This, a_data);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSTriShape_SetAppCulled
		{
			static void thunk(RE::BSTriShape* This, bool a_cull)
			{
				GetSingleton()->BSTriShape_SetAppCulled(This, a_cull);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct NiCullingProcess_Process1
		{
			static void thunk(RE::NiCullingProcess* This, RE::NiAVObject* a_object, std::uint32_t a_arg2)
			{
				if (auto triShape = a_object->AsTriShape())
					GetSingleton()->SeenInstance(triShape);
				func(This, a_object, a_arg2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSCullingProcess_Process1
		{
			static void thunk(RE::NiCullingProcess* This, RE::NiAVObject* a_object, std::uint32_t a_arg2)
			{
				if (auto triShape = a_object->AsTriShape())
					GetSingleton()->SeenInstance(triShape);
				func(This, a_object, a_arg2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGeometryListCullingProcess_Process1
		{
			static void thunk(RE::NiCullingProcess* This, RE::NiAVObject* a_object, std::uint32_t a_arg2)
			{
				if (auto triShape = a_object->AsTriShape())
					GetSingleton()->SeenInstance(triShape);
				func(This, a_object, a_arg2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			//stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);

			//if (REL::Module::IsAE()) {
			//	stl::write_vfunc<0x31, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			//} else {
			//	stl::write_vfunc<0x30, BSTriShape_UpdateWorldData>(RE::VTABLE_BSTriShape[0]);
			//}

			//stl::write_vfunc<0x16, NiCullingProcess_Process1>(RE::VTABLE_NiCullingProcess[0]);
			//	stl::write_vfunc<0x16, BSCullingProcess_Process1>(RE::VTABLE_BSCullingProcess[0]);
			stl::write_vfunc<0x16, BSGeometryListCullingProcess_Process1>(RE::VTABLE_BSGeometryListCullingProcess[0]);

			//	stl::detour_thunk<NiNode_Destroy>(REL::RelocationID(68937, 70288));

			logger::info("[Raytracing] Installed hooks");
		}
	};
};
