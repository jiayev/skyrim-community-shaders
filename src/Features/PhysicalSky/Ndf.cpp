#include "Features/PhysicalSky.h"

template <class... Ts>
struct overloads : Ts...
{
	using Ts::operator()...;
};

void NdfManager::SetupResources()
{
	logger::debug("Creating NDF resources...");
	{
		D3D11_TEXTURE2D_DESC tex_desc{
			.Width = s_ndf_dim,
			.Height = s_ndf_dim,
			.MipLevels = 1,
			.ArraySize = 5,
			.Format = DXGI_FORMAT_R8_UNORM,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = 5 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 5 }
		};

		tex_ndf_output = eastl::make_unique<Texture2D>(tex_desc);
		tex_ndf_output->CreateSRV(srv_desc);
		tex_ndf_output->CreateUAV(uav_desc);
	}

	CompileShaders();
}

void NdfManager::CompileShaders()
{
	logger::debug("Compiling NDF shaders...");

	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PhysicalSky\\NdfCumuliform.cs.hlsl", {}, "cs_5_0")))
		cumuliform_program.attach(rawPtr);
}

const char* NdfManager::GetSettingsTypeName(const NdfSettings& ndf_settings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) { return "Texture"; },
		[&](const CumuliformNdfSettings&) { return "Cumuliform"; }
	};

	return std::visit(visitor, ndf_settings);
}

const char* NdfManager::GetSettingsHint(const NdfSettings& ndf_settings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) {
			return "Read the cloud map from dds textures. More static but you can draw arbitrary shapes.\n"
				   "The texture should be a Texture2DArray consists of 5 grayscale images:\n"
				   "1. min height\n"
				   "2. max height\n"
				   "3. coverage\n"
				   "4. bottom type\n"
				   "5. top type";
		},
		[&](const CumuliformNdfSettings&) {
			return "A versatile cloud map generator that gets you from billowy cumulus to thick stratus sheets.";
		}
	};

	return std::visit(visitor, ndf_settings);
}

void NdfManager::DrawNdfSettings(NdfSettings& ndf_settings, TextureManager& tex_manager)
{
	// ndf_selector
	const static auto types = []() {
		std::vector<std::pair<std::string, NdfSettings>> retval = {
			{ "", TexNdfSettings() },
			{ "", CumuliformNdfSettings() },
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
	if (ImGui::BeginTable("TexManagers", 1, ImGuiTableFlags_BordersOuter, { -FLT_MIN, 0 })) {
		ImGui::TableNextColumn();
		ImGui::TextWrapped(GetSettingsHint(ndf_settings));
		ImGui::EndTable();
	}

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
		},
		[&](auto&) {}
	};
	std::visit(visitor, ndf_settings);
}

void NdfManager::UpdateNdf(const NdfSettings& ndf_settings)
{
	auto visitor = overloads{
		[&](const TexNdfSettings&) {},
		[&](const CumuliformNdfSettings&) {
			auto context = globals::d3d::context;

			auto uav = tex_ndf_output->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(cumuliform_program.get(), nullptr, 0);
			context->Dispatch((s_ndf_dim + 7) >> 3, (s_ndf_dim + 7) >> 3, 1);

			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	};
	std::visit(visitor, ndf_settings);
}

ID3D11ShaderResourceView* NdfManager::GetNdf(const NdfSettings& ndf_settings, TextureManager& tex_manager)
{
	auto visitor = overloads{
		[&](const TexNdfSettings& s) { return tex_manager.Query(s.tex_path); },
		[&](const auto&) { return tex_ndf_output->srv.get(); },
	};
	return std::visit(visitor, ndf_settings);
}