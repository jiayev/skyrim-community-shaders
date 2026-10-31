// Fades' seeds: a frame-value pool ("fade-seeds", the last of the pools: Fades registers after every feature) of the fade roots
// component's seeds, the listed roots' nodes copied on the render thread at each Kick and made into rows (FadeState::StaticOf) by
// FrameValues' producer; FadeStateCS and the builds read them as the cull binding RootSeeds.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::Fades
{
	namespace
	{
		// An engine reference a producer is done with, to the render thread (EngineReleases, at Present).
		template <class T>
		void ReleaseLater(RE::NiPointer<T>& a_reference)
		{
			if (a_reference)
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(a_reference)));
		}

		/**
		 * @brief A fade root's node as the frame's start had it: its bytes (FadeState::NodeBytes), copied by the render thread before the
		 * frame's culls update the node, for the producer's StaticOf (a pure read of them, the tree LOD switch's index aside, which no
		 * cull writes). Holds the node (the switch's owner) until the producer is done.
		 */
		struct SeedCopy
		{
			alignas(16) std::array<std::byte, FadeState::kMaxNodeBytes> bytes{};
			RE::NiPointer<RE::NiAVObject> node;
			std::shared_ptr<const FrameGlobals> globals;  // the frame's (StaticOf's LOD scale)
			std::uint32_t row = 0, generation = 0;
		};

		/**
		 * @brief What a kick hands its producer: the copies taken since the last kick (in request order), and the rows they name (highest
		 * named FadeSeedRow + 1). Its engine references go to the render thread (EngineReleases) whenever it goes, never dropped on a worker.
		 */
		struct SeedInput
		{
			std::vector<SeedCopy> values;
			std::uint32_t rows = 0;

			~SeedInput()
			{
				for (auto& copy : values)
					ReleaseLater(copy.node);
			}
		};

		Api::FrameValuePoolId fadeSeedsPool;  // install

		// Render thread: the copies no kick handed over yet (in request order), the most rows named, and the input a kick hands when
		// nothing is new (kept while the rows stand: no allocation a frame).
		std::vector<SeedCopy> unsentSeeds;
		std::uint32_t seedRows = 0;
		std::shared_ptr<const SeedInput> idleSeeds;

		// The producers', one at a time in frame order: the seed rows (by FadeSeedRow), and the copies a refused lease left unread.
		KeptWholeArray<FadeRootStatic> fadeSeeds;
		std::vector<SeedCopy> owedSeeds;

		// Render thread, every Kick: the node as the frame's start has it, before this frame's culls update it (FadeStateCS's first
		// update is this frame's): its bytes copied here, the row made from the copy by the producer (StaticOf, with the frame's globals:
		// its LOD scale).
		void FadeSeedsFrameStart(const Api::FrameValuePoolFrameStart& a_start)
		{
			auto seeds = Scene::FrameView::Get().TakeComponentSeeds(Scene::FadeRootComponentId());
			for (auto& seed : seeds) {
				if (!seed.node)
					continue;
				auto& copy = unsentSeeds.emplace_back();
				std::memcpy(copy.bytes.data(), seed.node.get(), FadeState::NodeBytes(FadeState::PlanOf(*seed.node)));
				copy.node = std::move(seed.node);
				copy.globals = a_start.globals;
				copy.row = seed.row;
				copy.generation = seed.generation;
				seedRows = std::max(seedRows, seed.row + 1);
			}
		}

		// Render thread, at Kick: every copy taken since the last kick, for the frame's producer.
		Api::FrameValueInput FadeSeedsKick(const Api::FrameValuePoolKick&)
		{
			if (!unsentSeeds.empty()) {
				auto input = std::make_shared<SeedInput>();
				input->values = std::exchange(unsentSeeds, {});
				input->rows = seedRows;
				return { std::move(input), seedRows };
			}
			if (!idleSeeds || idleSeeds->rows != seedRows) {
				auto input = std::make_shared<SeedInput>();
				input->rows = seedRows;
				idleSeeds = std::move(input);
			}
			return { idleSeeds, seedRows };
		}

		void FadeSeedsSample(Api::FrameValuePoolProducer& a_producer)
		{
			ZoneScopedN("CS.DCLF.FrameValues.FadeSeeds");
			a_producer.BeginBuild(fadeSeeds);
			auto& out = fadeSeeds.Mutable();
			const auto* input = a_producer.Input<SeedInput>();
			const std::uint32_t rows = input ? input->rows : 0u;
			if (out.size() < rows) {
				const std::size_t first = out.size();
				out.resize(rows, FadeRootStatic{});
				fadeSeeds.MarkRange(first, rows - first);
			}
			// In request order, those a refused lease left first: a later value for the same row wins. StaticOf reads the copy the frame's
			// start took (and the tree LOD switch it names); once a lease is refused (the window closed) the rest wait, in order, for the next
			// producer.
			auto carried = std::move(owedSeeds);
			owedSeeds.clear();
			bool refusedHere = false;
			std::uint64_t seeded = 0;
			const auto seed = [&](const SeedCopy& a_copy) {
				if (!refusedHere) {
					EngineReadWindow::Lease lease;
					if (lease) {
						FrameGlobals::Scope scope(a_copy.globals);
						FadeRootStatic row = FadeState::StaticOf(*reinterpret_cast<const RE::NiAVObject*>(a_copy.bytes.data()));
						row.generation = a_copy.generation;
						if (a_copy.row < out.size()) {
							out[a_copy.row] = row;
							fadeSeeds.Mark(a_copy.row);
							++seeded;
						}
						return;
					}
					refusedHere = true;
					FrameValues::Get().NoteRefused();
				}
				owedSeeds.push_back(a_copy);
			};
			for (const auto& copy : carried)
				seed(copy);
			if (input)
				for (const auto& copy : input->values)
					seed(copy);
			a_producer.NoteSeeded(seeded);
			// The carried copies' references to the render thread (the input's go with the input).
			for (auto& copy : carried)
				ReleaseLater(copy.node);
		}

		void FadeSeedsSend(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(fadeSeeds);
		}
	}

	void RegisterSeeds(Api::InstallContext& a_context)
	{
		fadeSeedsPool = a_context.RegisterFrameValuePool({ .name = "fade-seeds", .stride = sizeof(FadeRootStatic), .frameStart = &FadeSeedsFrameStart,
			.kick = &FadeSeedsKick, .sample = &FadeSeedsSample, .send = &FadeSeedsSend });
	}

	std::uint32_t SeedsIndex()
	{
		return FrameValues::Get().PoolIndex(fadeSeedsPool);
	}

	const std::vector<FadeRootStatic>& ProducerSeeds()
	{
		return fadeSeeds.Get();
	}

	const std::vector<FadeRootStatic>* SeedsIfDone(bool a_wait)
	{
		return FrameValues::Get().ProducerDone(a_wait) ? &fadeSeeds.Get() : nullptr;
	}
}
