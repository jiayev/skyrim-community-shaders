#pragma once

namespace EnvironmentProbe
{
	/** Material-independent octant distribution, specialized to each VS output signature. */
	class GeometryShaders
	{
	public:
		ID3D11GeometryShader* Get(const RE::BSGraphics::VertexShader& vertex);
		void Clear();

	private:
		struct VertexEntry
		{
			winrt::com_ptr<ID3D11VertexShader> owner;
			ID3D11GeometryShader* geometry;
		};
		std::unordered_map<ID3D11VertexShader*, VertexEntry> vertices;
		std::unordered_map<std::string, winrt::com_ptr<ID3D11GeometryShader>> signatures;
	};
}
