// The schedule's executor (ScheduleExecutor.h): the registered passes, the schedule resolved against them, the passes' setups and latch
// word, and the gather that adds an extension's epochs' passes. The one place DCLF's passes reach the graph (dclf-layout.md, rule 6), besides Diagnostics' observers.

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"

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

		/**
		 * @brief An epoch of the schedule, its keys resolved to the passes, in order: each its index into Executor::passes, and for a
		 * hosted epoch's pass in its host's list (Api::EpochDef::hostedBy) that epoch's row (kNoEpoch: the epoch's own). hosts: the epochs
		 * it hosts, a bit each by row (View epochs: their passes are made from their views' resources).
		 */
		struct ResolvedPass
		{
			std::uint32_t pass = 0;
			std::uint32_t hosted = kNoEpoch;
		};
		struct ResolvedEpoch
		{
			std::string id;
			RenderGraphRuntime::Segment segment{};
			Api::EngineView view{};
			std::vector<ResolvedPass> passes;
			std::uint32_t hosts = 0;
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
			// The epochs' definitions (Revision/Epochs.h), by row: their traits (named by names, owned), ids, and the epoch recording each
			// main and shadow shape, and each view's.
			std::array<EpochTraits, kMaxEpochs> traits{};
			std::array<std::string, kMaxEpochs> ids, names;
			std::uint32_t epochCount = 0;
			std::array<std::uint32_t, 2> mainEpochs{ kNoEpoch, kNoEpoch };
			std::array<std::uint32_t, kShadowShapes> shadowEpochs{ kNoEpoch };
			std::array<ViewEpochs, kViews> views{};
			// The frame's orders, as segment values (FrameOrders).
			Extensions::FrameOrders orders;
			// The triggers' ids, by row; each epoch's trigger row; Core's triggers (CoreTrigger).
			std::vector<std::string> triggers;
			std::array<std::uint32_t, kMaxEpochs> epochTriggers{};
			std::array<Api::TriggerId, static_cast<std::size_t>(CoreTrigger::Count)> coreTriggers{};
		};

		// Core's trigger points' ids (CoreTrigger's order): FrameDriver marks them.
		constexpr std::string_view kCoreTriggers[] = { "depth.committed" };
		static_assert(std::size(kCoreTriggers) == static_cast<std::size_t>(CoreTrigger::Count));

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

		static_assert(static_cast<int>(Api::EpochShape::Main) == static_cast<int>(ShapeKind::Main) &&
		                  static_cast<int>(Api::EpochShape::Shadow) == static_cast<int>(ShapeKind::Shadow) &&
		                  static_cast<int>(Api::EpochShape::View) == static_cast<int>(ShapeKind::View),
			"Api::EpochShape is Core's ShapeKind");

		/** @brief The row of a_schedule's epoch a_id (a_schedule.epochs.size() + the foreign epoch's index for a foreign one; ~0u: none). */
		std::uint32_t RowOf(const Api::FrameSchedule& a_schedule, std::string_view a_id)
		{
			for (std::uint32_t i = 0; i < a_schedule.epochs.size(); ++i)
				if (a_schedule.epochs[i].id == a_id)
					return i;
			for (std::uint32_t i = 0; i < a_schedule.foreign.size(); ++i)
				if (a_schedule.foreign[i].id == a_id)
					return static_cast<std::uint32_t>(a_schedule.epochs.size()) + i;
			return ~0u;
		}

		/** @brief Whether a_schedule's epoch row a_row is hosted (Api::EpochDef::hostedBy): in neither order, its segment its host's. */
		bool Hosted(const Api::FrameSchedule& a_schedule, std::uint32_t a_row)
		{
			return a_row < a_schedule.epochs.size() && !a_schedule.epochs[a_row].hostedBy.empty();
		}

		/**
		 * @brief Fails the install unless every epoch's and foreign epoch's id is set and its own, and every segment is one and used once
		 * (a hosted epoch's is its host's: ResolveEpochTraits checks it).
		 */
		void CheckEpochIds(const Api::FrameSchedule& a_schedule)
		{
			const auto rows = static_cast<std::uint32_t>(a_schedule.epochs.size() + a_schedule.foreign.size());
			auto idOf = [&](std::uint32_t a_row) { return a_row < a_schedule.epochs.size() ? a_schedule.epochs[a_row].id : a_schedule.foreign[a_row - a_schedule.epochs.size()].id; };
			auto segmentOf = [&](std::uint32_t a_row) {
				return a_row < a_schedule.epochs.size() ? a_schedule.epochs[a_row].segment : a_schedule.foreign[a_row - a_schedule.epochs.size()].segment;
			};
			if (a_schedule.epochs.size() > kMaxEpochs)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule has {} epochs, more than the revisions hold ({}): grow kMaxEpochs first", a_schedule.epochs.size(), kMaxEpochs));
			for (std::uint32_t row = 0; row < rows; ++row) {
				if (idOf(row).empty() || RowOf(a_schedule, idOf(row)) != row)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} has no id, or one another epoch has", idOf(row)));
				if (segmentOf(row) >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} has no segment", idOf(row)));
				for (std::uint32_t other = 0; other < row; ++other)
					if (segmentOf(other) == segmentOf(row) && !Hosted(a_schedule, row) && !Hosted(a_schedule, other))
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epochs {} and {} share segment {}", idOf(other), idOf(row), segmentOf(row)));
			}
		}

		/**
		 * @brief Fails the install unless a_order (a_what) names every epoch and foreign epoch once, a hosted one never (it runs as its
		 * host); its segments, in order.
		 */
		std::vector<std::uint32_t> ResolveOrder(const Api::FrameSchedule& a_schedule, std::span<const std::string_view> a_order, std::string_view a_what)
		{
			std::size_t rows = a_schedule.epochs.size() + a_schedule.foreign.size();
			std::vector<std::uint32_t> segments;
			std::vector<bool> listed(rows, false);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row)
				rows -= Hosted(a_schedule, row) ? 1 : 0;
			for (const auto id : a_order) {
				const auto row = RowOf(a_schedule, id);
				if (row == ~0u || listed[row])
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's {} names epoch {}, which is unknown or named twice", a_what, id));
				if (Hosted(a_schedule, row))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's {} names epoch {}, which its host runs (hostedBy)", a_what, id));
				listed[row] = true;
				segments.push_back(row < a_schedule.epochs.size() ? a_schedule.epochs[row].segment : a_schedule.foreign[row - a_schedule.epochs.size()].segment);
			}
			if (segments.size() != rows)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's {} lists {} of its {} epochs", a_what, segments.size(), rows));
			return segments;
		}

		/**
		 * @brief Epoch a_row's traits in a_state: its shape's kind and index, the epoch it shares (an earlier one recording the same shape
		 * itself, or the install fails), its gate and claims; the epoch recording its shape, or a view's second one.
		 */
		void ResolveEpochTraits(Executor& a_state, const Api::FrameSchedule& a_schedule, std::uint32_t a_row)
		{
			const auto& epoch = a_schedule.epochs[a_row];
			const auto shape = static_cast<ShapeKind>(epoch.shape);
			const std::size_t indices = shape == ShapeKind::View ? kViews : shape == ShapeKind::Shadow ? kShadowShapes : 2;
			if (static_cast<std::size_t>(epoch.shape) > static_cast<std::size_t>(ShapeKind::View) || epoch.shapeIndex >= indices)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} records a shape Core has not ({} {})", epoch.id,
					static_cast<std::uint32_t>(epoch.shape), epoch.shapeIndex));
			a_state.ids[a_row] = std::string(epoch.id);
			a_state.names[a_row] = std::string(epoch.label);
			auto& traits = a_state.traits[a_row];
			traits = { static_cast<RenderGraphRuntime::Segment>(epoch.segment), a_state.names[a_row], shape, epoch.shapeIndex, kNoEpoch, epoch.gate, epoch.claims };
			if (!epoch.hostedBy.empty()) {
				// Its host's recordings carry its passes (the assembly requests them with its shape, the host's commit commits it): an
				// earlier Main epoch of its own, at the same trigger and segment (it may host others: each a dimension of its recordings,
				// RevisionRecordings::hosted); a View shape of its own, its passes in the host's list.
				const auto host = RowOf(a_schedule, epoch.hostedBy);
				if (host >= a_row || a_state.traits[host].shape != ShapeKind::Main || a_state.traits[host].sharesWith != kNoEpoch || a_state.traits[host].hostedBy != kNoEpoch ||
					shape != ShapeKind::View || !epoch.sharesWith.empty() || !epoch.passes.empty() || a_schedule.epochs[host].trigger != epoch.trigger ||
					a_schedule.epochs[host].segment != epoch.segment)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} is hosted by {}, which is not an earlier Main epoch of its own of its trigger and "
													 "segment, or it is no View epoch of its own with its passes in the host's list",
						epoch.id, epoch.hostedBy));
				traits.hostedBy = host;
				a_state.traits[host].hostsMask |= 1u << a_row;
			}
			if (!epoch.sharesWith.empty()) {
				// The assembly takes a shared shape from its source's slot of the same draft: the source comes first. A hosted source's
				// shape is shared as any other (first person's colour epoch records against its hosted depth epoch's); a hosted epoch never
				// shares one (its host's recordings alone carry its passes: checked above).
				const auto source = RowOf(a_schedule, epoch.sharesWith);
				if (source >= a_row || a_state.traits[source].shape != shape || a_state.traits[source].index != epoch.shapeIndex || a_state.traits[source].sharesWith != kNoEpoch)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} shares {}'s shape, which is not an earlier epoch recording that shape itself",
						epoch.id, epoch.sharesWith));
				traits.sharesWith = source;
				if (shape == ShapeKind::View) {
					auto& view = a_state.views[epoch.shapeIndex];
					if (view.epochs[1] != kNoEpoch)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} is a second sharing view {}'s shape", epoch.id, epoch.shapeIndex));
					view.epochs[1] = a_row;
					view.segments[1] = traits.segment;
				}
				return;
			}
			auto& owner = shape == ShapeKind::Main ? a_state.mainEpochs[epoch.shapeIndex] : shape == ShapeKind::Shadow ? a_state.shadowEpochs[epoch.shapeIndex] :
			                                                                              a_state.views[epoch.shapeIndex].epochs[0];
			if (owner != kNoEpoch)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epochs {} and {} both record one shape: the second shares it", a_state.ids[owner], epoch.id));
			owner = a_row;
			if (shape == ShapeKind::View)
				a_state.views[epoch.shapeIndex].segments = { traits.segment, traits.segment };
		}

		/**
		 * @brief The schedule's triggers, resolved into a_state: their ids, each epoch's trigger row and Core's triggers; fails the install
		 * for a trigger id missing or duplicated, an epoch's trigger the schedule has not, a trigger no epoch names or one of Core's missing.
		 */
		void ResolveTriggers(Executor& a_state, const Api::FrameSchedule& a_schedule)
		{
			const auto& triggers = a_schedule.triggers;
			for (std::uint32_t row = 0; row < triggers.size(); ++row) {
				if (triggers[row].id.empty() || FindTrigger(a_schedule, triggers[row].id, "the schedule").index != row)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's trigger {} has no id, or one another trigger has", triggers[row].id));
				a_state.triggers.emplace_back(triggers[row].id);
			}
			std::vector<bool> used(triggers.size(), false);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row) {
				const auto trigger = FindTrigger(a_schedule, a_schedule.epochs[row].trigger, fmt::format("the schedule's epoch {}", a_schedule.epochs[row].id));
				a_state.epochTriggers[row] = trigger.index;
				used[trigger.index] = true;
			}
			for (std::uint32_t row = 0; row < triggers.size(); ++row)
				if (!used[row])
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's trigger {} is named by no epoch", triggers[row].id));
			for (std::size_t i = 0; i < a_state.coreTriggers.size(); ++i)
				a_state.coreTriggers[i] = FindTrigger(a_schedule, kCoreTriggers[i], "Core");
		}

		/**
		 * @brief The schedule's epochs' definitions, resolved into a_state, and the frame's orders; fails the install for an unknown or
		 * duplicated id, a segment used twice, a shape Core has not or one it records with no epoch, a bad share, an order that does not
		 * name every epoch once, or more epochs than kMaxEpochs.
		 */
		void ResolveEpochs(Executor& a_state, const Api::FrameSchedule& a_schedule)
		{
			CheckEpochIds(a_schedule);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row)
				ResolveEpochTraits(a_state, a_schedule, row);
			ResolveTriggers(a_state, a_schedule);
			// Core records every main and shadow shape and every view's (their loops run over all of them).
			for (std::uint32_t i = 0; i < 2; ++i)
				if (a_state.mainEpochs[i] == kNoEpoch || (i < kShadowShapes && a_state.shadowEpochs[i] == kNoEpoch))
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule records no epoch for Core's main or shadow shape {}", i));
			for (std::size_t v = 0; v < kViews; ++v)
				if (a_state.views[v].epochs[0] == kNoEpoch)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule records no epoch for view {}", v));
			a_state.epochCount = static_cast<std::uint32_t>(a_schedule.epochs.size());
			auto& orders = a_state.orders;
			orders.epochOrder = ResolveOrder(a_schedule, a_schedule.frameOrder, "frame order");
			orders.asyncOrder = ResolveOrder(a_schedule, a_schedule.asyncOrder, "async order");
			// The revision-driven ones: the async order's DCLF epochs with revision (a foreign epoch never is).
			for (const auto id : a_schedule.asyncOrder)
				if (const auto row = RowOf(a_schedule, id); row < a_schedule.epochs.size() && a_schedule.epochs[row].revision)
					orders.revisionEpochs.push_back(a_schedule.epochs[row].segment);
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

	Api::TriggerId FindTrigger(const Api::FrameSchedule& a_schedule, std::string_view a_id, std::string_view a_who)
	{
		std::uint32_t row = 0;
		while (row < a_schedule.triggers.size() && a_schedule.triggers[row].id != a_id)
			++row;
		if (a_id.empty() || row == a_schedule.triggers.size())
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} names trigger {}, which the schedule has not", a_who, a_id));
		return { row };
	}

	void ResolveSchedule(const Api::FrameSchedule& a_schedule)
	{
		auto& state = State();
		if (std::exchange(state.resolved, true))
			stl::report_and_fail("Drawcall Limit Fix: the schedule was resolved twice");
		ResolveEpochs(state, a_schedule);
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
		for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row) {
			const auto& epoch = a_schedule.epochs[row];
			const auto shape = static_cast<std::size_t>(epoch.shape);
			if (shape >= state.epochs.size() || epoch.segment >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} has no shape or segment", epoch.id));
			// A hosted epoch's passes are its host's (their keys in the host's list).
			if (state.traits[row].hostedBy != kNoEpoch)
				continue;
			ResolvedEpoch resolved{ std::string(epoch.id), static_cast<RenderGraphRuntime::Segment>(epoch.segment), static_cast<Api::EngineView>(epoch.shapeIndex), {},
				state.traits[row].hostsMask };
			for (std::size_t k = 0; k < epoch.passes.size(); ++k) {
				auto key = epoch.passes[k];
				// A hosted epoch's pass: "{its id}/{key}", in the list of the epoch hosting it.
				std::uint32_t hosted = kNoEpoch;
				if (const auto at = key.find(Api::kHostedKey); at != std::string_view::npos) {
					hosted = EpochById(key.substr(0, at));
					if (hosted == kNoEpoch || state.traits[hosted].hostedBy != row)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: the schedule's epoch {} names pass {} of an epoch it does not host", epoch.id, key));
					key = key.substr(at + 1);
				}
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
				resolved.passes.push_back({ found, hosted });
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

	Api::TriggerId CoreTriggerOf(CoreTrigger a_trigger)
	{
		return State().coreTriggers[static_cast<std::size_t>(a_trigger)];
	}

	std::uint32_t EpochTrigger(std::uint32_t a_epoch)
	{
		return State().epochTriggers[a_epoch];
	}

	std::string_view TriggerName(std::uint32_t a_trigger)
	{
		const auto& triggers = State().triggers;
		return a_trigger < triggers.size() ? std::string_view(triggers[a_trigger]) : std::string_view("none");
	}

	std::uint32_t EpochCount()
	{
		return State().epochCount;
	}

	const EpochTraits& EpochTraitsOf(std::uint32_t a_epoch)
	{
		return State().traits[a_epoch];
	}

	std::uint32_t EpochOfShape(ShapeKind a_shape, std::uint32_t a_index)
	{
		const auto& state = State();
		switch (a_shape) {
		case ShapeKind::Main:
			return a_index < state.mainEpochs.size() ? state.mainEpochs[a_index] : kNoEpoch;
		case ShapeKind::Shadow:
			return a_index < state.shadowEpochs.size() ? state.shadowEpochs[a_index] : kNoEpoch;
		default:
			return a_index < state.views.size() ? state.views[a_index].epochs[0] : kNoEpoch;
		}
	}

	std::uint32_t EpochById(std::string_view a_id)
	{
		const auto& state = State();
		for (std::uint32_t e = 0; e < state.epochCount; ++e)
			if (state.ids[e] == a_id)
				return e;
		return kNoEpoch;
	}

	const ViewEpochs& ViewEpochsOf(ViewKind a_kind)
	{
		return State().views[ViewIndex(a_kind)];
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
			// The view epochs it hosts (a Main epoch's: the main resources' views), each while its view's passes can be added
			// (ViewPassesReady: its target imported, drawing from these main resources): their passes are made with their view, and the
			// epoch's own passes see them (PassContext::hosted, in row order: the latched copies take their lists too). A view not ready
			// has its passes not added; the others' are.
			std::array<std::shared_ptr<ViewResources>, kViews> hostedViews{};
			std::vector<std::shared_ptr<ViewResources>> hosted;
			for (std::uint32_t row = 0; a_shape == Api::EpochShape::Main && row < state.epochCount; ++row) {
				if (!((epoch.hosts >> row) & 1))
					continue;
				const auto view = state.traits[row].index;
				auto resources = context.Main()->views[view].lock();
				if (resources && ViewPassesReady(*resources, *context.Main()))
					hosted.push_back(hostedViews[view] = std::move(resources));
			}
			context.hosted = hosted;
			Api::PassOut out(a_out, context.epoch);
			for (const auto& [index, hostedEpoch] : epoch.passes) {
				const auto& entry = state.passes[index];
				context.view = a_base.view;
				if (hostedEpoch != kNoEpoch) {
					const auto& view = hostedViews[state.traits[hostedEpoch].index];
					if (!view)
						continue;
					context.view = &view;
				}
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

namespace DCLF::Extensions
{
	const FrameOrders& ScheduledFrameOrders()
	{
		return Draws::State().orders;
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
