// Advanced Skin's t71/t74 parity (see the header). Render thread.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/TextureParity.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"

#include <array>

#include "Features/Skin.h"
#include "Globals.h"

namespace DCLF::Integrations::AdvancedSkin
{
	bool Loaded()
	{
		return globals::features::skin.loaded;
	}

	void DrawSkinTextureParity::Compare(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex,
		const SceneTables& a_tables, NoteFn a_note, void* a_owner)
	{
		constexpr std::size_t kTextures = kMaterialTextureRegisters.size();
		const auto& object = a_tables.objects[a_objectIndex];

		// Per-mesh controls, wetness and detail normal, bound by State::Draw after SetupGeometry.
		if (!(object.flags & kObjectNoBindings) && object.materialIndex < a_tables.materials.size()) {
			const auto expected = CapturedTextures(a_objectIndex);
			if (expected[0] || expected[1] || expected[2]) {
				std::array<ID3D11ShaderResourceView*, kTextures> native{};
				for (std::size_t f = 0; f < kTextures; ++f)
					a_context->PSGetShaderResources(kMaterialTextureRegisters[f], 1, &native[f]);
				++checks;
				if (native != expected) {
					++mismatches;
					a_note(a_owner, a_geometry, fmt::format("skin textures: DCLF t71 {} t74 {} t72 {}, native t71 {} t74 {} t72 {}", fmt::ptr(expected[0]), fmt::ptr(expected[1]), fmt::ptr(expected[2]), fmt::ptr(native[0]), fmt::ptr(native[1]), fmt::ptr(native[2])));
				}
				for (auto* bound : native) {
					if (bound)
						bound->Release();
				}
			}
		}
	}
}
