#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"

#include <functional>

namespace org::runtime
{
	class IUploadService;
}

namespace DCLF
{
	struct FrameGlobals;
	struct BindlessPlacement;
	struct BindlessShading;

	/**
	 * @brief FrameValues(N) (dclf-async-publication.md, "FrameValues"): the values a frame's draws need to be its own - the
	 * claimed members' placements (BindlessPlacement), the skins' palettes and the objects' shading (BindlessShading) - made on
	 * DCLF's pool while the frame runs, and waited for by the GPU, never by the render thread. The scene's tables hold none of
	 * them: these are the only ones drawn with.
	 *
	 * Each frame the render thread kicks one producer (Kick, at the scene's start) with the newest placement plan the walk made
	 * (SceneStore::PlacementPlan). The producers run one after another on the preparation pool:
	 * - they sample the plan's movers and roots, and the slots its walk wrote in full (once), across the pool, under engine-read
	 *   leases: a skin's palette after the engine's palette update (which the native draw's bone setter would have run), into the
	 *   rows kept by object slot and the palette rows kept by block (journalled KeptArrays); and the shading of the slots the walk
	 *   and the accumulate phase named (SceneStore::ShadingItem), with the wetness the frame's start captured;
	 * - wait, on the worker, until the frame that last read their ring buffers is done on the GPU (the buffers' GPU point);
	 * - send those buffers the runs they lack on the dedicated upload queue, then signal the frame-wait timeline to the frame's
	 *   sequence number (always, also after a failure), which every batch of the frame waits for (PersistentGraphHost's frame wait).
	 *
	 * The rows go to a ring of buffers, one per frame in flight, each holding every row: frame n's draws read buffer n % kRing (the
	 * frame record's t123, t122 and t121, and the culling's latches), which no producer writes while a frame reads it.
	 */
	class FrameValues
	{
	public:
		static constexpr std::uint32_t kRing = 4;
		/** @brief One float4 of a palette (three a bone): DCLFPalettes' element. */
		struct PaletteRow
		{
			float v[4];
		};

		static FrameValues& Get();

		/**
		 * @brief An object's placement row from the engine now: its geometry's transforms and bound, its sun entry node's bound
		 * (unbounded without one), its fade property's fade node (the layer property for a layer's row).
		 */
		static void SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_sunEntryNode, const RE::BSShaderProperty* a_fadeProperty,
			BindlessPlacement& a_out);
		/** @brief Render thread (diagnostics): a slot's row from the engine now, by the tables' structure; false for a free slot. */
		static bool SampleSlot(const SceneStore::Tables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out);

		/**
		 * @brief Render thread, at the scene's start (the engine's update done, the window open): the frame's producer, from a_plans
		 * (the walks' plans the publication log brought since the last frame, oldest first: each one's written slots sampled, the newest
		 * drawn with; none: the newest before them), the slots named for their shading (a_shading, the newest last) and
		 * the wetness captured (a_wetness). Sets the host's frame wait to its sequence number. False when it cannot run (no render
		 * graph, no dedicated upload queue): the frame waits for nothing, and its draws have no rows; what it was handed waits for the
		 * first producer.
		 */
		/**
		 * @brief What else the frame's producer uploads before it signals the frame's wait (step 6e E4: the payload ring entry the
		 * frame's epochs read), on the producer's thread and the dedicated uploader. A throw is logged; the signal always goes.
		 */
		using FrameUploads = std::function<void(org::runtime::IUploadService&)>;
		bool Kick(std::vector<std::shared_ptr<const SceneStore::PlacementPlan>> a_plans, std::vector<SceneStore::ShadingItem> a_shading,
			std::vector<SceneStore::WetnessValue> a_wetness, std::vector<SceneStore::FadeSeedItem> a_seeds, std::vector<SceneStore::TreeSeedItem> a_treeSeeds,
			std::shared_ptr<const FrameGlobals> a_globals, FrameUploads a_uploads = {});
		/** @brief Render thread: a frame with no producer (DCLF not running): no batch waits for one. */
		void Skip();
		/** @brief Render thread, at Present: the GPU point of everything the frame submitted (its ring buffers' readers). */
		void EndFrame();
		/** @brief Render thread: the descriptor of the placement buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t PlacementsIndex() const { return frameIndex; }
		/** @brief Render thread: the descriptor of the palette buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t PalettesIndex() const { return paletteIndex; }
		/** @brief Render thread: the descriptor of the shading buffer the current frame's draws read (0 before the first frame). */
		std::uint32_t ShadingIndex() const { return shadingIndex; }
		/**
		 * @brief Render thread: the descriptor of the fade seeds the current frame reads (T6b1a: StructuredBuffer<FadeRootStatic>, two rows
		 * a root slot: FadeSeedRow; 0 before the first frame). A root's seed is its node as the frame's start had it (StaticOf, at Kick).
		 */
		std::uint32_t FadeSeedsIndex() const { return seedsIndex; }
		/** @brief Render thread: the tree seeds likewise (StructuredBuffer<TreeStatic>, two rows a tree slot: MergeTreeSeed). */
		std::uint32_t TreeSeedsIndex() const { return treeSeedsIndex; }
		const std::vector<TreeStatic>* TreeSeedsIfDone(bool a_wait = false) const;
		/** @brief Render thread: the seed rows as the frame's producer left them, once it is done; a_wait (the fade parity's frames): waits for it. */
		const std::vector<FadeRootStatic>* FadeSeedsIfDone(bool a_wait = false) const;
		bool Available() const;

		/**
		 * @brief Render thread, CS_DCLF_PERSISTENT_PARITY frames: waits for the frame's producer, then compares its rows and palettes
		 * with the engine's now (SampleSlot; the skins' palettes as this frame's update left them): a difference is a placement or a
		 * palette the producer missed or took late. a_fresh: the plan the walk made this frame (its slots' rows are the next frame's).
		 */
		void CheckParity(const SceneStore::Tables& a_tables, const SceneStore::PlacementPlan* a_fresh);
		/**
		 * @brief Render thread: the rows as the frame's producer left them, once it is done (diagnostics); null while it runs. Valid
		 * until the next Kick.
		 */
		const std::vector<BindlessPlacement>* RowsIfDone() const;
		/** @brief Render thread: the palette rows likewise (PaletteRowsOf places a block's). */
		const std::vector<PaletteRow>* PalettesIfDone() const;
		/** @brief Render thread: the shading rows likewise; a_wait (diagnostics frames): waits for the frame's producer first. */
		const std::vector<BindlessShading>* ShadingIfDone(bool a_wait = false) const;
		/** @brief The report every 300 frames (empty when nothing ran). */
		std::string Report();

	private:
		FrameValues();
		~FrameValues();
		struct Impl;
		std::unique_ptr<Impl> impl;
		std::uint32_t frameIndex = 0, paletteIndex = 0, shadingIndex = 0, seedsIndex = 0, treeSeedsIndex = 0;
	};
}
