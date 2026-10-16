// The point lights' culls' parity (LightCulls.h): counted on the registration threads, taken by the render thread's report.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LightCulls.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Diagnostics/LocalLightCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/SunExclusion.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <array>
#include <atomic>
#include <mutex>

namespace DCLF::Features::Shadows::Local::LightCulls
{
	namespace
	{
		using View = LocalLightCullView;
		using LocalLightCull::kReachKindCount;
		using LocalLightCull::kReachNames;

		struct Atomics
		{
			std::atomic<std::uint64_t> passes{ 0 }, lost{ 0 }, filterChecked{ 0 }, filterMissed{ 0 };
			std::atomic<std::uint64_t> compared{ 0 }, agree{ 0 }, engineOnly{ 0 }, dclfOnly{ 0 }, unhandled{ 0 };
			// By the reach of the light that differs (the last: it did not reach the entry).
			std::array<std::atomic<std::uint64_t>, kReachKindCount + 1> engineOnlyBy{}, dclfOnlyBy{};
		};
		Atomics counts;
		std::uint64_t frames = 0;  // the frames the culls kept whole (render thread)
		std::mutex firstMutex;     // the first lost pass and first missed caster, from the registration threads
		std::string firstLost;
		std::array<std::uint64_t, 16> lostByReject{};
		std::string filterFirstMissed;
		std::uint32_t bitsLogged = 0;

		void Selected(const Diagnostics::LightCullsSelected& a_event)
		{
			frames += a_event.whole ? 1 : 0;
		}

		/**
		 * @brief Whether a_object is under an excluded entry: any of its ancestors, as the skip takes it out (Process1 returns there). A
		 * caster of one need not be a candidate geometry: an entry is excluded by its candidates alone (an effect under a cell entry, for one).
		 */
		bool UnderExcluded(const SunExclusion& a_exclusion, const RE::NiAVObject* a_object)
		{
			bool underExcluded = false;
			for (const RE::NiAVObject* object = a_object; object && !underExcluded; object = object->parent)
				underExcluded = View::Excluded(a_exclusion, object);
			return underExcluded;
		}

		/**
		 * @brief The category filter: a caster under a filtered category node must hang from a child it keeps, withheld or not (the
		 * registration also writes the engine's geometry's light masks).
		 */
		void CheckFilter(const SunExclusion& a_exclusion, const RE::BSRenderPass* a_pass)
		{
			bool through = false;
			if (!View::UnderKeptChild(a_pass->geometry, through) && through) {
				counts.filterMissed.fetch_add(1, std::memory_order_relaxed);
				std::string chain;
				for (const RE::NiAVObject* object = a_pass->geometry; object; object = object->parent)
					chain += fmt::format(" <- {}:{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", Engine::IsHidden(object) ? " hidden" : "",
						a_exclusion.candidates->entries.contains(object) ? (View::Excluded(a_exclusion, object) ? " [excluded entry]" : " [entry]") : "");
				std::scoped_lock lock(firstMutex);
				if (filterFirstMissed.empty())
					filterFirstMissed = chain;
			}
			counts.filterChecked.fetch_add(through ? 1 : 0, std::memory_order_relaxed);
		}

		/** @brief A lost pass's object in the tables (its flags, shadow reject and technique): its reject, 15 when it has none. */
		std::uint32_t DescribeObject(const RE::BSRenderPass* a_pass, std::string& a_what)
		{
			const auto& tables = Scene::FrameView::Get().GetTables();
			std::uint32_t reject = 15;
			for (std::uint32_t o = 0; o < tables.objectGeometry.size(); ++o)
				if (tables.objectGeometry[o] == a_pass->geometry) {
					reject = tables.shadowReject[o];
					a_what += fmt::format(" object {} flags {:#x} reject {} technique {:#x}", o, tables.objects[o].flags, tables.shadowReject[o], tables.shadowTechnique[o]);
					break;
				}
			return reject;
		}

		/** @brief A lost pass's candidate entry, the exclusion's build, and each candidate geometry of the entry now. */
		void DescribeCandidate(const SunExclusion& a_exclusion, const RE::BSRenderPass* a_pass, std::string& a_what)
		{
			const auto& tables = Scene::FrameView::Get().GetTables();
			const auto& candidates = *a_exclusion.candidates;
			const auto it = candidates.geometries.find(a_pass->geometry);
			a_what += it == candidates.geometries.end() ? std::string(" not a candidate geometry") :
			                                              fmt::format(" candidate of entry {} (excluded {})", candidates.geometryEntry[it->second],
															  View::Excluded(a_exclusion, candidates.entryNodes[candidates.geometryEntry[it->second]]));
			// The exclusion's build, and each candidate geometry of the entry now (object, flags, claimed).
			a_what += fmt::format(" [exclusion built frame {}{}, selected at {}]", a_exclusion.builtFrame, a_exclusion.reused ? " (reused)" : "", Scene::Coordinator::Get().GetFrame());
			if (it != candidates.geometries.end()) {
				const auto entry = candidates.geometryEntry[it->second];
				const auto claims = Scene::FrameView::Get().FrameClaims();
				for (const auto& [geometry, index] : candidates.geometries) {
					if (candidates.geometryEntry[index] != entry)
						continue;
					std::uint32_t object = ~0u;
					for (std::uint32_t o = 0; o < tables.objectGeometry.size() && object == ~0u; ++o)
						object = tables.objectGeometry[o] == geometry && !(tables.objects[o].flags & kObjectFree) ? o : object;
					a_what += fmt::format(" {{'{}' object {} flags {:#x} claimed {}}}", geometry->name.c_str() ? geometry->name.c_str() : "", static_cast<std::int32_t>(object),
						object != ~0u ? tables.objects[object].flags : 0u, claims && (claims->PhasesOf(geometry) & kSetCasterPoint));
				}
			}
		}

		/** @brief How the cull reached a lost pass: the chain up, with the entries and the excluded ones marked, and each switch's selection. */
		void DescribeChain(const SunExclusion& a_exclusion, const RE::BSRenderPass* a_pass, std::string& a_what)
		{
			const RE::NiAVObject* below = nullptr;
			for (const RE::NiAVObject* object = a_pass->geometry; object; below = object, object = object->parent) {
				a_what += fmt::format(" <- {}:{}{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "", Engine::IsHidden(object) ? " hidden" : "",
					a_exclusion.candidates->entries.contains(object) ? (View::Excluded(a_exclusion, object) ? " [excluded entry]" : " [entry]") : "");
				if (const auto* switchNode = below ? const_cast<RE::NiAVObject*>(object)->AsSwitchNode() : nullptr) {
					const auto state = Engine::ReadSwitch(*switchNode);
					std::int32_t path = -1;
					const auto& children = switchNode->GetChildren();
					for (std::uint16_t c = 0; c < children.size(); ++c)
						path = children[c].get() == below ? static_cast<std::int32_t>(c) : path;
					a_what += fmt::format(" [index {}, path child {}, selects it {}]", state.index, path, Engine::SwitchSelects(*switchNode, below));
				}
			}
		}

		/** @brief A pass not withheld under an excluded entry, which the skip would lose: counted by its object's reject, the first described. */
		void NoteLost(const SunExclusion& a_exclusion, const RE::BSRenderPass* a_pass, std::uint32_t a_source)
		{
			counts.lost.fetch_add(1, std::memory_order_relaxed);
			std::string what = fmt::format("'{}' (insert {})", a_pass->geometry->name.c_str(), a_source);
			const std::uint32_t reject = DescribeObject(a_pass, what);
			DescribeCandidate(a_exclusion, a_pass, what);
			DescribeChain(a_exclusion, a_pass, what);
			std::scoped_lock lock(firstMutex);
			++lostByReject[std::min<std::uint32_t>(reject, 15)];
			if (firstLost.empty())
				firstLost = std::move(what);
		}

		/** @brief A frame the culls keep whole: a pass the skip would have kept from the engine. */
		void Registered(const Diagnostics::PointLightPass& a_event)
		{
			const auto* a_pass = a_event.pass;
			const bool a_withheld = a_event.withheld;
			const std::uint32_t a_source = a_event.source;
			if (!View::Installed() || !View::CullsWhole() || !a_pass || !a_pass->geometry)
				return;
			const auto* exclusion = View::FrameExclusion();
			if (!exclusion || !View::ParaboloidRenderer(a_event.batch))
				return;
			const bool underExcluded = UnderExcluded(*exclusion, a_pass->geometry);
			if (!underExcluded && View::FilterLive())
				CheckFilter(*exclusion, a_pass);
			if (a_withheld)
				return;
			counts.passes.fetch_add(1, std::memory_order_relaxed);
			if (underExcluded)
				NoteLost(*exclusion, a_pass, a_source);
		}

		/**
		 * @brief A frame the culls keep whole cuts nothing: where a cut would have taken the geometry out (an excluded ancestor, or a filtered
		 * node's child it does not keep), the engine's bits are compared with DCLF's.
		 */
		void Bits(const Diagnostics::PointLightBits& a_event)
		{
			const auto* a_geometry = a_event.geometry;
			const std::uint32_t entry = a_event.entry, bits = a_event.bits, engine = a_event.engine;
			const auto& reach = a_event.reach;
			const auto* exclusion = View::FrameExclusion();
			const auto& candidates = *exclusion->candidates;
			if (entry == ~0u) {
				bool cut = false;
				for (const RE::NiAVObject* object = a_geometry; object && !cut; object = object->parent)
					cut = View::Excluded(*exclusion, object);
				if (!cut && View::FilterLive()) {
					bool through = false;
					cut = !View::UnderKeptChild(a_geometry, through) && through;
				}
				if (!cut || !engine)
					return;
				// Cut, but not as a geometry of an excluded entry: its bits would be lost.
				counts.unhandled.fetch_add(1, std::memory_order_relaxed);
				std::scoped_lock lock(firstMutex);
				if (bitsLogged++ < 8) {
					std::string chain;
					for (const RE::NiAVObject* object = a_geometry; object; object = object->parent)
						chain += fmt::format(" <- {}:{}{}", object->GetRTTI() ? object->GetRTTI()->name : "?", object->name.c_str() ? object->name.c_str() : "",
							candidates.entries.contains(object) ? (View::Excluded(*exclusion, object) ? " [excluded entry]" : " [entry]") : "");
					logger::info("[DCLF] point lights' bits: cut but not an excluded entry's geometry, engine bits {:#x}:{}", engine, chain);
				}
				return;
			}
			counts.compared.fetch_add(1, std::memory_order_relaxed);
			if (engine == bits) {
				counts.agree.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			(engine & ~bits ? counts.engineOnly : counts.dclfOnly).fetch_add(1, std::memory_order_relaxed);
			const std::uint32_t lights = View::FrameLights();
			std::string detail;
			for (std::uint32_t light = 0; light < lights; ++light) {
				const auto sphere = View::FrameLight(light);
				const std::uint32_t bit = sphere.bit;
				if (!bit || !((engine ^ bits) & bit))
					continue;
				std::uint32_t kind = kReachKindCount;
				for (std::uint32_t k = 0; k < kReachKindCount && kind == kReachKindCount; ++k)
					kind = (reach[k] >> light) & 1 ? k : kReachKindCount;
				((engine & bit) ? counts.engineOnlyBy : counts.dclfOnlyBy)[kind].fetch_add(1, std::memory_order_relaxed);
				const auto& bound = a_geometry->worldBound;
				const float distance = std::sqrt((bound.center.x - sphere.centre[0]) * (bound.center.x - sphere.centre[0]) +
												 (bound.center.y - sphere.centre[1]) * (bound.center.y - sphere.centre[1]) +
												 (bound.center.z - sphere.centre[2]) * (bound.center.z - sphere.centre[2]));
				detail += fmt::format(" light {} ({}, {} only, distance {:.0f}, bound {:.0f}, radius {:.0f})", light, kReachNames[kind], (engine & bit) ? "engine" : "DCLF", distance,
					bound.radius, sphere.radius);
			}
			std::scoped_lock lock(firstMutex);
			if (bitsLogged++ < 8)
				logger::info("[DCLF] point lights' bits differ: '{}' (entry '{}'), engine {:#x}, DCLF {:#x}:{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "?",
					candidates.entryNodes[entry]->name.c_str() ? candidates.entryNodes[entry]->name.c_str() : "?", engine, bits, detail);
		}
	}

	void Install()
	{
		Diagnostics::Subscribe<Diagnostics::LightCullsSelected>(Selected);
		Diagnostics::Subscribe<Diagnostics::PointLightPass>(Registered);
		Diagnostics::Subscribe<Diagnostics::PointLightBits>(Bits);
	}

	Counts Take()
	{
		const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0); };
		Counts c;
		c.frames = std::exchange(frames, 0);
		c.passes = take(counts.passes), c.lost = take(counts.lost), c.filterChecked = take(counts.filterChecked), c.filterMissed = take(counts.filterMissed);
		c.compared = take(counts.compared), c.agree = take(counts.agree), c.engineOnly = take(counts.engineOnly), c.dclfOnly = take(counts.dclfOnly);
		c.unhandled = take(counts.unhandled);
		for (std::uint32_t k = 0; k <= kReachKindCount; ++k) {
			c.engineOnlyBy[k] = take(counts.engineOnlyBy[k]);
			c.dclfOnlyBy[k] = take(counts.dclfOnlyBy[k]);
		}
		std::scoped_lock lock(firstMutex);
		c.lostByReject = std::exchange(lostByReject, {});
		c.firstLost = std::exchange(firstLost, {});
		c.filterFirstMissed = std::exchange(filterFirstMissed, {});
		bitsLogged = 0;
		return c;
	}
}
