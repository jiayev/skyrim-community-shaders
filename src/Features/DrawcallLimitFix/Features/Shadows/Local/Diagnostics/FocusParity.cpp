// The focus views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): the Focus board against the engine's views (the tally,
// Diagnostics::ShadowViewTally, filled by the shadow epochs' checks), and what the engine registers into a covered focus view against
// its target's fade root. Render thread, but for the membership check on the registration threads.

#include "Features/DrawcallLimitFix/Features/Shadows/Local/FocusViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/Local.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <array>
#include <atomic>

namespace DCLF::Features::Shadows::Local
{
	namespace
	{
		Reports::Window focusWindow;

		// The focus membership check, from the engine's registration threads.
		std::atomic<std::uint64_t> registrations{ 0 }, outside{ 0 };
		std::atomic<std::uint32_t> firstState{ 0 };  // 0 free, 1 being written, 2 written
		std::array<char, 512> first{};  // named when met: the geometry need not outlive the window

		/** @brief The focus views' membership check since the last Take: registrations into a covered focus view, those under another fade root, the first named. */
		struct FocusMembers
		{
			std::uint64_t registrations = 0, outside = 0;
			std::string first;
		};

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

		/** @brief The focus views' parity line with the membership line, logged, the tally reset: every 300 frames with focus views. */
		void FocusViewsLines()
		{
			auto& p = Diagnostics::ShadowViewTally(Diagnostics::ShadowView::Focus);
			const std::uint64_t differ = p.viewProj + p.eye + p.viewport + p.casters + p.missing;
			logger::info("[DCLF] focus views (T3c: DCLF's, from the cameras BSShadowDirectionalLight::sub aimed, against the engine's): {} views ({} matrices, {} eye, "
						 "{} viewport, {} caster volume differ), {} without DCLF's{}{}",
				p.views, p.viewProj, p.eye, p.viewport, p.casters, p.missing, differ ? " <- FOCUS VIEW" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
			p = {};
			// The membership check.
			const auto members = TakeFocusMembers();
			logger::info("[DCLF] focus members (T3c: the engine's registrations into DCLF's focus views against their targets' fade roots): {} registrations, {} under "
						 "another root{}{}",
				members.registrations, members.outside, members.outside ? " <- FOCUS MEMBERS" : " <- OK", members.first.empty() ? std::string() : "; first: " + members.first);
		}

		void OnFocusFrame(const Diagnostics::On<Diagnostics::EnginePoint::FocusViewsFrame>&)
		{
			if (focusWindow.Tick())
				FocusViewsLines();
		}

		/** @brief A registration into a covered focus view against DCLF's rule: under the view's target's fade root. */
		void FocusRegistered(const Diagnostics::FocusRegistration& a_event)
		{
			registrations.fetch_add(1, std::memory_order_relaxed);
			const auto* geometry = static_cast<const RE::BSGeometry*>(a_event.geometry);
			const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const RE::NiAVObject* root = property ? property->fadeNode : nullptr;
			const auto* target = EngineFocusTarget(a_event.descriptor);
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
	}

	void SubscribeFocusParity()
	{
		Diagnostics::Subscribe<Diagnostics::EnginePoint::FocusViewsFrame>(OnFocusFrame);
		Diagnostics::Subscribe<Diagnostics::FocusRegistration>(FocusRegistered);
	}
}
