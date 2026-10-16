// The schedule's executor (ScheduleExecutor.h): the registered passes, the schedule resolved against them, the passes' setups and latch
// word, and the gather that adds an extension's epochs' passes. The one place DCLF's passes reach the graph (dclf-layout.md, rule 6), besides Diagnostics' observers.

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"

#include <algorithm>
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
			decltype(Api::PassDef::setup) setup = nullptr;
			decltype(Api::PassDef::latchWord) latchWord = nullptr;
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
		// The depth commit's build, whose latch holds the stage word (Api::PassDef::latchWord): the pass reading it runs before it.
		constexpr std::string_view kDepthBuild = "core.z.build-draws";

		// Written only during InstallFeatures (the main thread, before DCLF's threads exist: their start orders these writes before
		// every read), read-only after, as the registry's lists are.
		struct Executor
		{
			std::vector<PassEntry> passes;
			// By shape (Api::EpochShape), in the schedule's order.
			std::array<std::vector<ResolvedEpoch>, 3> epochs;
			decltype(Api::PassDef::latchWord) latchWord = nullptr;  // the one pass's that has one (none: null)
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
		if (a_pass.key.empty() || !a_pass.make == !a_pass.emit || (a_pass.make && a_pass.passName.empty()) || (a_pass.emit && (a_pass.setup || a_pass.latchWord)))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete pass {}", a_who, a_pass.key));
		for (const auto& entry : state.passes)
			if (entry.key == a_pass.key)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered pass {}, which {} has", a_who, a_pass.key, entry.who));
		state.passes.push_back({ std::string(a_pass.key), std::string(a_pass.passName), std::string(a_who), a_pass.kind, a_pass.queue, a_pass.make, a_pass.emit,
			a_pass.setup, a_pass.latchWord, a_pass.passName.find(kViewToken) != std::string_view::npos });
	}

	void ResolveSchedule(const Api::FrameSchedule& a_schedule)
	{
		auto& state = State();
		if (std::exchange(state.resolved, true))
			stl::report_and_fail("Drawcall Limit Fix: the schedule was resolved twice");
		// BuildDrawsLatch has one stage word, the depth segment's (Api/Passes.h).
		std::uint32_t latchPass = ~0u;
		for (std::uint32_t i = 0; i < state.passes.size(); ++i) {
			if (!state.passes[i].latchWord)
				continue;
			if (latchPass != ~0u)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s pass {} asks for the latch's stage word, which {}'s pass {} has: grow BuildDrawsLatch first",
					state.passes[i].who, state.passes[i].key, state.passes[latchPass].who, state.passes[latchPass].key));
			latchPass = i;
		}
		bool latchScheduled = false;
		for (const auto& epoch : a_schedule.epochs) {
			const auto shape = static_cast<std::size_t>(epoch.shape);
			if (shape >= state.epochs.size() || epoch.segment >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} has no shape or segment", epoch.id));
			ResolvedEpoch resolved{ std::string(epoch.id), static_cast<RenderGraphRuntime::Segment>(epoch.segment), static_cast<Api::EngineView>(epoch.view), {} };
			for (std::size_t k = 0; k < epoch.passes.size(); ++k) {
				const auto key = epoch.passes[k];
				std::uint32_t found = ~0u;
				for (std::uint32_t i = 0; i < state.passes.size() && found == ~0u; ++i)
					if (state.passes[i].key == key)
						found = i;
				if (found == ~0u)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} names pass {}, which nobody registered", epoch.id, key));
				// A setup's state is held with the scene's tables, which a Shadow epoch's context does not reach.
				if (state.passes[found].setup && epoch.shape == Api::EpochShape::Shadow)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} names pass {}, which has a setup, in a shadow epoch", epoch.id, key));
				// The stage word is the depth commit's: its pass runs in the depth epoch, before the build that reads the latch.
				if (found == latchPass) {
					const auto later = epoch.passes.subspan(k + 1);
					const bool beforeBuild = epoch.segment == static_cast<std::uint32_t>(RenderGraphRuntime::Segment::ZPrepass) && epoch.shape == Api::EpochShape::Main &&
					                         std::find(later.begin(), later.end(), kDepthBuild) != later.end();
					if (!beforeBuild)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} names pass {}, which asks for the latch's stage word, outside the depth epoch before {}",
							epoch.id, key, kDepthBuild));
					latchScheduled = true;
				}
				resolved.passes.push_back(found);
			}
			state.epochs[shape].push_back(std::move(resolved));
		}
		if (latchPass != ~0u) {
			if (!latchScheduled)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: pass {} asks for the latch's stage word, but the schedule does not name it", state.passes[latchPass].key));
			state.latchWord = state.passes[latchPass].latchWord;
		}
	}

	std::vector<std::shared_ptr<const void>> SetUpPassStates(rhi::Device a_device, const SceneBuffers& a_scene)
	{
		const auto& passes = State().passes;
		std::vector<std::shared_ptr<const void>> states(passes.size());
		const Api::PassSetup setup{ a_device, a_scene };
		for (std::size_t i = 0; i < passes.size(); ++i)
			if (passes[i].setup)
				states[i] = passes[i].setup(setup);
		return states;
	}

	std::uint32_t PassLatchWord()
	{
		const auto latchWord = State().latchWord;
		return latchWord ? latchWord() : 0u;
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
				// A pass with a setup only with its state (SetUpPassStates); the others get none.
				context.state = nullptr;
				if (entry.setup) {
					const auto& states = context.Main()->scene->passStates;
					if (index >= states.size() || !states[index])
						continue;
					context.state = states[index];
				}
				if (auto pass = entry.make(context))
					AddPass(a_out, entry.kind, NameOf(entry, context), std::move(pass), context.epoch, entry.queue);
			}
		}
	}
}

namespace DCLF::Api
{
	bool PassContext::PhaseTwo() const
	{
		return static_cast<bool>(Main()->hzb);
	}

	void PassOut::Add(std::string a_name, PassKind a_kind, std::shared_ptr<org::RenderPass> a_pass, PassQueue a_queue)
	{
		Draws::AddPass(out, a_kind, std::move(a_name), std::move(a_pass), epoch, a_queue);
	}
}
