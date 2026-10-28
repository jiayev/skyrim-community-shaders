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
		void BeforeDraw();
		void AfterDraw();
		bool RejectGeometry(RE::BSCullingProcess* process, RE::BSGeometry& geometry);
		ID3D11ShaderResourceView* Map();
		Parameters Data();
	}
}
