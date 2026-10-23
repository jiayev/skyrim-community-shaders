// Advanced Skin's wetness parities (see the header). Render thread.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/WetnessParity.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include "Features/Skin.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		// The persistent parity's since the shading line's last take (render thread: the check and the report).
		std::uint64_t rowWetness = 0, rowWetnessDiffer = 0;
		std::string rowWetnessFirst;

		const std::vector<std::uint32_t>& Members(const SceneTables& a_tables)
		{
			return a_tables.classMembers[static_cast<std::size_t>(Api::EntryClass::ActorOwned)].objects;
		}
	}

	void DrawWetnessParity::Compare(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, const SceneTables& a_tables,
		const std::vector<BindlessShading>* a_rows, NoteFn a_note, void* a_owner)
	{
		// b7: the SetupGeometry hook's upload (bound only while enabled).
		auto& skin = globals::features::skin;
		if (skin.settings.EnableSkin && a_rows && a_objectIndex < a_rows->size()) {
			const auto& expected = (*a_rows)[a_objectIndex].skinPerGeometry;
			const auto& native = skin.currentWetness;
			++checks;
			if (native.x != 0.0f || native.y != 0.0f)
				++wetDraws;
			if (native.x != expected[0] || native.y != expected[1] || native.z != expected[2] || native.w != expected[3]) {
				++mismatches;
				a_note(a_owner, a_geometry, fmt::format("skin wetness: DCLF ({} {} {} {}), native ({} {} {} {})", expected[0], expected[1], expected[2],
					expected[3], native.x, native.y, native.z, native.w));
			}
			const auto* owner = a_geometry->GetUserData();
			const bool actor = owner && owner->GetFormType() == RE::FormType::ActorCharacter;
			const auto& members = Members(a_tables);
			if (actor != std::binary_search(members.begin(), members.end(), a_objectIndex)) {
				++ownerMismatches;
				a_note(a_owner, a_geometry, fmt::format("skin wetness: owned by an actor {}, DCLF resolved {}", actor, !actor));
			}
		}
	}

	void CheckRowWetness(const SceneTables& a_tables, const std::vector<BindlessShading>& a_rows, const std::vector<std::uint8_t>& a_named)
	{
		// The wetness, as the frame's start captured it (Skin::GetWetness's same-frame value: no second advance): a mesh named this
		// frame (joined, or its record rewritten) takes its value with the next frame's capture.
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		for (const auto o : Members(a_tables)) {
			if (o >= a_rows.size() || o >= a_named.size() || a_named[o] || !a_tables.objectGeometry[o])
				continue;
			++rowWetness;
			const float4 value = wetness ? skin.GetWetness(a_tables.objectGeometry[o]) : float4{};
			const float expected[4] = { value.x, value.y, value.z, value.w };
			if (std::memcmp(expected, a_rows[o].skinPerGeometry, sizeof(expected)) == 0)
				continue;
			++rowWetnessDiffer;
			if (rowWetnessFirst.empty()) {
				const auto* geometry = a_tables.objectGeometry[o];
				const auto* owner = geometry->GetUserData();
				const auto& row = a_rows[o].skinPerGeometry;
				rowWetnessFirst = fmt::format("slot {} '{}' (owner {:08x}, {}): expected ({} {} {} {}), row ({} {} {} {})", o, geometry->name.c_str(),
					owner ? owner->GetFormID() : 0u, owner ? static_cast<int>(owner->GetFormType()) : -1, expected[0], expected[1], expected[2], expected[3],
					row[0], row[1], row[2], row[3]);
			}
		}
	}

	std::string TakeRowWetnessText()
	{
		const auto text = fmt::format("; wetness {} meshes, {} differ{}{}", rowWetness, rowWetnessDiffer,
			rowWetness ? (rowWetnessDiffer ? " <- DIFFER; first: " : " <- OK") : "", rowWetnessFirst);
		rowWetness = rowWetnessDiffer = 0;
		rowWetnessFirst.clear();
		return text;
	}
}
