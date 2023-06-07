#pragma once
#include <cstdint>
#include <d3d11.h>

#define MAX_SHARED_PARTICLES_SIZE 2048
#define MAX_PARTICLE_STRIP_COUNT 51200

#define MAX_VS_CONSTANTS 20
#define MAX_PS_CONSTANTS 64
#define MAX_CS_CONSTANTS 32

#include "BSGraphics.h"
#include "BSShaderRenderTargets.h"

namespace BSGraphics
{
	enum BSShaderTimerMode
	{
		TIMER_MODE_DEFAULT = 0,
		TIMER_MODE_DELTA = 1,
		TIMER_MODE_SYSTEM = 2,
		TIMER_MODE_REAL_DELTA = 3,
		TIMER_MODE_FRAME_COUNT = 4,
		TIMER_MODE_COUNT = 5,
	};

	class ShaderState
	{
	public:
		class ShadowSceneNode* pShadowSceneNode[4];
		float fTimerValues[TIMER_MODE_COUNT];
		RE::NiColorA LoadedRange;
		bool bInterior;
		bool bLiteBrite;
		bool CharacterLightEnabled;
		char _pad0[0x51];
		float fLandLOFadeSeconds;
		float fInvFrameBufferRange;
		float fLeafAnimDampenDistStartSPU;
		float fLeafAnimDampenDistEndSPU;
		RE::NiPoint2 kOldGridArrayCenter;
		RE::NiPoint2 kGridArrayCenter;
		float kfGriddArrayLerpStart;
		uint32_t uiCurrentShaderTechnique;
		uint8_t cSceneGraph;
		uint32_t usDebugMode;
		RE::NiTransform DirectionalAmbientTransform;
		RE::NiColorA AmbientSpecular;
		RE::NiColorA CharacterLightParams;  // { Primary, Secondary, Luminance, Max Luminance }
		bool bAmbientSpecularEnabled;
		uint32_t uiTextureTransformCurrentBuffer;
		uint32_t uiTextureTransformFlipMode;
		uint32_t uiCameraInWaterState;
		RE::NiBound kCachedPlayerBound;
		char _pad1[0x8];
		float fWaterIntersect;

		static ShaderState* QInstance();
	};

	enum class Space
	{
		World = 0,
		Model = 1,
	};

	//
	// Renderer shadow state settings
	//
	enum ShaderFlags : uint32_t
	{
		DIRTY_RENDERTARGET = 0x1,
		DIRTY_VIEWPORT = 0x2,
		DIRTY_DEPTH_MODE = 0x4,
		DIRTY_DEPTH_STENCILREF_MODE = 0x8,
		DIRTY_UNKNOWN1 = 0x10,
		DIRTY_RASTER_CULL_MODE = 0x20,
		DIRTY_RASTER_DEPTH_BIAS = 0x40,
		DIRTY_ALPHA_BLEND = 0x80,
		DIRTY_ALPHA_TEST_REF = 0x100,
		DIRTY_ALPHA_ENABLE_TEST = 0x200,
		DIRTY_VERTEX_DESC = 0x400,
		DIRTY_PRIMITIVE_TOPO = 0x800,
		DIRTY_UNKNOWN2 = 0x1000,
	};

	enum ClearDepthStencilTarget
	{
		CLEAR_DEPTH_STENCIL_TARGET_DEPTH = 1,
		CLEAR_DEPTH_STENCIL_TARGET_STENCIL = 2,
		CLEAR_DEPTH_STENCIL_TARGET_ALL = 3,
	};

	enum SetRenderTargetMode : uint32_t
	{
		SRTM_CLEAR = 0,
		SRTM_CLEAR_DEPTH = 1,
		SRTM_CLEAR_STENCIL = 2,
		SRTM_RESTORE = 3,
		SRTM_NO_CLEAR = 4,
		SRTM_FORCE_COPY_RESTORE = 5,
		SRTM_INIT = 6,
	};

	enum DepthStencilStencilMode
	{
		DEPTH_STENCIL_STENCIL_MODE_DISABLED = 0,

		DEPTH_STENCIL_STENCIL_MODE_DEFAULT = DEPTH_STENCIL_STENCIL_MODE_DISABLED,  // Used for BSShader::RestoreX
	};

	enum DepthStencilDepthMode
	{
		DEPTH_STENCIL_DEPTH_MODE_DISABLED = 0,
		DEPTH_STENCIL_DEPTH_MODE_TEST = 1,
		DEPTH_STENCIL_DEPTH_MODE_TEST_WRITE = 3,
		DEPTH_STENCIL_DEPTH_MODE_TESTEQUAL = 4,    // Unverified
		DEPTH_STENCIL_DEPTH_MODE_TESTGREATER = 6,  // Unverified

		DEPTH_STENCIL_DEPTH_MODE_DEFAULT = DEPTH_STENCIL_DEPTH_MODE_TEST_WRITE,  // Used for BSShader::RestoreX
	};

	enum RasterStateCullMode
	{
		RASTER_STATE_CULL_MODE_BACK = 1,

		RASTER_STATE_CULL_MODE_DEFAULT = RASTER_STATE_CULL_MODE_BACK,  // Used for BSShader::RestoreX
	};

	struct RenderTargetProperties
	{
		uint32_t uiWidth;
		uint32_t uiHeight;
		DXGI_FORMAT eFormat;
		bool bCopyable;
		bool bSupportUnorderedAccess;
		bool bAllowMipGeneration;
		int iMipLevel;
		uint32_t uiTextureTarget;
		uint32_t uiUnknown;
	};
	static_assert(sizeof(RenderTargetProperties) == 0x1C);

	struct DepthStencilTargetProperties
	{
		uint32_t uiWidth;
		uint32_t uiHeight;
		uint32_t uiArraySize;
		bool Unknown1;
		bool Stencil;
		bool Use16BitsDepth;
	};
	static_assert(sizeof(DepthStencilTargetProperties) == 0x10);

	struct CubeMapRenderTargetProperties
	{
		uint32_t uiWidth;
		uint32_t uiHeight;
		DXGI_FORMAT eFormat;
	};
	static_assert(sizeof(CubeMapRenderTargetProperties) == 0xC);

	struct RenderTargetData
	{
		ID3D11Texture2D* Texture;
		ID3D11Texture2D* TextureCopy;
		ID3D11RenderTargetView* RTV;        // For "Texture"
		ID3D11ShaderResourceView* SRV;      // For "Texture"
		ID3D11ShaderResourceView* SRVCopy;  // For "TextureCopy"
		ID3D11UnorderedAccessView* UAV;     // For "Texture"
	};
	static_assert(sizeof(RenderTargetData) == 0x30);

	struct DepthStencilData
	{
		ID3D11Texture2D* Texture;
		ID3D11DepthStencilView* Views[8];
		ID3D11DepthStencilView* ReadOnlyViews[8];
		ID3D11ShaderResourceView* DepthSRV;
		ID3D11ShaderResourceView* StencilSRV;
	};
	static_assert(sizeof(DepthStencilData) == 0x98);

	struct CubemapRenderTargetData
	{
		ID3D11Texture2D* Texture;
		ID3D11RenderTargetView* CubeSideRTV[6];
		ID3D11ShaderResourceView* SRV;
	};
	static_assert(sizeof(CubemapRenderTargetData) == 0x40);

	struct Texture3DTargetData
	{
		ID3D11Texture3D* texture;        // 00
		ID3D11UnorderedAccessView* uav;  // 08
		ID3D11ShaderResourceView* SRV;   // 10
		uint64_t unkCount;               // 18
	};
	static_assert(sizeof(Texture3DTargetData) == 0x20);

	//
	// General resources
	//
	struct Texture
	{
		ID3D11Texture2D* m_Texture;
		char _pad0[0x8];
		ID3D11ShaderResourceView* m_ResourceView;
	};

	struct Buffer
	{
		ID3D11Buffer* m_Buffer;  // Selected from pool in Load*ShaderFromFile()
		void* m_Data;            // m_Data = DeviceContext->Map(m_Buffer)

		// Based on shader load flags, these **can be null**. At least one of the
		// pointers is guaranteed to be non-null.
	};

	//
	// Constant group types used for shader parameters
	//
	const uint8_t INVALID_CONSTANT_BUFFER_OFFSET = 0xFF;

	enum ConstantGroupLevel
	{
		CONSTANT_GROUP_LEVEL_TECHNIQUE = 0x0,  // Varies between PS/VS shaders
		CONSTANT_GROUP_LEVEL_MATERIAL = 0x1,   // Varies between PS/VS shaders
		CONSTANT_GROUP_LEVEL_GEOMETRY = 0x2,   // Varies between PS/VS shaders
		CONSTANT_GROUP_LEVEL_COUNT = 0x3,

		// Slot 7 is used for grass but unknown
		CONSTANT_GROUP_LEVEL_INSTANCE = 0x8,  // Instanced geometry such as grass and trees
		CONSTANT_GROUP_LEVEL_PREVIOUS_BONES = 0x9,
		CONSTANT_GROUP_LEVEL_BONES = 0xA,
		CONSTANT_GROUP_LEVEL_ALPHA_TEST_REF = 0xB,  // PS/VS. Used as a single float value for alpha testing (16 bytes allocated)
		CONSTANT_GROUP_LEVEL_PERFRAME = 0xC,        // PS/VS. Per-frame constants. Contains view projections and some other variables.
	};

	class CustomConstantGroup
	{
		friend class Renderer;
		template <typename>
		friend class ConstantGroup;

	private:
		//
		// Invalid constant offsets still need a place to be written to. This is supposed to
		// be in ConstantGroup<T>, but it causes a compiler crash.
		//
		// See: ConstantGroup<T>::ParamVS, INVALID_CONSTANT_BUFFER_OFFSET
		//
		inline static char EmptyWriteBuffer[1024];

		D3D11_MAPPED_SUBRESOURCE m_Map{};
		ID3D11Buffer* m_Buffer = nullptr;
		bool m_Unified = false;            // True if buffer is from global ring buffer
		uint32_t m_UnifiedByteOffset = 0;  // Offset into ring buffer

	public:
		inline void* RawData() const
		{
			return m_Map.pData;
		}
	};

	template <typename T>
	class ConstantGroup : public CustomConstantGroup
	{
		friend class Renderer;

	private:
		T* m_Shader = nullptr;

		template <typename U>
		U& MapVar(uint32_t Offset) const
		{
			static_assert(sizeof(U) <= sizeof(EmptyWriteBuffer));

			if (RawData() == nullptr || Offset == INVALID_CONSTANT_BUFFER_OFFSET)
				return *(U*)EmptyWriteBuffer;

			return *(U*)((uintptr_t)RawData() + (Offset * sizeof(float)));
		}

	public:
		template <typename U, uint32_t ParamIndex>
		U& ParamVS() const
		{
			static_assert(std::is_same<T, struct VertexShader>::value, "ParamVS() requires ConstantGroup<VertexShader>");
			static_assert(ParamIndex < MAX_VS_CONSTANTS);

			return MapVar<U>(m_Shader->m_ConstantOffsets[ParamIndex]);
		}

		template <typename U, uint32_t ParamIndex>
		U& ParamPS() const
		{
			static_assert(std::is_same<T, struct PixelShader>::value, "ParamPS() requires ConstantGroup<PixelShader>");
			static_assert(ParamIndex < MAX_PS_CONSTANTS);

			return MapVar<U>(m_Shader->m_ConstantOffsets[ParamIndex]);
		}

		ConstantGroup<T>& operator=(const CustomConstantGroup& Other)
		{
			memcpy(&m_Map, &Other.m_Map, sizeof(m_Map));
			m_Buffer = Other.m_Buffer;
			m_Unified = Other.m_Unified;
			m_UnifiedByteOffset = Other.m_UnifiedByteOffset;
			m_Shader = nullptr;

			return *this;
		}
	};

	//
	// Shaders
	//
#pragma warning(push)
#pragma warning(disable: 4200)  // MSVC
#pragma warning(disable: 94)    // Intel C++ Compiler
	struct VertexShader
	{
		uint32_t m_TechniqueID;        // Bit flags
		ID3D11VertexShader* m_Shader;  // DirectX handle
		uint32_t m_ShaderLength;       // Raw bytecode length

		union
		{
			struct
			{
				Buffer m_PerTechnique;  // CONSTANT_GROUP_LEVEL_TECHNIQUE
				Buffer m_PerMaterial;   // CONSTANT_GROUP_LEVEL_MATERIAL
				Buffer m_PerGeometry;   // CONSTANT_GROUP_LEVEL_GEOMETRY
			};

			Buffer m_ConstantGroups[CONSTANT_GROUP_LEVEL_COUNT];
		};

		uint64_t m_VertexDescription;                 // ID3D11Device::CreateInputLayout lookup (for VSMain)
		uint8_t m_ConstantOffsets[MAX_VS_CONSTANTS];  // Actual offset is multiplied by 4
		uint8_t __padding[4];
		uint8_t m_RawBytecode[0];  // Raw bytecode
	};
	static_assert(sizeof(VertexShader) == 0x68);

	struct PixelShader
	{
		uint32_t m_TechniqueID;       // Bit flags
		ID3D11PixelShader* m_Shader;  // DirectX handle

		union
		{
			struct
			{
				Buffer m_PerTechnique;  // CONSTANT_GROUP_LEVEL_TECHNIQUE
				Buffer m_PerMaterial;   // CONSTANT_GROUP_LEVEL_MATERIAL
				Buffer m_PerGeometry;   // CONSTANT_GROUP_LEVEL_GEOMETRY
			};

			Buffer m_ConstantGroups[CONSTANT_GROUP_LEVEL_COUNT];
		};

		uint8_t m_ConstantOffsets[MAX_PS_CONSTANTS];  // Actual offset is multiplied by 4
	};
	static_assert(sizeof(PixelShader) == 0x80);

	struct ComputeShader
	{
		char _pad0[0x8];
		Buffer m_PerTechnique;  // CONSTANT_GROUP_LEVEL_TECHNIQUE
		char _pad1[0xC];
		Buffer m_PerMaterial;  // CONSTANT_GROUP_LEVEL_MATERIAL
		char _pad2[0xC];
		Buffer m_PerGeometry;  // CONSTANT_GROUP_LEVEL_GEOMETRY
		char _pad3[0x4];
		ID3D11ComputeShader* m_Shader;                // DirectX handle
		uint32_t m_TechniqueID;                       // Bit flags
		uint32_t m_ShaderLength;                      // Raw bytecode length
		uint8_t m_ConstantOffsets[MAX_CS_CONSTANTS];  // Actual offset is multiplied by 4
		uint8_t m_RawBytecode[0];                     // Raw bytecode
	};
	static_assert(sizeof(ComputeShader) == 0x90, "");

	// Not part of the vanilla game
	struct HullShader
	{
		uint32_t m_TechniqueID;      // Bit flags
		ID3D11HullShader* m_Shader;  // DirectX handle
	};

	// Not part of the vanilla game
	struct DomainShader
	{
		uint32_t m_TechniqueID;        // Bit flags
		ID3D11DomainShader* m_Shader;  // DirectX handle
	};

	using VertexCGroup = ConstantGroup<VertexShader>;
	using PixelCGroup = ConstantGroup<PixelShader>;
#pragma warning(pop)

	//
	// Renderer-specific types to handle uploading raw data to the GPU
	//
	struct LineShape
	{
		ID3D11Buffer* m_VertexBuffer;
		ID3D11Buffer* m_IndexBuffer;
		uint64_t m_VertexDesc;
		uint32_t m_RefCount;
	};
	static_assert(sizeof(LineShape) == 0x20);

	struct TriShape : LineShape
	{
		void* m_RawVertexData;
		void* m_RawIndexData;
	};
	static_assert(sizeof(TriShape) == 0x30);

	struct DynamicTriShape
	{
		ID3D11Buffer* m_VertexBuffer;
		ID3D11Buffer* m_IndexBuffer;
		uint64_t m_VertexDesc;
		uint32_t m_VertexAllocationOffset;
		uint32_t m_VertexAllocationSize;
		uint32_t m_RefCount;
		void* m_RawVertexData;
		void* m_RawIndexData;
	};
	static_assert(sizeof(DynamicTriShape) == 0x38);

	struct DynamicTriShapeData
	{
		ID3D11Buffer* m_VertexBuffer;
		ID3D11Buffer* m_IndexBuffer;
		uint64_t m_VertexDesc;
	};

	struct DynamicTriShapeDrawData
	{
		uint32_t m_Offset;
	};

	using TechniqueID = std::uint32_t;

	class BSShader :
		public RE::NiRefObject,          // 00
		public RE::NiBoneMatrixSetterI,  // 10
		public RE::BSReloadShaderI       // 18
	{
	public:
		template <class Key>
		struct TechniqueIDHasher
		{
		public:
			std::uint32_t operator()(const Key& a_key) const
			{
				return a_key->m_TechniqueID;
			}
		};

		template <class T>
		struct TechniqueIDCompare
		{
		public:
			bool operator()(const T& a_lhs, const T& a_rhs) const
			{
				return a_lhs->m_TechniqueID == a_rhs->m_TechniqueID;
			}
		};

		template <class Key>
		using TechniqueIDMap = RE::BSTSet<Key, TechniqueIDHasher<Key>, TechniqueIDCompare<Key>>;

		inline static constexpr auto RTTI = RE::RTTI_BSShader;

		~BSShader() override;  // 00

		// add
		virtual bool SetupTechnique(TechniqueID Technique) = 0;                                   // 02
		virtual void RestoreTechnique(TechniqueID Technique) = 0;                                 // 03
		virtual void SetupMaterial(RE::BSShaderMaterial const* Material);                         // 04
		virtual void RestoreMaterial(RE::BSShaderMaterial const* Material);                       // 05
		virtual void SetupGeometry(RE::BSRenderPass* Pass, uint32_t Flags) = 0;                   // 06
		virtual void RestoreGeometry(RE::BSRenderPass* Pass, uint32_t RenderFlags) = 0;           // 07
		virtual void GetTechniqueName(TechniqueID Technique, char* Buffer, uint32_t BufferSize);  // 08
		virtual void ReloadShaders(bool Unknown);                                                 // 09

		std::uint32_t m_Type;
		TechniqueIDMap<VertexShader*> m_VertexShaderTable;
		TechniqueIDMap<PixelShader*> m_PixelShaderTable;
		const char* m_LoaderType;
	};

	static_assert(sizeof(BSShader) == 0x90);
	static_assert(offsetof(BSShader, m_Type) == 0x20);
	static_assert(offsetof(BSShader, m_VertexShaderTable) == 0x28);
	static_assert(offsetof(BSShader, m_PixelShaderTable) == 0x58);
	static_assert(offsetof(BSShader, m_LoaderType) == 0x88);

	class RendererWindow
	{
	public:
		HWND hWnd;
		int iWindowX;
		int iWindowY;
		int uiWindowWidth;
		int uiWindowHeight;
		IDXGISwapChain* pSwapChain;
		uint32_t SwapChainRenderTarget;
		char _pad0[0x2C];
	};
	static_assert(sizeof(RendererWindow) == 0x50);

	class RendererData
	{
	public:
		char _pad0[0x22];
		bool bReadOnlyDepth;  // bReadOnlyStencil?
		char _pad1[0x15];
		ID3D11Device* pDevice;
		ID3D11DeviceContext* pContext;
		RendererWindow RenderWindowA[32];
		RenderTargetData pRenderTargets[RENDER_TARGET_COUNT];
		DepthStencilData pDepthStencils[DEPTH_STENCIL_COUNT];
		CubemapRenderTargetData pCubemapRenderTargets[RENDER_TARGET_CUBEMAP_COUNT];
		Texture3DTargetData pTexture3DRenderTargets[TEXTURE3D_COUNT];
		float ClearColor[4];
		uint8_t ClearStencil;
		CRITICAL_SECTION RendererLock;
	};

	template <typename T, uint32_t size = 1>
	class EYE_POSITION
	{
	public:
		/**
         * Get the eye of type T
		 *
         * @param a_index The index of the eye. By default it is set to 0. Default in SSE or Left in VR. 1 is Right.
		 * @throws std::out_of_range if index is greater than or equal to size.
         */
		T getEye(uint32_t a_index = 0)
		{
			if (a_index >= size)
				throw std::out_of_range("Index for eye is out of range");
			return eye[a_index];
		}

	private:
		T eye[size];  // default or left eye is index 0, right eye is index 1
	};

	class RendererShadowState
	{
	public:
		struct RUNTIME_DATA2
		{
#define RUNTIME_DATA2_CONTENT                                                               \
	EYE_POSITION<RE::NiPoint3> m_PosAdjust;         /* 35c, VR 3A4 */                       \
	EYE_POSITION<RE::NiPoint3> m_PreviousPosAdjust; /* 368, VR 3BC */                       \
	EYE_POSITION<ViewData> m_CameraData;            /* 374, VR 3E0 - size of each is 250 */ \
	uint32_t m_AlphaBlendModeExtra;                 /* 5c4, VR 880 */                       \
	float unk5c8;                                   /* 5c8, VR 884 */                       \
	float unk5cc;                                   /* 5cc VR 888 */                        \
	uint32_t unk5d0;                                /* 5d0 VR 88c */
			RUNTIME_DATA2_CONTENT;
		};
		static_assert(sizeof(RUNTIME_DATA2) == 0x280);
		static_assert(offsetof(RUNTIME_DATA2, m_PosAdjust) == 0);
		static_assert(offsetof(RUNTIME_DATA2, m_PreviousPosAdjust) == 0xc);
		static_assert(offsetof(RUNTIME_DATA2, m_CameraData) == 0x20);

		struct VR_RUNTIME_DATA2
		{
#define VR_RUNTIME_DATA2_CONTENT                                                               \
	EYE_POSITION<RE::NiPoint3, 2> m_PosAdjust;         /* 35c, VR 3A4 */                       \
	EYE_POSITION<RE::NiPoint3, 2> m_PreviousPosAdjust; /* 368, VR 3BC */                       \
	uint8_t unk3d4[0x3e0 - 0x3d4];                     /* 3d4, VR only pad */                  \
	EYE_POSITION<ViewData, 2> m_CameraData;            /* 374, VR 3E0 - size of each is 250 */ \
	uint32_t m_AlphaBlendModeExtra;                    /* 5c4, VR 880 */                       \
	float unk5c8;                                      /* 5c8, VR 884 */                       \
	float unk5cc;                                      /* 5cc VR 888 */                        \
	uint32_t unk5d0;                                   /* 5d0 VR 88c */                        \
	ID3D11Buffer* VSConstantBuffers[12];               /* VR 890 only */                       \
	ID3D11Buffer* PSConstantBuffers[12];               /* VR 8F0 only */
			VR_RUNTIME_DATA2_CONTENT;
		};
		static_assert(sizeof(VR_RUNTIME_DATA2) == 0x5b0);
		static_assert(offsetof(VR_RUNTIME_DATA2, m_PosAdjust) == 0);
		static_assert(offsetof(VR_RUNTIME_DATA2, m_PreviousPosAdjust) == 0x18);
		static_assert(offsetof(VR_RUNTIME_DATA2, m_CameraData) == 0x40);
		static_assert(offsetof(VR_RUNTIME_DATA2, VSConstantBuffers) == 0x4f0);

		struct RUNTIME_DATA
		{
#ifndef ENABLE_SKYRIM_VR
#	define RUNTIME_DATA_CONTENT                                                        \
		RUNTIME_DATA2_CONTENT;               /* 35c, VR 3A4 */
#elif !defined(ENABLE_SKYRIM_AE) && !defined(ENABLE_SKYRIM_SE)
#	define RUNTIME_DATA_CONTENT                                                        \
		VR_RUNTIME_DATA2_CONTENT;            /* 35c, VR 3A4 */
#else
#	define RUNTIME_DATA_CONTENT
#endif
			uint64_t m_VertexDesc;               /* 340 doesn't fit in SSE, VR 388 only? */
			VertexShader* m_CurrentVertexShader; /* 348, VR 390 */
			PixelShader* m_CurrentPixelShader;   /* 350, VR 398 */
			D3D11_PRIMITIVE_TOPOLOGY m_Topology; /* 358, VR 3A0 */                          
			RUNTIME_DATA_CONTENT;
		};
#ifndef ENABLE_SKYRIM_VR
		static_assert(sizeof(RUNTIME_DATA) == 0x2a0);
		static_assert(offsetof(RUNTIME_DATA, m_Topology) == 0x18);
		static_assert(offsetof(RUNTIME_DATA, m_PosAdjust) == 0x1c);
		static_assert(offsetof(RUNTIME_DATA, m_PreviousPosAdjust) == 0x28);
		static_assert(offsetof(RUNTIME_DATA, m_CameraData) == 0x40);
#elif !defined(ENABLE_SKYRIM_AE) && !defined(ENABLE_SKYRIM_SE)
		static_assert(sizeof(RUNTIME_DATA) == 0x5d0);
		static_assert(offsetof(RUNTIME_DATA, m_Topology) == 0x18);
		static_assert(offsetof(RUNTIME_DATA, m_PosAdjust) == 0x1c);
		static_assert(offsetof(RUNTIME_DATA, m_PreviousPosAdjust) == 0x34);
		static_assert(offsetof(RUNTIME_DATA, m_CameraData) == 0x60);
		static_assert(offsetof(RUNTIME_DATA, VSConstantBuffers) == 0x510);
#else
		static_assert(sizeof(RUNTIME_DATA) == 0x20);
		static_assert(offsetof(RUNTIME_DATA, m_Topology) == 0x18);
#endif

		ShaderFlags m_StateUpdateFlags;     // 00 Flags +0x0  0xFFFFFFFF; global state updates
		uint32_t m_PSResourceModifiedBits;  // 04 Flags +0x4  0xFFFF
		uint32_t m_PSSamplerModifiedBits;   // 08 Flags +0x8  0xFFFF
		uint32_t m_CSResourceModifiedBits;  // 0c Flags +0xC  0xFFFF
		uint32_t m_CSSamplerModifiedBits;   // 10 Flags +0x10 0xFFFF
		uint32_t m_CSUAVModifiedBits;       // 14 Flags +0x14 0xFF
		uint32_t m_OMUAVModifiedBits;       // 18 Flags +0x18 0xFF
		uint32_t m_SRVModifiedBits;         // 1c Flags +0x1C 0xFF

		uint32_t m_RenderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];  // 20
		uint32_t m_DepthStencil;                                           // 40 - Index
		uint32_t m_DepthStencilSlice;                                      // 44 Index
		uint32_t m_CubeMapRenderTarget;                                    // 48 = Index
		uint32_t m_CubeMapRenderTargetView;                                // 4c Index

		SetRenderTargetMode m_SetRenderTargetMode[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];  // 50
		SetRenderTargetMode m_SetDepthStencilMode;                                          // 70
		SetRenderTargetMode m_SetCubeMapRenderTargetMode;                                   // 74

		D3D11_VIEWPORT m_ViewPort;  // 78

		DepthStencilDepthMode m_DepthStencilDepthMode;          // 90
		DepthStencilDepthMode m_DepthStencilDepthModePrevious;  // 94 - also some kind of mode
		uint32_t m_DepthStencilStencilMode;                     // 98
		uint32_t m_StencilRef;                                  // 9c

		uint32_t m_RasterStateFillMode;       // a0
		uint32_t m_RasterStateCullMode;       // a4
		uint32_t m_RasterStateDepthBiasMode;  // a8
		uint32_t m_RasterStateScissorMode;    // ac

		uint32_t m_AlphaBlendMode;             // b0
		uint32_t m_AlphaBlendAlphaToCoverage;  // b4
		uint32_t m_AlphaBlendWriteMode;        // b8

		bool m_AlphaTestEnabled;  // BC
		float m_AlphaTestRef;     // C0

		uint32_t m_PSTextureAddressMode[16];        // c4
		uint32_t m_PSTextureFilterMode[16];         // 104
		uint32_t unk144;                            // 144
		ID3D11ShaderResourceView* m_PSTexture[16];  // 148

		uint32_t m_CSTextureAddressMode[16];  // 1c8
		uint32_t m_CSTextureFilterMode[16];   // 208

		ID3D11ShaderResourceView* m_CSTexture[16];  // 248
		uint32_t m_CSTextureMinLodMode[16];         // 2C8
		ID3D11UnorderedAccessView* m_CSUAV[8];      // 308

		RUNTIME_DATA_CONTENT;  // 340

		[[nodiscard]] inline RUNTIME_DATA& GetRuntimeData() noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, 0x340, 0x388);
		}

		[[nodiscard]] inline const RUNTIME_DATA& GetRuntimeData() const noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, 0x340, 0x388);
		}

		[[nodiscard]] inline RUNTIME_DATA2& GetRuntimeData2() noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA2>(this, 0x35c, 0x3a4);
		}

		[[nodiscard]] inline const RUNTIME_DATA2& GetRuntimeData2() const noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA2>(this, 0x35c, 0x3a4);
		}

		[[nodiscard]] inline VR_RUNTIME_DATA2& GetVRRuntimeData2() noexcept
		{
			return REL::RelocateMember<VR_RUNTIME_DATA2>(this, 0x35c, 0x3a4);
		}

		[[nodiscard]] inline const VR_RUNTIME_DATA2& GetVRRuntimeData2() const noexcept
		{
			return REL::RelocateMember<VR_RUNTIME_DATA2>(this, 0x35c, 0x3a4);
		}

		static RendererShadowState* QInstance();
	};
#undef RUNTIME_DATA_CONTENT
#undef RUNTIME_DATA2_CONTENT
#undef VR_RUNTIME_DATA2_CONTENT

	class Renderer
	{
	public:
		char _pad0[0x10];
		char _pad1[0x22];
		bool bReadOnlyDepth;  // bReadOnlyStencil?
		char _pad2[0x15];
		ID3D11Device* pDevice;
		ID3D11DeviceContext* pContext;
		RendererWindow RenderWindowA[32];
		RenderTargetData pRenderTargets[RENDER_TARGET_COUNT];
		DepthStencilData pDepthStencils[DEPTH_STENCIL_COUNT];
		CubemapRenderTargetData pCubemapRenderTargets[RENDER_TARGET_CUBEMAP_COUNT];
		Texture3DTargetData pTexture3DRenderTargets[TEXTURE3D_COUNT];
		float ClearColor[4];
		uint8_t ClearStencil;
		CRITICAL_SECTION RendererLock;

		static Renderer* QInstance();
	};

	class SceneState
	{
	public:
		RE::ShadowSceneNode* pShadowSceneNode[4];
		float fTimerValues[TIMER_MODE_COUNT];
		RE::NiColorA LoadedRange;
		bool bInterior;
		bool bLiteBrite;
		bool CharacterLightEnabled;
		char _pad0[0x51];
		float fLandLOFadeSeconds;
		float fInvFrameBufferRange;
		float fLeafAnimDampenDistStartSPU;
		float fLeafAnimDampenDistEndSPU;
		RE::NiPoint2 kOldGridArrayCenter;
		RE::NiPoint2 kGridArrayCenter;
		float kfGriddArrayLerpStart;
		uint32_t uiCurrentShaderTechnique;
		uint8_t cSceneGraph;
		uint32_t usDebugMode;
		RE::NiTransform DirectionalAmbientTransform;
		RE::NiColorA AmbientSpecular;
		RE::NiColorA CharacterLightParams;  // { Primary, Secondary, Luminance, Max Luminance }
		bool bAmbientSpecularEnabled;
		uint32_t uiTextureTransformCurrentBuffer;
		uint32_t uiTextureTransformFlipMode;
		uint32_t uiCameraInWaterState;
		RE::NiBound kCachedPlayerBound;
		char _pad1[0x8];
		float fWaterIntersect;

		static SceneState* QInstance();
	};

	class NiAccumulator : public RE::NiObject
	{
	public:
		virtual void StartAccumulating(RE::NiCamera const*);
		virtual void FinishAccumulating();
		virtual void RegisterObjectArray(/*NiVisibleArray &*/);
		virtual void Unknown0();
		virtual void Unknown1();

		const RE::NiCamera* m_pkCamera;
	};
	static_assert(sizeof(NiAccumulator) == 0x18, "");

	class NiBackToFrontAccumulator : public NiAccumulator
	{
	public:
		virtual ~NiBackToFrontAccumulator();

		char _pad[0x18];  // NiTPointerList<BSGeometry *> m_kItems;
		int m_iNumItems;
		int m_iMaxItems;
		RE::BSGeometry** m_ppkItems;
		float* m_pfDepths;
		int m_iCurrItem;
	};
	static_assert(sizeof(NiBackToFrontAccumulator) == 0x50, "");

	class NiAlphaAccumulator : public NiBackToFrontAccumulator
	{
	public:
		virtual ~NiAlphaAccumulator();

		bool m_bObserveNoSortHint;
		bool m_bSortByClosestPoint;
		bool m_bInterfaceSort;
	};
	static_assert(sizeof(NiAlphaAccumulator) == 0x58, "");

	class BSShaderAccumulator : public NiAlphaAccumulator
	{
	public:
		typedef bool (*REGISTEROBJECTFUNC)(BSShaderAccumulator* Accumulator, RE::BSGeometry* Geometry, RE::BSShaderProperty* Property, void* Unknown);
		typedef void (*FINISHACCUMULATINGFUNC)(BSShaderAccumulator* Accumulator, uint32_t Flags);

		virtual ~BSShaderAccumulator();
		virtual void StartAccumulating(RE::NiCamera const*) override;
		virtual void FinishAccumulatingDispatch(uint32_t RenderFlags);
		struct RUNTIME_DATA
		{
#define RUNTIME_DATA_CONTENT                      \
	RE::BSBatchRenderer* m_BatchRenderer;         \
	uint32_t m_CurrentPass;                       \
	uint32_t m_CurrentBucket;                     \
	bool m_CurrentActive;                         \
	char _pad[0x7];                               \
	RE::ShadowSceneNode* m_ActiveShadowSceneNode; \
	uint32_t m_RenderMode;                        \
	char _pad2[0x18];                             \
	RE::NiPoint3 m_EyePosition;                   \
	char _pad3[0x8];
			RUNTIME_DATA_CONTENT
		};
		static_assert(sizeof(RUNTIME_DATA) == 0x50);
		static_assert(offsetof(RUNTIME_DATA, m_BatchRenderer) == 0);
		static_assert(offsetof(RUNTIME_DATA, m_ActiveShadowSceneNode) == 0x18);

		[[nodiscard]] inline RUNTIME_DATA& GetRuntimeData() noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, 0x130, 0x158);
		}

		[[nodiscard]] inline const RUNTIME_DATA& GetRuntimeData() const noexcept
		{
			return REL::RelocateMember<RUNTIME_DATA>(this, 0x148, 0x170);
		}
		static BSShaderAccumulator* GetCurrentAccumulator();

		//members
		char _pad1[0xD0];
		bool m_1stPerson;   // 128
		char _pad0[0x3];    // 129
		bool m_DrawDecals;  // 130
#ifndef ENABLE_SKYRIM_VR
		RUNTIME_DATA_CONTENT;  // 130
#elif !defined(ENABLE_SKYRIM_AE) && !defined(ENABLE_SKYRIM_SE)
		std::uint64_t unk000[(0x158 - 0x130) >> 3];  // 130
		RUNTIME_DATA_CONTENT;                        // 158
#endif
	};
#ifndef ENABLE_SKYRIM_VR
	static_assert(sizeof(BSShaderAccumulator) == 0x180);
	static_assert(offsetof(BSShaderAccumulator, m_BatchRenderer) == 0x130);
	static_assert(offsetof(BSShaderAccumulator, m_ActiveShadowSceneNode) == 0x148);
#elif !defined(ENABLE_SKYRIM_AE) && !defined(ENABLE_SKYRIM_SE)
	static_assert(offsetof(BSShaderAccumulator, m_BatchRenderer) == 0x158);
	static_assert(offsetof(BSShaderAccumulator, m_ActiveShadowSceneNode) == 0x170);
#endif
#undef RUNTIME_DATA_CONTENT

	class TESImagespaceManager
	{
	public:
		float pad0[42];
		RE::ImageSpaceBaseData* baseData0;  //a8
		RE::ImageSpaceBaseData* baseData1;  //b0
		float pad1;
		float pad2;
		float pad3;
		float pad4;
		RE::ImageSpaceBaseData hdrData;  // c8

		static TESImagespaceManager* GetSingleton()
		{
			REL::Relocation<TESImagespaceManager**> singleton{ REL::RelocationID(527731, 414660) };
			return *singleton;
		}
	};
}
