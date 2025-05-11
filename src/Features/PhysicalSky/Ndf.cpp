#include "Features/PhysicalSky.h"

ID3D11ShaderResourceView* FileNdfProvider::GetNdf()
{
	return tex_manager->Query(tex_path);
}

void FileNdfProvider::DrawUi()
{
	if (ImGui::BeginCombo("Texture", tex_path.c_str())) {
		for (auto& path_choice : tex_manager->ListPaths())
			if (ImGui::Selectable(path_choice.c_str(), path_choice == tex_path))
				tex_path = path_choice;
		ImGui::EndCombo();
	}
}