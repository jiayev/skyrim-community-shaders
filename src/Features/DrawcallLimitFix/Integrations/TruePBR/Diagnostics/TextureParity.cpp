// TruePBR's texture parity (see the header), CaptureParity's. Render thread.

#include "Features/DrawcallLimitFix/Integrations/TruePBR/Diagnostics/TextureParity.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"
#include "Features/DrawcallLimitFix/Scene/Model/Tables.h"

#include <algorithm>
#include <array>

#include <d3d11.h>

#include "Globals.h"
#include "TruePBR.h"

namespace DCLF::Integrations::TruePBR
{
	void DrawTextureParity::Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
		NoteFn a_note, void* a_owner)
	{
		if (!globals::features::truePBR.loaded)
			return;
		const auto& object = a_tables.objects[a_objectIndex];
		if ((object.flags & kObjectNoBindings) || object.materialIndex >= a_tables.materials.size())
			return;
		// t80-t91, bound by State::Draw from TruePBR's landscape hook (other materials leave the previous draw's: not compared).
		const auto& features = a_tables.materials[object.materialIndex].featureTextures;
		const auto first = features.begin() + Extensions::FeatureTextureSlotOf(kTextureRegisters.front());
		const auto last = first + kTextureRegisters.size();
		if (std::none_of(first, last, [](const auto* a_view) { return a_view != nullptr; }))
			return;
		std::array<ID3D11ShaderResourceView*, ::TruePBR::NumExtendedTextures> native{};
		a_context->PSGetShaderResources(::TruePBR::FirstExtendedTexture, ::TruePBR::NumExtendedTextures, native.data());
		++checks;
		for (std::uint32_t i = 0; i < ::TruePBR::NumExtendedTextures; ++i) {
			if (native[i] != first[i]) {
				++mismatches;
				a_note(a_owner, a_geometry, fmt::format("TruePBR texture t{}: DCLF {}, native {}", ::TruePBR::FirstExtendedTexture + i, fmt::ptr(first[i]),
					fmt::ptr(native[i])));
				break;
			}
		}
		for (auto* bound : native) {
			if (bound)
				bound->Release();
		}
	}

	void DrawTextureParity::Report() const
	{
		logger::info("[DCLF] TruePBR texture parity {}: {} landscape draws' t80-t91 checked, {} differ", mismatches == 0 ? "OK" : "MISMATCH",
			checks, mismatches);
	}
}
