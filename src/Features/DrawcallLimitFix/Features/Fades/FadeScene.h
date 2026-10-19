#pragma once

// Fades' render side: its scene resources (FadeScene: FadeStateCS's buffers, the root programs, the write-back ring, held with the scene's
// tables), the write-back's host side, and the parts RegisterRender registers (FadeRender.cpp), each in its own source. Included by the
// feature's sources that use them.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeLayout.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRecords.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
	}
}

namespace DCLF::Features::Fades
{
	/**
	 * @brief The fade write-back's host side (dclf-architecture.md, "The fade write-back"): its events handed to the write-back task
	 * in batches through a lock-free stack. A frame whose epoch did not run is counted missed: FadeStateCS did not run either, so
	 * nothing is lost.
	 */
	struct FadeWriteBack : Draws::HostEventRing
	{
		FadeWriteBack() :
			HostEventRing(&BytesFor) {}
		struct Batch
		{
			std::vector<FadeEvent> events;
			std::uint32_t frame = 0;  // the scene frame whose FadeStateCS appended them (the list's header)
			Batch* next = nullptr;
		};
		// Render thread: each frame slot's last arm.
		struct Armed
		{
			bool armed = false;
			std::uint32_t frame = 0;
		};
		std::vector<Armed> armed;
		std::atomic<Batch*> batches{ nullptr };
		// The write-back task's alone (one at a time, never joined): per root slot the scene frame of the event last taken, so an
		// older one (a list read late) is not written over it.
		std::vector<std::uint32_t> appliedFrame;
		std::atomic<bool> running{ false };
		/**
		 * @brief What a task found to write: the stores in event order, and the tables they were judged against, held until the main
		 * thread makes them (WriteFadeStores) so no node they name is let go first. The task touches no engine memory.
		 */
		struct Stores
		{
			struct Store
			{
				void* node = nullptr;
				std::uint32_t flags = 0;  // the fade bits (kFadeFlagMask) only
				float fade = 0.0f;
				// A leaf or tree root's LOD state (+0x152, +0x153, +0x14C), written with the fade (lod false: a plain fade root's
				// levels are the node's own, not written).
				bool lod = false;
				std::uint32_t levels = 0;
				float blend = 0.0f;
			};
			std::shared_ptr<const SceneTables> tables;
			std::vector<Store> stores;
		};
		std::atomic<Stores*> ready{ nullptr };
		// Since the last report; notWritten: stores left because DCLF does not write the root (the engine's cull steps it: one writer).
		std::atomic<std::uint64_t> applied{ 0 }, stale{ 0 }, busy{ 0 }, storeNs{ 0 }, notWritten{ 0 }, dropped{ 0 };
		// Since the last report: frames harvested (and those with events), events.
		std::atomic<std::uint64_t> harvested{ 0 }, harvestedWithEvents{ 0 }, events{ 0 };

		void Push(Batch* a_batch)
		{
			a_batch->next = batches.load(std::memory_order_relaxed);
			while (!batches.compare_exchange_weak(a_batch->next, a_batch, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		~FadeWriteBack()
		{
			delete ready.exchange(nullptr);
			for (Batch* b = batches.exchange(nullptr); b;) {
				Batch* following = b->next;
				delete b;
				b = following;
			}
		}
		static std::uint64_t BytesFor(std::uint32_t a_events) { return (kFadeEventHeaderWords + std::uint64_t(a_events) * kFadeEventWords) * sizeof(std::uint32_t); }
	};

	/**
	 * @brief Fades' scene resources (Api::SceneResources "fade roots"), made with the scene's tables when the fade state program loads:
	 * FadeStateCS's buffers by root slot, its frame row and log, the root programs and animation stamps with their mirrors, the published
	 * states, and the write-back ring. Core's cull reads them at its cull bindings (Api::CullBindingSlot's Root* slots), its draws at the
	 * frame bindings t120/t119.
	 */
	struct FadeScene
	{
		// Fade roots (FadeStateCS): static rows by root slot, GPU state rows, the frame's inputs, and CS_DCLF_FADE_PARITY's log.
		Draws::Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
		Draws::Versioned fadeRootPrograms;  // per root slot: its program (Api::MainCullInputContribution::judge)
		Draws::Versioned fadeAnimated;      // per root slot: the scene frame whose animation batch updated it
		org::services::FilledVersion fadeAnimatedFilled;
		// CPU mirrors: only differing runs go up.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeAnimatedCounts;  // per root slot: the batch's updates while it is counted, else 0
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		org::services::FilledContents<> fadeRootProgramsFilled;  // the root and program versions it holds
		// What the programs mirror was judged against: its key (as filled), the roots' content epoch and the input's programsVersion.
		struct ProgramsMirrorOf
		{
			std::uint64_t key = 0, rootsEpoch = 0, programs = 0;
		} fadeRootProgramsMirrorOf;
		// The judge's batch (Api::RootProgramsJudge), kept between commits: the roots judged, the questions (those with a node) and the
		// answers.
		struct RootProgramsBatch
		{
			std::vector<std::uint32_t> roots, asked, programs;
		} fadeRootProgramsBatch;
		// FadeStateCS's published states per frame parity; builds read the previous frame's. Every version is zero-filled (builds
		// then take the static row's state).
		std::array<Draws::Versioned, 2> fadeStatesOut;
		/** @brief The published states a frame's builds read (the previous frame's). */
		const Draws::Versioned& FadeStatesRead(std::uint32_t a_frame) const { return fadeStatesOut[(a_frame + 1) & 1]; }
		org::services::FilledContents<> fadeRootsFilled;  // the root rows' content epoch (the root states' family's journal)
		/** @brief The root rows' content epoch the fadeRoots version a_sealed names holds (~0: none). */
		std::uint64_t FadeRootsContentEpoch(const Draws::NamedVersions& a_sealed) const { return fadeRootsFilled.Since(a_sealed.Of(fadeRoots)).value_or(~0ull); }
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::uint32_t fadeLogBase = ~0u;
		std::shared_ptr<const ComputeProgram> fadeState;
		std::shared_ptr<const ComputeProgram> portalQueries;
		std::shared_ptr<Portals::Batch> portalBatch;
		// Fade write-back: event list (count zeroed by every depth commit), each root's last reported generation and milestone
		// (every version zero-filled: no root reported yet, as a zero generation is never a listing's).
		Draws::Versioned fadeEvents, fadeReported;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
	};

	/** @brief Fades' scene resources in a_scene (null: none made). FadeResources.cpp. */
	FadeScene* SceneOf(const Draws::SceneBuffers& a_scene);
	/** @brief SceneOf, sharing the state's ownership (a pass holds it). */
	std::shared_ptr<FadeScene> SharedSceneOf(const Draws::SceneBuffers& a_scene);

	/**
	 * @brief Render thread: the descriptor of the fade seeds the current frame reads (StructuredBuffer<FadeRootStatic>, two rows a root
	 * slot: FadeSeedRow; 0 before the first frame): the "fade-seeds" pool's backing (its readers take it as the cull binding RootSeeds).
	 * A root's seed is its node as the frame's start had it (its bytes copied at Kick, StaticOf on the copy by the producer). FadeSeeds.cpp.
	 */
	std::uint32_t SeedsIndex();
	/** @brief Render thread: the seed rows as the frame's producer left them, once it is done; a_wait (diagnostics frames): waits for it. */
	const std::vector<FadeRootStatic>* SeedsIfDone(bool a_wait = false);

	/**
	 * @brief Render thread, the frame's start (the root states' kick: Extensions::RootStatesKick): hands the fade events read back since the
	 * last kick to a task on DCLF's executor that judges them into the stores WriteFadeStores makes (dclf-architecture.md, "The fade
	 * write-back"). Without it a stood-in root's node stays as the engine last wrote it for its readers (tree LOD crossfade, occlusion
	 * cull, animation gate). FadeWriteBack.cpp.
	 */
	void KickFadeWriteBack();
	/** @brief Render thread (DrainAsync's root states' wait; never per frame): waits for the fade write-back task, drops what it found. */
	void WaitFadeWriteBack();
	/**
	 * @brief Main thread at Main::Update's entry (no list cull, cell animation or animation job runs): makes the last task's stores on the
	 * roots DCLF writes (a_written: FadeWrittenByDclf; other roots are left to the engine's cull). a_live false (DCLF does not run): dropped.
	 */
	void WriteFadeStores(bool a_live, const std::function<bool(const void*)>& a_written);

	// RegisterRender's parts (FadeRender.cpp, in its order), each in the source of its code:
	/** @brief The seeds: the "fade-seeds" frame-value pool (the fade roots component's seeds). FadeSeeds.cpp. */
	void RegisterSeeds(Api::InstallContext& a_context);
	/** @brief The scene resources "fade roots" (setup, graph, requirements, fit, sealed). FadeResources.cpp. */
	void RegisterResources(Api::InstallContext& a_context);
	/** @brief The depth commit hook: the root rows and FadeStateCS's frame inputs, the write-back armed, the roots' programs. FadeCommit.cpp. */
	void RegisterCommit(Api::InstallContext& a_context);
	/** @brief The passes at the depth epoch's cull state: the fade state, then its events' copy to the host. FadeStage.cpp. */
	void RegisterPasses(Api::InstallContext& a_context);
}
