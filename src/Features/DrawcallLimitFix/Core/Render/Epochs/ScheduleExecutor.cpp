// The schedule's executor (ScheduleExecutor.h): the registered passes, the schedule resolved against them, and the gather that adds an
// extension's epochs' passes. The one place DCLF's passes reach the graph (dclf-layout.md, rule 6), besides Diagnostics' observers.

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

#include <array>
#include <string>

namespace DCLF::Draws
{
	namespace
	{
		/** @brief A registered pass (its descriptor, the strings owned). */
		struct PassEntry
		{
			std::string key, passName, who;
			Api::PassKind kind = Api::PassKind::Compute;
			Api::PassQueue queue = Api::PassQueue::Default;
			decltype(Api::PassDef::make) make = nullptr;
			decltype(Api::PassDef::emit) emit = nullptr;
			bool viewName = false;  // passName names "{view}"
		};

		/** @brief An epoch of the schedule, its keys resolved to the passes (indices into Executor::passes), in order. */
		struct ResolvedEpoch
		{
			std::string id;
			RenderGraphRuntime::Segment segment{};
			Api::EngineView view{};
			std::vector<std::uint32_t> passes;
		};

		constexpr std::string_view kViewToken = "{view}";

		// Written only during InstallFeatures (the main thread, before DCLF's threads exist: their start orders these writes before
		// every read), read-only after, as the registry's lists are.
		struct Executor
		{
			std::vector<PassEntry> passes;
			// By shape (Api::EpochShape), in the schedule's order.
			std::array<std::vector<ResolvedEpoch>, 3> epochs;
			bool resolved = false;
		};

		Executor& State()
		{
			static Executor executor;
			return executor;
		}

		/** @brief a_entry's graph name in a_context's epoch: its passName, "{view}" the view's prefix. */
		std::string NameOf(const PassEntry& a_entry, const Api::PassContext& a_context)
		{
			if (!a_entry.viewName)
				return a_entry.passName;
			auto name = a_entry.passName;
			const std::string_view prefix = a_context.View()->traits->prefix;
			for (auto at = name.find(kViewToken); at != std::string::npos; at = name.find(kViewToken, at + prefix.size()))
				name.replace(at, kViewToken.size(), prefix);
			return name;
		}
	}

	void RegisterPassDef(const Api::PassDef& a_pass, std::string_view a_who)
	{
		auto& state = State();
		if (a_pass.key.empty() || !a_pass.make == !a_pass.emit || (a_pass.make && a_pass.passName.empty()))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete pass {}", a_who, a_pass.key));
		for (const auto& entry : state.passes)
			if (entry.key == a_pass.key)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered pass {}, which {} has", a_who, a_pass.key, entry.who));
		state.passes.push_back({ std::string(a_pass.key), std::string(a_pass.passName), std::string(a_who), a_pass.kind, a_pass.queue, a_pass.make, a_pass.emit,
			a_pass.passName.find(kViewToken) != std::string_view::npos });
	}

	void ResolveSchedule(const Api::FrameSchedule& a_schedule)
	{
		auto& state = State();
		if (std::exchange(state.resolved, true))
			stl::report_and_fail("Drawcall Limit Fix: the schedule was resolved twice");
		for (const auto& epoch : a_schedule.epochs) {
			const auto shape = static_cast<std::size_t>(epoch.shape);
			if (shape >= state.epochs.size() || epoch.segment >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} has no shape or segment", epoch.id));
			ResolvedEpoch resolved{ std::string(epoch.id), static_cast<RenderGraphRuntime::Segment>(epoch.segment), static_cast<Api::EngineView>(epoch.view), {} };
			for (const auto key : epoch.passes) {
				std::uint32_t found = ~0u;
				for (std::uint32_t i = 0; i < state.passes.size() && found == ~0u; ++i)
					if (state.passes[i].key == key)
						found = i;
				if (found == ~0u)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} names pass {}, which nobody registered", epoch.id, key));
				resolved.passes.push_back(found);
			}
			state.epochs[shape].push_back(std::move(resolved));
		}
	}

	void GatherScheduled(Api::EpochShape a_shape, Api::EngineView a_view, const Api::PassContext& a_base, std::vector<org::RenderGraph::ExternalPassDesc>& a_out)
	{
		const auto& state = State();
		for (const auto& epoch : state.epochs[static_cast<std::size_t>(a_shape)]) {
			if (a_shape == Api::EpochShape::View && epoch.view != a_view)
				continue;
			auto context = a_base;
			context.epochId = epoch.id;
			context.segment = epoch.segment;
			context.epoch = RenderGraphRuntime::EpochOf(epoch.segment);
			Api::PassOut out(a_out, context.epoch);
			for (const auto index : epoch.passes) {
				const auto& entry = state.passes[index];
				if (entry.emit) {
					entry.emit(context, out);
					continue;
				}
				if (auto pass = entry.make(context))
					AddPass(a_out, entry.kind, NameOf(entry, context), std::move(pass), context.epoch, entry.queue);
			}
		}
	}
}

namespace DCLF::Api
{
	void PassOut::Add(std::string a_name, PassKind a_kind, std::shared_ptr<org::RenderPass> a_pass, PassQueue a_queue)
	{
		Draws::AddPass(out, a_kind, std::move(a_name), std::move(a_pass), epoch, a_queue);
	}
}
