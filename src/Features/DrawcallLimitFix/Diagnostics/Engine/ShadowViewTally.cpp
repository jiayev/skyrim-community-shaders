// The shadow views' parity (ShadowViewTally.h): render thread, but for the focus membership check on the registration threads.

#include "ShadowViewTally.h"

#include "Events.h"
#include "SunAccumulationView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Engine.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <atomic>

namespace DCLF::Diagnostics
{
	namespace
	{
		constexpr std::size_t kProcessObjectArray = 0x128;  // BSCullingProcess::objectArray
		std::array<ViewTally, 3> tallies;
		Reports::Window sunWindow, lightWindow, focusWindow;
		std::uint32_t cascadesChecked = 0;  // per cascade of the current Accumulate: compared with the cascade cull's

		// The focus membership check, from the engine's registration threads.
		std::atomic<std::uint64_t> registrations{ 0 }, outside{ 0 };
		std::atomic<std::uint32_t> firstState{ 0 };  // 0 free, 1 being written, 2 written
		std::array<char, 512> first{};  // named when met: the geometry need not outlive the window

		void CopyPlanes(const RE::NiFrustumPlanes& a_from, std::array<std::array<float, 4>, 6>& a_to, std::uint32_t& a_mask)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				const auto& plane = a_from.cullingPlanes[p];
				a_to[p] = { plane.normal.x, plane.normal.y, plane.normal.z, plane.constant };
			}
			a_mask = a_from.activePlanes.underlying() & 0x3Fu;
		}

		void SunUpdated(const On<EnginePoint::SunViewsUpdated>&)
		{
			if (sunWindow.Tick())
				Reports::SunViewsLines();
		}

		void LightFrame(const On<EnginePoint::LightViewsFrame>&)
		{
			if (lightWindow.Tick())
				Reports::LightViewsLines();
		}

		void FocusFrame(const On<EnginePoint::FocusViewsFrame>&)
		{
			if (focusWindow.Tick())
				Reports::FocusViewsLines();
		}

		/** @brief A registration into a covered focus view against DCLF's rule: under the view's target's fade root. */
		void FocusRegistered(const FocusRegistration& a_event)
		{
			registrations.fetch_add(1, std::memory_order_relaxed);
			const auto* geometry = static_cast<const RE::BSGeometry*>(a_event.geometry);
			const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const RE::NiAVObject* root = property ? property->fadeNode : nullptr;
			const auto* target = ShadowCameras::EngineFocusTarget(a_event.descriptor);
			if (root == target || outside.fetch_add(1, std::memory_order_relaxed) != 0)
				return;
			std::uint32_t free = 0;
			if (!firstState.compare_exchange_strong(free, 1, std::memory_order_acquire))
				return;
			const auto written = fmt::format_to_n(first.data(), first.size() - 1, "'{}' in focus view {} under fade root '{}', the target '{}'",
				geometry && geometry->name.c_str() ? geometry->name.c_str() : "", a_event.descriptor, root && root->name.c_str() ? root->name.c_str() : "none",
				target && target->name.c_str() ? target->name.c_str() : "none");
			*written.out = '\0';
			firstState.store(2, std::memory_order_release);
		}

		void AccumulateBegin(const On<EnginePoint::SunAccumulateBegin>&)
		{
			cascadesChecked = 0;
		}

		void CascadeMissing(const SunCascadeMissing& a_event)
		{
			auto& tally = tallies[static_cast<std::size_t>(ShadowView::Sun)];
			++tally.missing;
			const auto& sun = ShadowCameras::Sun();
			tally.Note("missing cascade", 0.0f, fmt::format("Accumulate of frame {}, descriptor {}, DCLF's views of frame {} ({} cascades)", a_event.frame, a_event.descriptor,
				sun.sceneFrame, sun.count));
		}

		/** @brief The cascade's volume is DCLF's (Accumulate's start), checked once a cascade against the cull's. */
		void CascadeCulled(const SunCascadeCulled& a_event)
		{
			// After the cull: the traversal's first call (vfunc 0xB8) has set the process up from the cascade's camera. With an empty
			// objectArray it made none, and the planes are not this cascade's.
			const std::uint32_t bit = 1u << (a_event.cascade & 31);
			if ((cascadesChecked & bit) || Engine::At<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>(a_event.fullProcess, kProcessObjectArray).empty())
				return;
			cascadesChecked |= bit;
			const auto& cascade = SunAccumulationView::Cascade(a_event.cascade);
			auto& p = tallies[static_cast<std::size_t>(ShadowView::Sun)];
			++p.cascades;
			ShadowCameras::Planes engine, mine;
			CopyPlanes(a_event.process->planes, engine.plane, engine.mask);
			mine.plane = cascade.planes;
			mine.mask = cascade.planeMask;
			if (!cascade.captured || !ShadowCameras::SamePlanes(mine, engine)) {
				++p.cascadePlanes;
				p.Note("cascade planes", 1.0f, fmt::format("cascade {}: {}", a_event.cascade, ShadowCameras::DescribePlanes(mine, engine)));
			}
			ShadowCameras::Planes engineCaster, mineCaster;
			if (a_event.process->doCustomCullPlanes)
				CopyPlanes(a_event.process->customCullPlanes, engineCaster.plane, engineCaster.mask);
			mineCaster.plane = cascade.customPlanes;
			mineCaster.mask = cascade.customMask;
			if (!ShadowCameras::SamePlanes(mineCaster, engineCaster)) {
				++p.cascadeCasters;
				p.Note("cascade caster planes", 1.0f, fmt::format("cascade {}: masks {:#x} {:#x}", a_event.cascade, mineCaster.mask, engineCaster.mask));
			}
		}
	}

	void ViewTally::Note(const char* a_what, float a_size, std::string a_detail)
	{
		largest = std::max(largest, a_size);
		// The first of each kind, up to four kinds.
		if (first.find(a_what) == std::string::npos && std::count(first.begin(), first.end(), '|') < 3)
			first += fmt::format("{}{}: {}", first.empty() ? "" : " | ", a_what, a_detail);
	}

	ViewTally& ShadowViewTally(ShadowView a_view)
	{
		return tallies[static_cast<std::size_t>(a_view)];
	}

	void InstallShadowViews()
	{
		Subscribe<EnginePoint::SunViewsUpdated>(SunUpdated);
		Subscribe<EnginePoint::LightViewsFrame>(LightFrame);
		Subscribe<EnginePoint::FocusViewsFrame>(FocusFrame);
		Subscribe<FocusRegistration>(FocusRegistered);
		Subscribe<EnginePoint::SunAccumulateBegin>(AccumulateBegin);
		Subscribe<SunCascadeMissing>(CascadeMissing);
		Subscribe<SunCascadeCulled>(CascadeCulled);
	}

	FocusMembers TakeFocusMembers()
	{
		FocusMembers m;
		m.registrations = registrations.exchange(0, std::memory_order_relaxed);
		m.outside = outside.exchange(0, std::memory_order_relaxed);
		if (firstState.load(std::memory_order_acquire) == 2) {
			if (m.outside)
				m.first = first.data();
			firstState.store(0, std::memory_order_release);
		}
		return m;
	}
}
