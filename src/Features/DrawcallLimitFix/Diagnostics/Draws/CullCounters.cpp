// The culling's own counters (always on): the colour epoch's count words, copied to the host each execution (CounterReadback) and
// sampled every 120th epoch, read when the sample's frame slot comes round, with the CPU's side of the sun test and, under
// CS_DCLF_SET_PARITY, of the terrain near test (a task on DCLF's executor) and the forward and refraction registrations. Under
// CS_DCLF_FRAME_TRACE every colour commit also logs its slot's last counts (FrameTrace.cpp).

#include "DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"
#include "CullCounters.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		/**
		 * @brief The CPU's side of a sample, over the sampled frame's colour inputs and planes, run by a task on DCLF's executor.
		 * Its inputs are immutable or its own; it reads the geometries' bounds under a lease each (the engine-read window), and lets
		 * go of the tables through EngineReleases. The render thread reads the results once `done` is set.
		 */
		struct CpuSide
		{
			decltype(KeptView<Draws::DrawInput>::elements) resident;  // the payload's resident region (immutable)
			std::vector<Draws::DrawInput> inputs;                           // its input list, copied
			std::shared_ptr<const SceneTables> tables;                      // the frame's (the geometries' owners)
			std::vector<Draws::GpuCascade> cascades;
			bool sunOn = false, terrain = false;
			std::array<float, 4> terrainNear{};
			// Results.
			std::uint32_t sunTested = 0, sunMissed = 0, terrainTested = 0, terrainNearCount = 0;
			bool complete = true;  // false: the window closed first (the sample is dropped)
			std::atomic<bool> done{ false };

			void Run()
			{
				ZoneScopedN("CS.DCLF.CullCounters.CpuSide");
				const RE::NiPoint3 eye{ terrainNear[0], terrainNear[1], terrainNear[2] };
				const auto test = [&](const Draws::DrawInput& a_input) {
					// The sun test: the colour segment's inputs of the one main list (its view bits, as BuildDrawsCS tests them first).
					const bool sun = sunOn && (a_input.view.mask & Draws::kColourViewBits) && (a_input.flags & kObjectSunTest);
					// CS_DCLF_SET_PARITY (<- TB NEAR): Terrain Blending's near test over the depth segment's first phase's terrain group
					// 1 inputs, with the eye and distance its latch had, as TerrainBlending.cpp tests a pass.
					const bool nearTest = terrain && (a_input.view.mask & Draws::kDepthViewBits) && ObjectTerrainGroup(a_input.flags) == 1;
					if (!sun && !nearTest)
						return true;
					sunTested += sun ? 1 : 0;
					terrainTested += nearTest ? 1 : 0;
					const auto* geometry = tables && a_input.objectIndex < tables->objectGeometry.size() ? tables->objectGeometry[a_input.objectIndex] : nullptr;
					if (!geometry)
						return true;
					const EngineReadWindow::Lease lease;
					if (!lease)
						return false;
					const auto bound = geometry->worldBound;
					if (sun) {
						const float center[3]{ bound.center.x, bound.center.y, bound.center.z };
						bool inside = false;
						for (std::size_t c = 0; c < cascades.size() && !inside; ++c)
							inside = Draws::InSunCascade(cascades[c], center, bound.radius);
						sunMissed += inside ? 0 : 1;
					}
					if (nearTest)
						terrainNearCount += bound.center.GetDistance(eye) - bound.radius <= terrainNear[3] ? 1 : 0;
					return true;
				};
				if (resident)
					for (const auto& input : *resident)
						if (!(complete = test(input)))
							break;
				if (complete)
					for (const auto& input : inputs)
						if (!(complete = test(input)))
							break;
				// What holds engine references is let go of on the render thread.
				EngineReleases::PushOwner(std::shared_ptr<void>(std::const_pointer_cast<SceneTables>(std::move(tables))));
				resident.reset();
				inputs = {};
				done.store(true, std::memory_order_release);
			}
		};

		/** @brief A colour sample: armed on the readback after its epoch, its words read at its slot's next colour commit. */
		struct ColourSample
		{
			std::weak_ptr<Draws::CounterReadback> on;  // the readback it was armed on (a graph build makes another)
			std::shared_ptr<CpuSide> cpu;
			std::uint32_t forwardEngine = 0, refractionEngine = 0;
			bool forwardObserved = false, refractionObserved = false, terrainCpu = false;
			bool read = false;
			std::array<std::uint32_t, Draws::kCountWords> words{};
		};

		/** @brief Render thread. Never destroyed: nothing in it is let go of at exit. */
		struct CullCounterState
		{
			// The colour commit's frame slot and culling stamp (NoteCullLatch), for the epoch's arm after it.
			std::uint32_t colourSlot = 0, colourStamp = 0;
			bool colourNoted = false;
			std::optional<ColourSample> sample;
			std::uint32_t epochs = 0;
		};
		CullCounterState& State()
		{
			static auto* state = new CullCounterState();
			return *state;
		}
	}

	void AppendCullCounterPass(const MainGraphPasses& a_passes)
	{
		const auto& resources = a_passes.resources;
		if (a_passes.point != MainGraphPasses::kAfterColour || !resources->count)
			return;
		// The colour shape's culling latch is at its slot region's start.
		auto pass = Draws::MakeCounterReadbackPass(Draws::CullCounterView::kColour, { resources->count }, [](const org::PassPrepareContext& a_preparation) {
			Draws::CounterSource out;
			if (const auto frame = Draws::CurrentFrame(a_preparation, RenderGraphRuntime::Segment::MainOpaque); frame && frame->latch) {
				out.generation = frame->generation;
				out.latch = frame->latch;
				out.copies.push_back({ 0, 0 });
			}
			return out;
		});
		a_passes.out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.cull-counters", std::move(pass)).Epoch(a_passes.epoch));
	}

	void NoteCullLatch(const CullLatchWritten& a_written)
	{
		auto& state = DrawsState::Get();
		if (a_written.depthOnly) {
			std::copy(std::begin(a_written.latch.terrainNear), std::end(a_written.latch.terrainNear), state.terrainNearUpload.begin());
			return;
		}
		state.sunUpload = a_written.latch;
		// Inside the colour commit: the host has waited for the slot's last execution and not yet submitted this one. A sample
		// armed in this slot is read now (its stamp names the execution the words are of).
		auto& counters = State();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		counters.colourSlot = host->CurrentFrameSlot();
		counters.colourStamp = a_written.latch.visibilityStamp;
		counters.colourNoted = true;
		const auto readback = Draws::CullCounterReadback(Draws::CullCounterView::kColour);
		// CS_DCLF_FRAME_TRACE: the slot's last colour execution's counts, every frame.
		if (Diagnostics::Observe().frameTrace && readback)
			TraceColourCounts(*readback, counters.colourSlot);
		auto& sample = counters.sample;
		if (!readback || !readback->armed.armed || readback->armed.frameSlot != counters.colourSlot || !sample || sample->read || sample->on.lock() != readback)
			return;
		auto& armed = readback->armed;
		armed.armed = false;
		sample->read = readback->Read(armed.frameSlot, armed.source, armed.stamp, [&](const std::uint32_t* a_words) { std::copy_n(a_words, Draws::kCountWords, sample->words.begin()); });
		++(sample->read ? readback->harvested : readback->missed);
		if (!sample->read)
			sample.reset();
	}

	void ReadCullCounters(const MainEpochDone& a_epoch)
	{
		if (a_epoch.depthOnly)
			return;
		auto& a_stats = a_epoch.stats;
		const MainPayload& a_payload = a_epoch.payload;
		const auto& impl = DrawsView::Get();
		auto& counters = State();
		const bool noted = std::exchange(counters.colourNoted, false);
		// This frame's engine blended registrations of members (taken every colour epoch, so each sample has its own frame's).
		const std::uint32_t forwardEngine = Registrations::TakeForward();
		const std::uint32_t refractionEngine = Registrations::TakeRefraction();  // the refraction's, likewise
		const auto readback = Draws::CullCounterReadback(Draws::CullCounterView::kColour);
		if (auto& sample = counters.sample) {
			if (!sample->read) {
				// Waiting for its slot's next colour commit; dropped when its readback is gone (a graph build).
				if (!readback || sample->on.lock() != readback || !readback->armed.armed)
					sample.reset();
				return;
			}
			if (!sample->cpu->done.load(std::memory_order_acquire))
				return;
			if (sample->cpu->complete) {
				const auto& words = sample->words;
				const auto& cpu = *sample->cpu;
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullOccluded = words[6];
				a_stats.hzbNear = words[7];
				a_stats.hzbFar = words[8];
				a_stats.hzbSampled = words[9];
				if (words[10]) {
					a_stats.hzbSample.valid = true;
					std::memcpy(&a_stats.hzbSample.farthest, &words[11], sizeof(float));
					std::memcpy(&a_stats.hzbSample.nearestZ, &words[12], sizeof(float));
					a_stats.hzbSample.uvMin[0] = (words[13] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMin[1] = (words[13] >> 16) / 65535.0f;
					a_stats.hzbSample.uvMax[0] = (words[14] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMax[1] = (words[14] >> 16) / 65535.0f;
					a_stats.hzbSample.mip = words[15];
				} else {
					a_stats.hzbSample.valid = false;
				}
				a_stats.cullDrawnPhaseTwo = words[17];
				a_stats.cullRescuedByPhaseTwo = words[18];
				a_stats.decalsCulled = words[kCountDecalsCulledWord];
				a_stats.decalsTested = words[kCountDecalsTestedWord];
				a_stats.sunTested = words[kCountSunTestedWord];
				a_stats.sunMissed = words[kCountSunMissedWord];
				a_stats.fadeTested = words[kCountFadeTestedWord];
				a_stats.fadeHidden = words[kCountFadeHiddenWord];
				if (impl.scene) {
					if (const auto* fades = Features::Fades::SceneOf(*impl.scene))
						a_stats.fadeRoots = fades->fadeRootCount;
				}
				a_stats.sunCpuTested = cpu.sunTested;
				a_stats.sunCpuMissed = cpu.sunMissed;
				a_stats.terrainNearTested = words[kCountTerrainNearTestedWord];
				a_stats.terrainNear = words[kCountTerrainNearWord];
				a_stats.terrainCpu = sample->terrainCpu;
				a_stats.terrainCpuTested = cpu.terrainTested;
				a_stats.terrainCpuNear = cpu.terrainNearCount;
				a_stats.forwardDrawn = words[kCountForwardWord];
				a_stats.forwardTested = words[kCountForwardTestedWord];
				a_stats.forwardInView = words[kCountForwardInViewWord];
				a_stats.forwardOccluded = words[kCountForwardOccludedWord];
				a_stats.forwardEngine = sample->forwardEngine;
				a_stats.forwardObserved = sample->forwardObserved;
				a_stats.refractionDrawn = words[kCountRefractionWord];
				a_stats.refractionTested = words[kCountRefractionTestedWord];
				a_stats.refractionInView = words[kCountRefractionInViewWord];
				a_stats.refractionOccluded = words[kCountRefractionOccludedWord];
				a_stats.classUnplanned = words[kCountClassUnplannedWord];
				a_stats.classOverflow = words[kCountClassOverflowWord];
				a_stats.refractionEngine = sample->refractionEngine;
				a_stats.refractionObserved = sample->refractionObserved;
			}
			sample.reset();
			return;
		}
		if ((counters.epochs++ % 120) != 0 || !readback || !noted)
			return;
		// Armed on this execution's slot and stamp; its copy is part of every colour execution.
		readback->armed = { true, counters.colourSlot, 0, counters.colourStamp, 0, 0, {} };
		auto& sample = counters.sample.emplace();
		sample.on = readback;
		sample.forwardEngine = forwardEngine;
		sample.forwardObserved = Diagnostics::Observe().claims && Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward);
		sample.refractionEngine = refractionEngine;
		sample.refractionObserved = Diagnostics::Observe().claims && Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Forward) &&
		                            Extensions::ObjectClassOn(ActiveToggles(), Api::ObjectClass::Refraction);
		const auto& terrainNearUpload = DrawsState::Get().terrainNearUpload;
		sample.terrainCpu = Diagnostics::Observe().claims && terrainNearUpload[3] > 0.0f;
		// The CPU's side over the same frame's colour inputs and the same planes, off the render thread (the window is open until
		// Present; a task that finds it closed drops the sample).
		auto cpu = std::make_shared<CpuSide>();
		cpu->resident = a_payload.resident.elements;
		cpu->inputs = a_payload.body->inputList;
		cpu->tables = Scene::FrameView::Get().AcceptedTables();
		cpu->cascades = impl.sunCascades;
		cpu->sunOn = (DrawsState::Get().sunUpload.sunState & kSunTestOn) != 0;
		cpu->terrain = sample.terrainCpu;
		cpu->terrainNear = terrainNearUpload;
		sample.cpu = cpu;
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu, "DCLF cull counters",
				[cpu = std::move(cpu)](const auto&) { cpu->Run(); }))
			stl::report_and_fail("Drawcall Limit Fix: the culling counters' CPU side was refused by DCLF's executor");
	}
}
