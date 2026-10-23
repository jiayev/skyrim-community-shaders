// Fades' writer hooks: the writer tables and fade stores at Main::Update's entry, the animation job's fade updates (job threads) and
// their batch at the list jobs' end (render thread), the fade OnVisibles without their stores (cull threads), their stand-in observer
// and installs; and the root test's fade-out distance.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"  // the write-back's WriteFadeStores
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesAddresses.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace DCLF::Features::Fades
{
	namespace
	{
		using namespace Engine;

		// Module offsets, AE 1.6.1170 and SE 1.5.97; field offsets the same on both.
		// NiAVObject / BSFadeNode fields BSFadeNode::OnVisible (AE 0x141479f50) reads before its fade update.
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::uint32_t kFlagFadeTargetReached = 1u << 14;
		constexpr DCLF::Engine::Offset kAnimatedFadeCallSite{ 0x2d0044, 0x27cba4 };  // FUN_1402cff60 (UpdateAnimationJob) -> FUN_14147a160
		// The NiNode OnVisible BSFadeNode's ends in (its children's cull), and fields the fade OnVisible detours read.
		constexpr DCLF::Engine::Offset kNodeOnVisible{ 0xd1e2c0, 0xc58c70 };  // NiNode::OnVisible
		constexpr std::size_t kNodeWorldZ = 0xEC;                 // NiAVObject::world.translate.z
		constexpr std::size_t kProcessCameraUpdates = 0x11C;      // byte: NiCullingProcess::cameraRelatedUpdates
		constexpr std::size_t kFadeLodType = 0x153;               // byte: the LOD type in its low nibble
		// FUN_14147b110 (the fade value) and FUN_14147a160 (the fade update): what the fade-out distance is made of.
		constexpr std::size_t kFadeNear = 0x128;                  // float, on the fade node
		constexpr std::size_t kFadeFar = 0x12C;                   // float
		constexpr std::size_t kFadeAlwaysOut = 0x109;             // byte: bit 0 fades the node out whatever its distance

		// The animation job's fade updates (FUN_1402cff60's call of FUN_14147a160, AnimatedFade) since the last list jobs, with their
		// inputs; at the list jobs' end (DrainAnimatedFades, render thread) they become the batch FadeStateCS applies before its cull
		// test. Lock-free: the nodes are queued; the inputs, the same for every call of one animation update, are a single-writer
		// record (the first caller claims it, writes it, then publishes it), later callers compare against it and set animatedVaried.
		// A caller that finds the record claimed but not yet published queues its inputs for DrainAnimatedFades to compare.
		EventQueue<AnimatedFadeRoot> animatedNodes;
		std::vector<AnimatedFadeRoot> animatedBatch;  // render thread
		enum : std::uint32_t
		{
			kAnimatedEmpty,
			kAnimatedWriting,
			kAnimatedPublished
		};
		std::atomic<std::uint32_t> animatedState{ kAnimatedEmpty };
		AnimatedFadeInputs animatedInputs{};  // written by the claimer only, read once published (and by DrainAnimatedFades)
		std::atomic<std::uint32_t> animatedVaried{ 0 };
		EventQueue<AnimatedFadeInputs, 64> animatedUnpublished;  // compared by DrainAnimatedFades
		AnimatedFadeInputs batchInputs{};  // render thread
		// FadeWrittenByDclf's set: immutable SceneTables from Main::Update's entry, held by the main thread until the next entry
		// (no OnVisible or gate runs then). Lock-free.
		std::atomic<const void*> fadeWriterTables{ nullptr };
		std::shared_ptr<const void> fadeWriterTablesHeld;
		std::atomic<std::uint64_t> onVisibleFadeSkipped{ 0 };
		std::atomic<std::uint64_t> gateFadeSkipped{ 0 };

		/** @brief The fade roots a_tables (SceneTables) lists: the family's columns, read in place (null before its first write). */
		const Scene::FadeRootTables* RootsOf(const void* a_tables)
		{
			return static_cast<const SceneTables*>(a_tables)->families.Columns<Scene::FadeRootTables>(Scene::FadeRootFamilyId());
		}

		/** @brief Main::Update's entry: publishes the accepted tables as the writer set (a_running false: none). */
		void PublishWriterTables(bool a_running)
		{
			// The replaced tables are released after publication: the last cycle was waited for, so no OnVisible or gate reads them.
			std::shared_ptr<const void> tables;
			if (a_running)
				tables = Scene::FrameView::Get().AcceptedTables();
			fadeWriterTables.store(tables.get(), std::memory_order_release);
			fadeWriterTablesHeld = std::move(tables);
		}

		/** @brief The animation job's hook: the update noted (a_node's root in the writer tables); returns whether DCLF writes the root. */
		bool NoteAnimatedFade(const void* a_node, const float* a_camera)
		{
			// One lookup in the writer tables (FadeWrittenByDclf's): the root's slot for the batch, and whether DCLF writes it. A node they
			// do not list is no root of DCLF's this update (one listed later is seeded from the node, this update included).
			bool written = false;
			if (const auto* tables = fadeWriterTables.load(std::memory_order_acquire)) {
				if (const auto* roots = RootsOf(tables)) {
					if (const auto it = roots->fadeRootIndex.find(a_node); it != roots->fadeRootIndex.end() && it->second < roots->fadeRoots.size()) {
						const auto& row = roots->fadeRoots[it->second];
						written = (row.bits & kFadeRootWritten) != 0;
						animatedNodes.Push({ a_node, it->second, row.generation });
					}
				}
			}
			// FUN_14147a160's inputs: the camera FUN_1402cff60 passes (world root camera's position, lodAdjust) and the fade globals;
			// the same for every call of one animation update.
			if (!a_camera)
				return written;
			const AnimatedFadeInputs inputs{ { a_camera[0], a_camera[1], a_camera[2] }, a_camera[3], Global<std::int32_t>(Address::kFadeFrameCounter), Global<float>(Address::kFadeStepTime), 1u };
			std::uint32_t state = animatedState.load(std::memory_order_acquire);
			if (state == kAnimatedEmpty && animatedState.compare_exchange_strong(state, kAnimatedWriting, std::memory_order_acquire)) {
				animatedInputs = inputs;
				animatedState.store(kAnimatedPublished, std::memory_order_release);
				return written;
			}
			if (state == kAnimatedPublished) {
				if (std::memcmp(&inputs, &animatedInputs, offsetof(AnimatedFadeInputs, valid)) != 0)
					animatedVaried.store(1, std::memory_order_relaxed);
				return written;
			}
			// Claimed, not yet published: compared once the jobs are done.
			animatedUnpublished.Push(inputs);
			return written;
		}

		/**
		 * @brief FUN_1402cff60's call of FUN_14147a160: the animation job updates an animated reference's fade node the cull did not
		 * reach (kAccumulated clear), from the main camera.
		 */
		struct AnimatedFade
		{
			static void thunk(void* a_node, float a_amount, const float* a_camera)
			{
				// FadeStateCS makes the same update on the GPU (the animated batch), for every root.
				// One writer: the root keeps the write-back's state; the gate reads its currentFade.
				if (NoteAnimatedFade(a_node, a_camera)) {
					gateFadeSkipped.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				func(a_node, a_amount, a_camera);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) without its stores, for a root DCLF writes. Control flow kept (fades off, or
		 * settled with kIgnoreFade at fade and amount 1: children; camera updates with a LOD-type-6 node in its special state:
		 * children; faded out or zero amount: none), but the fade update (FUN_14147a160 or the type-6 step) and +0x13C are skipped.
		 */
		void WrittenFadeOnVisible(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
		{
			using NodeOnVisible = void (*)(void*, RE::NiCullingProcess*, std::int32_t);
			static const auto children = reinterpret_cast<NodeOnVisible>(REL::Module::get().base() + kNodeOnVisible);
			const float one = Global<float>(Address::kFloatOne);
			const float amount = At<float>(a_node, kFadeAmount);
			const bool settled = (At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadeSettled) && amount == one && At<float>(a_node, kCurrentFade) == one;
			if (!Global<std::uint8_t>(Engine::Address::kFadesOn) || settled) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<std::uint8_t>(a_process, kProcessCameraUpdates) && (At<std::uint8_t>(a_node, kFadeLodType) & 0xF) == 6 &&
				!(Global<float>(Engine::Address::kFadeSpecialA) != Global<float>(Engine::Address::kFadeSpecialB))) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<float>(a_node, kCurrentFade) <= 0.0f || amount == 0.0f)
				return;
			children(a_node, a_process, a_arg);
		}

		struct FadeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSLeafAnimNode::OnVisible (AE 0x14147c9c0): for a root DCLF writes, its leaf LOD step and +0x13C store skipped. */
		struct LeafOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSTreeNode::OnVisible (AE 0x14147d3c0): for a root DCLF writes, the engine's height test kept (a tree above the limit is
		 * not visited), BSLeafAnimNode's without its stores, and the LOD fix-up's stores (+0x153, +0x14C) skipped.
		 */
		struct TreeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				if (At<std::uint8_t>(a_process, Address::kProcessTreeHeightTest) && Global<std::uint8_t>(Address::kTreeHeightTestOn) &&
					At<float>(a_node, kNodeWorldZ) - Global<float>(Address::kTreeHeightBase) > Global<float>(Address::kTreeHeightLimit))
					return;
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The animation job's fade update and the fade OnVisibles, with the primary cull's stand-in (its install answered). */
		void InstallWriterHooks()
		{
			const auto base = REL::Module::get().base();
			if (CallsTo(base + kAnimatedFadeCallSite, base + Engine::Address::kFadeUpdate))
				stl::write_thunk_call<AnimatedFade>(base + kAnimatedFadeCallSite);
			else
				logger::warn("[DCLF] primary cull: the animation job's fade update is not where expected");
			// Fade OnVisibles leave DCLF-written roots alone (detours, not vtable slots, so subclass tables and FadeState::PlanOf keep
			// the engine's addresses).
			if (!Address::kFadeOnVisible.Known() || !Address::kLeafOnVisible.Known() || !Engine::Address::kTreeOnVisible.Known()) {
				logger::warn("[DCLF] primary cull: the fade OnVisibles are not known for this runtime; they keep their own fade stores");
				return;
			}
			stl::detour_thunk<FadeOnVisible>(base + Address::kFadeOnVisible);
			stl::detour_thunk<LeafOnVisible>(base + Address::kLeafOnVisible);
			stl::detour_thunk<TreeOnVisible>(base + Engine::Address::kTreeOnVisible);
		}
	}

	void WriteFadeWriters(bool a_running)
	{
		// The fade write-back on every root the accepted tables list with kFadeRootWritten, published first as the set the gate
		// and fade OnVisibles skip.
		PublishWriterTables(a_running);
		WriteFadeStores(a_running, [](const void* a_root) { return FadeWrittenByDclf(a_root); });
	}

	bool FadeWrittenByDclf(const void* a_node)
	{
		const auto* tables = fadeWriterTables.load(std::memory_order_acquire);
		return tables && WrittenByTables(tables, a_node);
	}

	bool WrittenByTables(const void* a_tables, const void* a_node)
	{
		const auto* roots = RootsOf(a_tables);
		if (!roots)
			return false;
		const auto it = roots->fadeRootIndex.find(a_node);
		return it != roots->fadeRootIndex.end() && it->second < roots->fadeRoots.size() && (roots->fadeRoots[it->second].bits & kFadeRootWritten);
	}

	FadeWriterCounts TakeFadeWriterCounts()
	{
		FadeWriterCounts c;
		c.onVisibleSkipped = onVisibleFadeSkipped.exchange(0, std::memory_order_relaxed);
		c.gateSkipped = gateFadeSkipped.exchange(0, std::memory_order_relaxed);
		if (const auto* tables = fadeWriterTables.load(std::memory_order_acquire))
			if (const auto* roots = RootsOf(tables))
				c.roots = roots->fadeRootIndex.size();
		return c;
	}

	void TakeAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs)
	{
		// Render thread, as DrainAnimatedFades, which made the batch.
		a_roots.swap(animatedBatch);
		animatedBatch.clear();
		a_inputs = std::exchange(batchInputs, {});
	}

	void DrainAnimatedFades()
	{
		// The animation job runs between two culls, so its updates are complete now: its queued nodes and published inputs become
		// the batch, and the record is free for the next update.
		animatedBatch.clear();
		animatedNodes.Drain([&](AnimatedFadeRoot&& a_root) { animatedBatch.push_back(a_root); });
		batchInputs = {};
		if (animatedState.load(std::memory_order_acquire) == kAnimatedPublished) {
			batchInputs = animatedInputs;
			batchInputs.varied = animatedVaried.load(std::memory_order_relaxed);
		}
		animatedUnpublished.Drain([&](AnimatedFadeInputs&& a_inputs) {
			if (batchInputs.valid && std::memcmp(&a_inputs, &batchInputs, offsetof(AnimatedFadeInputs, valid)) != 0)
				batchInputs.varied = 1;
		});
		animatedVaried.store(0, std::memory_order_relaxed);
		animatedState.store(kAnimatedEmpty, std::memory_order_release);
	}

	float FadeDistanceOf(const SceneCapture::NodeRecord& a_root)
	{
		// From the fade node's record and the frame's captured globals, never the engine's.
		const auto& g = FrameGlobals::Current();
		// No fade update at all, or no distance term in the fade value (FUN_14147b110 returns 1).
		if (!g.fadesOn || !g.fadeLodUpdates)
			return 0.0f;
		const std::uint32_t type = a_root.fadeType;
		// Type 8 fades gradually even out of view (FUN_14147a160) and OnVisible's type-6 branch never fades out (ServiceFade): the
		// engine draws either the frame it comes into view.
		if (type == 8 || (type == 6 && g.fadeSpecialA == g.fadeSpecialB))
			return 0.0f;
		const float mult = g.fadeDistanceMult;
		const float nearDistance = mult * a_root.fadeNear;
		const float farDistance = mult * a_root.fadeFar;
		if (!(farDistance > nearDistance))
			return 0.0f;  // the fade value never falls below 1
		// The fade value 1 - (x - near) / (far - near), x the scaled distance, reaches the threshold at x = limit.
		const float limit = nearDistance + (1.0f - g.fadeOutThreshold) * (farDistance - nearDistance);
		const float divisor = g.fadeTypeDivisors[type];
		// x = distance * lodFactor / divisor, or distance * the default scale when the divisor is not positive.
		const float distance = divisor > 0.0f ? limit * divisor : -(limit / g.fadeDefaultScale);
		return std::isfinite(distance) ? distance : 0.0f;
	}

	void RegisterWriters(Api::InstallContext& a_context)
	{
		// Last of the observers, so Main::Update's entry keeps its order: kAccumulated, the other observers' updateEntry (Portals' live
		// flag), the writer tables and stores, then the UpdateEntry event and afterWrites. Without the primary cull neither point runs,
		// and none of the writer hooks goes in (as when its install wrote them).
		if (!a_context.RegisterStandInObserver({ .updateEntry = &WriteFadeWriters, .listJobsDone = &DrainAnimatedFades }))
			return;
		InstallWriterHooks();
	}
}
