#pragma once
#include "DirectXMath.h"
#include <d3d11.h>

namespace BSGraphics
{
	struct alignas(16) ViewData
	{
		DirectX::XMVECTOR m_ViewUp;
		DirectX::XMVECTOR m_ViewRight;
		DirectX::XMVECTOR m_ViewDir;
		DirectX::XMMATRIX m_ViewMat;
		DirectX::XMMATRIX m_ProjMat;
		DirectX::XMMATRIX m_ViewProjMat;
		DirectX::XMMATRIX m_UnknownMat1;
		DirectX::XMMATRIX m_ViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_PreviousViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_ProjMatrixUnjittered;
		DirectX::XMMATRIX m_UnknownMat2;
		float m_ViewPort[4];  // NiRect<float> { left = 0, right = 1, top = 1, bottom = 0 }
		RE::NiPoint2 m_ViewDepthRange;
		char _pad0[0x8];
	};
	static_assert(sizeof(ViewData) == 0x250);

	struct CameraStateData
	{
		RE::NiCamera* pReferenceCamera;
		ViewData CamViewData;
		RE::NiPoint3 PosAdjust;
		RE::NiPoint3 CurrentPosAdjust;
		RE::NiPoint3 PreviousPosAdjust;
		bool UseJitter;
		char _pad0[0x8];
	};
	static_assert(sizeof(CameraStateData) == 0x290);

	struct State
	{
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNoiseMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjDiffuseMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNormalMap;
		RE::NiPointer<RE::NiSourceTexture> pDefaultTextureProjNormalDetailMap;
		char _pad0[0x1C];
		float unknown[2];
		float jitter[2];

		//unsure whether the following haven't moved in AE, couldn't find any place where they're used
		uint32_t uiFrameCount;
		bool bInsideFrame;
		bool bLetterbox;
		bool bUnknown1;
		bool bCompiledShaderThisFrame;
		bool bUseEarlyZ;
		// ...

		// somewhere in the middle of this struct, there are bytes in AE that are not present in SE
		// therefore use GetRuntimeData to get the rest

		struct RUNTIME_DATA
		{
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureBlack;  // "BSShader_DefHeightMap"
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureWhite;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureGrey;
			RE::NiPointer<RE::NiSourceTexture> pDefaultHeightMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultReflectionCubeMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultFaceDetailMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTexEffectMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureNormalMap;
			RE::NiPointer<RE::NiSourceTexture> pDefaultTextureDitherNoiseMap;
			RE::BSTArray<CameraStateData> kCameraDataCacheA;
			float _pad2;                  // unknown dword
			float fHaltonSequence[2][8];  // (2, 3) Halton Sequence points
			float fDynamicResolutionCurrentWidthScale;
			float fDynamicResolutionCurrentHeightScale;
			float fDynamicResolutionPreviousWidthScale;
			float fDynamicResolutionPreviousHeightScale;
			float fDynamicResolutionWidthRatio;
			float fDynamicResolutionHeightRatio;
			uint16_t usDynamicResolutionCounter;
		};
		CameraStateData* FindCameraDataCache(const RE::NiCamera* Camera, bool UseJitter);

		static_assert(offsetof(RUNTIME_DATA, fDynamicResolutionCurrentWidthScale) == 0xA4);

		[[nodiscard]] RUNTIME_DATA& GetRuntimeData() noexcept
		{
			return REL::RelocateMemberIfNewer<RUNTIME_DATA>(SKSE::RUNTIME_SSE_1_6_317, this, 0x58, 0x60);
		}

		[[nodiscard]] inline const RUNTIME_DATA& GetRuntimeData() const noexcept
		{
			return REL::RelocateMemberIfNewer<RUNTIME_DATA>(SKSE::RUNTIME_SSE_1_6_317, this, 0x58, 0x60);
		}
	};
}
