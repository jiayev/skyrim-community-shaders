#pragma once
namespace EnvironmentProbe
{
	struct Request;
	namespace Shadows
	{
		struct alignas(16) Parameters
		{
			DirectX::XMFLOAT4X4 projection{};
			float depthBias = 0;
			uint32_t enabled = 0;
			float padding[2]{};
		};
		void InstallHooks();
		void Render(const Request& request);
		void Reset();
		bool StaticCaster(std::uint64_t flags, bool alphaBlending, bool alphaTesting);
		bool DynamicBsx(std::uint32_t flags);
		void EnsureTarget();
		ID3D11Texture2D* Target();
		ID3D11DepthStencilView* DepthView();
		RE::NiCamera* PrepareDclf();
		void DclfSubmitted();
		void BeforeDraw();
		void AfterDraw();
		bool RejectGeometry(RE::BSCullingProcess* process, RE::BSGeometry& geometry);
		ID3D11ShaderResourceView* Map();
		Parameters Data();
	}
}
