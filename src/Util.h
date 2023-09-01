#pragma once

namespace Util
{
	struct UnkOuterStruct
	{
		struct UnkInnerStruct
		{
			uint8_t unk00[0x18];  // 00
			bool bTAA;            // 18
		};

		// members
		uint8_t unk00[0x1F0];            // 00
		UnkInnerStruct* unkInnerStruct;  // 1F0

		static UnkOuterStruct* GetSingleton()
		{
			REL::Relocation<UnkOuterStruct*&> instance{ REL::VariantID(527731, 414660, 0x34234C0) };  // 31D11A0, 326B280, 34234C0
			return instance.get();
		}

		bool GetTAA() const
		{
			if (this == nullptr)
				return false;
			return unkInnerStruct->bTAA;
		}

		void SetTAA(bool a_enabled)
		{
			if (this == nullptr)
				return;
			unkInnerStruct->bTAA = a_enabled;
		}
	};

	void StoreTransform3x4NoScale(DirectX::XMFLOAT3X4& Dest, const RE::NiTransform& Source);
	ID3D11ShaderResourceView* GetSRVFromRTV(ID3D11RenderTargetView* a_rtv);
	ID3D11RenderTargetView* GetRTVFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromSRV(ID3D11ShaderResourceView* a_srv);
	std::string GetNameFromRTV(ID3D11RenderTargetView* a_rtv);
	void SetResourceName(ID3D11DeviceChild* Resource, const char* Format, ...);
	ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const std::vector<std::pair<const char*, const char*>>& Defines, const char* ProgramType, const char* Program = "main");
	std::string DefinesToString(std::vector<std::pair<const char*, const char*>>& defines);
	std::string DefinesToString(std::vector<D3D_SHADER_MACRO>& defines);
}
