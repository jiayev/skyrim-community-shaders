#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Events.h"
#	include "Observers.h"

// CS_DCLF_PERSISTENT_PARITY: every input of a publication's payloads against its object's view mask (ViewMaskOf), and every input of a
// list against the list's view types (a list holding an object the mask leaves out of it). Run by the builds ahead (one at a time); the
// report takes the counts.

namespace DCLF::Diagnostics
{
	void CheckViewMasks(const DrawsBuiltAhead& a_built)
	{
		if (!ParityDue(a_built.frame))
			return;
		ZoneScopedN("CS.DCLF.ViewMaskParity");
		const auto& a_draws = a_built.draws;
		const auto& a_tables = a_built.tables;
		auto& p = DrawsState::Get().viewMasks;
		std::uint64_t inputs = 0, differ = 0;
		// a_list: 0 and 1 the main part (undrawable, drawable), 2 + mode a shadow mode's.
		// The main part (both segments read the same): every input taken by one of them, and a drawable one by both (the colour
		// segment's bits, kViewMain or kViewDecal, and the depth segment's but for a decal). A scene entry's shadow bits are the shadow
		// build's.
		auto checkMain = [&](const DrawInput& a_input) {
			++inputs;
			const std::uint32_t expected = MainMaskOf(a_tables, a_input.objectIndex);
			const std::uint32_t mask = a_input.view.mask & kMainSideBits;
			// A forward object (blended Lighting, refraction, an Effect member: kViewForward or kViewRefraction alone) is the colour
			// segment's alone, as a decal is.
			const bool colourOnly = ObjectDecalGroup(a_input.flags) != 0 || (a_input.flags & kObjectForward) != 0;
			const bool drawable = (a_input.flags & kInputDrawable) != 0;
			if (mask == expected && (mask & (kDepthViewBits | kColourViewBits)) &&
				(!drawable || ((mask & kColourViewBits) && (colourOnly || (mask & kDepthViewBits)))))
				return;
			// A first-person member's is kViewFirstPerson alone: the first-person epochs', no main segment's.
			if (mask == expected && mask == kViewFirstPerson)
				return;
			++differ;
			std::uint64_t none = ~0ull;
			if (p.first.compare_exchange_strong(none, (std::uint64_t(drawable ? 1 : 0) << 56) | (std::uint64_t(a_input.objectIndex & 0xFFFFFFFFu) << 24) | (expected & 0xFFFFFFu)))
				p.firstMask.store(mask, std::memory_order_relaxed);
		};
		if (const auto& payload = a_draws.payloads[kAsyncColour] ? a_draws.payloads[kAsyncColour] : a_draws.payloads[kAsyncZPrepass]) {
			// The scene list's region: an entry holds a main part, shadow words, or both; never neither.
			for (std::size_t i = 0; i < payload->resident.Count(); ++i) {
				const DrawInput& input = *payload->resident.At(i);
				if (input.view.mask & kMainSideBits)
					checkMain(input);
				else if (!input.view.mask) {
					++inputs;
					++differ;
				}
			}
			for (const auto& input : payload->inputList)
				checkMain(input);
		}
		// The shadow list: an input's mask is the views of the modes that hold it (ModeViewBits), never outside its object's phases,
		// with the key of each family it is drawn in.
		if (const auto& shadowPayload = a_draws.shadow)
			shadowPayload->ForEachInput([&](const DrawInput& a_input) {
				const std::uint32_t shadowMask = a_input.view.mask & ~kMainSideBits;
				if (!shadowMask)
					return;  // a scene entry's main part alone
				++inputs;
				const std::uint32_t phases = ViewMaskOf(a_tables, a_input.objectIndex, false);
				std::uint32_t list = 0;
				bool ok = (shadowMask & ~phases) == 0;
				for (std::uint32_t m = 0; m < kShadowModeCount && ok; ++m)
					if (shadowPayload->inputs.modeUsed[m] && (a_input.view.mask & ModeViewBits(m))) {
						ok = ShadowKeyOf(m, a_input) != ~0u;
						list = 2 + m;
					}
				if (ok)
					return;
				++differ;
				std::uint64_t none = ~0ull;
				if (p.first.compare_exchange_strong(none, (std::uint64_t(list) << 56) | (std::uint64_t(a_input.objectIndex & 0xFFFFFFFFu) << 24) | (phases & 0xFFFFFFu)))
					p.firstMask.store(a_input.view.mask, std::memory_order_relaxed);
			});
		p.checks.fetch_add(1, std::memory_order_relaxed);
		p.inputs.fetch_add(inputs, std::memory_order_relaxed);
		p.differ.fetch_add(differ, std::memory_order_relaxed);
	}
}

#endif
