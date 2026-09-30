#pragma once

namespace SkinTextures
{
	struct Resource
	{
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		uint64_t fingerprint = 0;
		bool reconstructNormal = false;
		std::string error;
	};
	Resource Load(const std::string& a_path);
	void Validate(const std::string& a_path);
}
