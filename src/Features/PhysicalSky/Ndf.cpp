#include "Features/PhysicalSky.h"

template <class... Ts>
struct overloads : Ts...
{
	using Ts::operator()...;
};

const char* NdfManager::GetSettingsTypeName(const NdfSettings& ndf_settings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) { return "Texture"; }
	};

	return std::visit(visitor, ndf_settings);
}

void NdfManager::DrawNdfSettings(NdfSettings& ndf_settings, TextureManager& tex_manager)
{
	// ndf_selector
	const static auto types = []() {
		std::vector<std::pair<std::string, NdfSettings>> retval = {
			{ "", TexNdfSettings() },
		};
		for (auto& [name, s] : retval)
			name = GetSettingsTypeName(s);
		return retval;
	}();

	if (ImGui::BeginCombo("Cloud Map Generator", GetSettingsTypeName(ndf_settings))) {
		for (auto& [name, s] : types)
			if (ImGui::Selectable(name.c_str(), false)) {
				ndf_settings = s;
				break;
			}
		ImGui::EndCombo();
	}

	ImGui::Separator();

	// ndf editor
	auto visitor = overloads{
		[&](TexNdfSettings& s) {
			if (ImGui::BeginCombo("Texture Path", s.tex_path.c_str())) {
				for (auto& path_choice : tex_manager.ListPaths())
					if (ImGui::Selectable(path_choice.c_str(), path_choice == s.tex_path))
						s.tex_path = path_choice;
				ImGui::EndCombo();
			}

			if (!tex_manager.Query(s.tex_path))
				ImGui::TextColored({ 1, 0, 0, 1 }, "Failed to load texture.");
		}
	};
	std::visit(visitor, ndf_settings);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(NdfSettings& ndf_settings, TextureManager& tex_manager)
{
	auto visitor = overloads{
		[&](TexNdfSettings& s) { return tex_manager.Query(s.tex_path); }
	};

	return std::visit(visitor, ndf_settings);
}