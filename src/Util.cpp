#include "Util.h"

namespace Util
{
	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source)
	{
		//
		// Shove a Matrix3+Point3 directly into a float[3][4] with no modifications
		//
		// Dest[0][#] = Source.m_Rotate.m_pEntry[0][#];
		// Dest[0][3] = Source.m_Translate.x;
		// Dest[1][#] = Source.m_Rotate.m_pEntry[1][#];
		// Dest[1][3] = Source.m_Translate.x;
		// Dest[2][#] = Source.m_Rotate.m_pEntry[2][#];
		// Dest[2][3] = Source.m_Translate.x;
		//
		static_assert(sizeof(RE::NiTransform::rotate) == 3 * 3 * sizeof(float));  // NiMatrix3
		static_assert(sizeof(RE::NiTransform::translate) == 3 * sizeof(float));   // NiPoint3
		static_assert(offsetof(RE::NiTransform, translate) > offsetof(RE::NiTransform, rotate));

		_mm_store_ps(Dest.m[0], _mm_loadu_ps(Source.rotate.entry[0]));
		_mm_store_ps(Dest.m[1], _mm_loadu_ps(Source.rotate.entry[1]));
		_mm_store_ps(Dest.m[2], _mm_loadu_ps(Source.rotate.entry[2]));
		Dest.m[0][3] = Source.translate.x;
		Dest.m[1][3] = Source.translate.y;
		Dest.m[2][3] = Source.translate.z;
	}

	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv)
	{
		if (a_rtv) {
			if (auto r = RE::BSGraphics::Renderer::GetSingleton()) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return rt.SRV;
					}
				}
			}
		}
		return nullptr;
	}

	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv)
	{
		if (a_srv) {
			if (auto r = RE::BSGraphics::Renderer::GetSingleton()) {
				for (int i = 0; i < RE::RENDER_TARGETS::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return rt.RTV;
					}
				}
			}
		}
		return nullptr;
	}

	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;
 
		if (a_srv) {
			if (auto r = RE::BSGraphics::Renderer::GetSingleton()) {
				for (int i = 0; i < RENDER_TARGET::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_srv == rt.SRV || a_srv == rt.SRVCopy) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv)
	{
		using RENDER_TARGET = RE::RENDER_TARGETS::RENDER_TARGET;
		if (a_rtv) {
			if (auto r = RE::BSGraphics::Renderer::GetSingleton()) {
				for (int i = 0; i < RENDER_TARGET::kTOTAL; i++) {
					auto rt = r->GetRuntimeData().renderTargets[i];
					if (a_rtv == rt.RTV) {
						return std::string(magic_enum::enum_name(static_cast<RENDER_TARGET>(i)));
					}
				}
			}
		}
		return "NONE";
	}

	GUID WKPDID_D3DDebugObjectNameT = { 0x429b8c22, 0x9188, 0x4b0c, 0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00 };

	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...)
	{
		if (!Resource)
			return;

		char buffer[1024];
		va_list va;

		va_start(va, Format);
		int len = _vsnprintf_s(buffer, _TRUNCATE, Format, va);
		va_end(va);

		Resource->SetPrivateData(WKPDID_D3DDebugObjectNameT, len, buffer);
	}
}