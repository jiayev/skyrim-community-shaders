// CS_DCLF_PERSISTENT_PARITY: the catalog's constant tables (DCLF's modules reflected, what the builds pack by) against Community
// Shaders' (ShaderCache, which takes its lock and may start a compile: an observer only, never what the builds read), for every entry the
// installed lookups resolved. Render thread; the report takes the counts.

#include "DrawsState.h"
#include "Observers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "ShaderCache.h"

namespace DCLF::Diagnostics
{
	void CheckConstantTables(const RE::BSShader& a_lighting, const Lookups& a_lookups)
	{
		auto& parity = DrawsState::Get().constantTables;
		++parity.checks;
		auto& cache = SIE::ShaderCache::Instance();
		// A table entry as a byte (Community Shaders' are int8_t, read unsigned by the engine), 0 past its end.
		auto at = [](const auto& a_table, std::uint32_t a_variable) -> std::uint32_t {
			return a_variable < a_table.size() ? static_cast<std::uint8_t>(a_table[a_variable]) : 0u;
		};
		for (const auto& entry : a_lookups.pipelines) {
			// An effect key's tables are of the Effect shader, not of the Lighting shader objects compared here.
			if (entry.setIndex == Lookups::kNone || KeyFamily(entry.key.rasterFlags) == Api::ProgramFamily::Effect)
				continue;
			++parity.entries;
			const auto* vs = cache.GetVertexShader(a_lighting, entry.key.vertexDescriptor);
			const auto* ps = cache.GetPixelShader(a_lighting, entry.key.pixelDescriptor);
			if (!vs || !ps) {
				++parity.uncached;
				continue;
			}
			for (const bool pixel : { false, true }) {
				const auto names = DrawPipelines::LightingVariableNames(pixel);
				const auto* groups = pixel ? kPSGroups : kVSGroups;
				const auto* firsts = pixel ? kPSFirstVariable : kVSFirstVariable;
				for (std::uint32_t v = 0; v < names.size(); ++v) {
					// Offset 0 is also what a table holds for a variable the stage lacks: it counts only for its group's first.
					std::uint32_t first = ~0u;
					for (std::uint32_t group = 0; group < 3; ++group)
						if ((groups[group] >> v) & 1)
							first = firsts[group];
					const std::uint32_t ours = at(pixel ? entry.psTable : entry.vsTable, v);
					const std::uint32_t theirs = pixel ? at(ps->constantTable, v) : at(vs->constantTable, v);
					const bool inOurs = ours != 0 || v == first, inTheirs = theirs != 0 || v == first;
					auto describe = [&] {
						return fmt::format("VS {:08X} PS {:08X}: {} {} at {}, ShaderCache {}", entry.key.vertexDescriptor, entry.key.pixelDescriptor, pixel ? "PS" : "VS",
							names[v], ours, theirs);
					};
					if (inOurs && inTheirs) {
						++parity.variables;
						if (ours != theirs && parity.differ++ == 0)
							parity.first = describe();
					} else if (inOurs) {
						if (parity.catalogOnly++ == 0)
							parity.catalogOnlyFirst = describe();
					} else if (inTheirs) {
						++parity.cacheOnly;
					}
				}
			}
		}
	}
}
