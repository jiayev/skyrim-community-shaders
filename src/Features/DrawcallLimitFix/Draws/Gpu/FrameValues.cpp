// FrameValues (see the header): the render thread's Kick and the producers it dispatches onto the preparation pool, one at a time.

#include "FrameValues.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"

#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"

#include "Features/DrawcallLimitFix/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Engine/Capture/Billboards.h"
#include "Features/DrawcallLimitFix/Engine/Capture/ParticleSnapshots.h"
#include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <bit>
#include <cmath>
#include <optional>
#include <cstring>
#include <limits>

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/Gpu/FrameValueMoves.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/PooledBackings.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#	include "Features/DrawcallLimitFix/Engine/Capture/SceneEvents.h"
#	include "Features/DrawcallLimitFix/Runtime/FrameRetirement.h"
#	include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#	include "Features/DrawcallLimitFix/Runtime/Switches.h"
#	include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/Runtime/IUploadService.h>
#	include <Resources/Buffers/Buffer.h>
#	include <ORGModuleServices/BackingPool.h>
#	include <ORGModuleServices/GrowthPolicy.h>
#	include <rhi.h>

#	include <algorithm>
#	include <ankerl/unordered_dense.h>
#	include <chrono>
#endif

namespace DCLF
{
	namespace
	{
		// First-person particle expansion counters, for the first-person report.
		std::atomic<std::uint64_t> firstPersonParticleExpansions{ 0 }, firstPersonParticleCount{ 0 };
		std::atomic<std::uint32_t> firstPersonParticleFarthest{ 0 };  // non-negative float bits (ordered as the float)

		// Row-major 3x4, as the shaders read a transform.
		void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		// The node's bound, or unbounded (radius +max, inside every process).
		void StoreSunEntry(const RE::NiAVObject* a_node, float (&a_out)[4])
		{
			if (a_node && a_node->worldBound.radius >= 0.0f) {
				const auto& bound = a_node->worldBound;
				a_out[0] = bound.center.x;
				a_out[1] = bound.center.y;
				a_out[2] = bound.center.z;
				a_out[3] = bound.radius;
			} else {
				a_out[0] = a_out[1] = a_out[2] = 0.0f;
				a_out[3] = std::numeric_limits<float>::max();
			}
		}

		void StoreFadeNode(const RE::BSShaderProperty* a_property, float (&a_out)[4])
		{
			const auto node = LodFadeNodeOf(a_property);
			std::memcpy(a_out, node.data(), sizeof(a_out));
		}

		/** @brief a_geometry's depth-first index among a_node's geometries (null slots skipped); false if not found. */
		bool OrdinalUnder(const RE::NiNode& a_node, const RE::BSGeometry& a_geometry, std::uint32_t& a_index)
		{
			for (const auto& child : a_node.GetChildren()) {
				auto* object = child.get();
				if (!object)
					continue;
				if (object == &a_geometry)
					return true;
				if (object->AsGeometry())
					++a_index;
				else if (const auto* node = object->AsNode(); node && OrdinalUnder(*node, a_geometry, a_index))
					return true;
			}
			return false;
		}

		// The ordered-node interleave (BindlessPlacement::forwardSort; engine walk in drawcall-limit-fix.md): a blended leaf's nearest
		// BSOrderedNode (exact type, FUN_1415039f0) opens a group whose children draw in child order; the groups are sorted by the
		// node's world-bound centre depth (comparator 0x1414f55e0) and interleaved with the blended list. Only blended leaves join a
		// group (FUN_1414b2330), so an opaque leaf's row stays 0. The bound is live unless kFixedBound (0x141503a60). Read inside the
		// caller's engine-read lease.
		void StoreForwardSort(const RE::BSGeometry& a_geometry, float (&a_out)[4])
		{
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0.0f;
			const auto& data = a_geometry.GetGeometryRuntimeData();
			const auto* alpha = data.alphaProperty.get();
			bool blended = alpha && (alpha->alphaFlags & 1);
			if (!blended)
				if (const auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()); lighting && lighting->material)
					blended = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material)->materialAlpha < 1.0f;
			if (!blended)
				return;
			static const REL::Relocation<const RE::NiRTTI*> orderedRtti{ RE::BSOrderedNode::Ni_RTTI };
			const RE::NiNode* ordered = nullptr;
			for (auto* node = a_geometry.parent; node; node = node->parent)
				if (node->GetRTTI() == orderedRtti.get()) {
					ordered = node;
					break;
				}
			std::uint32_t index = 0;
			if (!ordered || !OrdinalUnder(*ordered, a_geometry, index))
				return;
			const auto& bound = ordered->worldBound;
			a_out[0] = bound.center.x;
			a_out[1] = bound.center.y;
			a_out[2] = bound.center.z;
			a_out[3] = std::bit_cast<float>(index + 1);
		}
	}

	FrameValues::FirstPersonParticleStats FrameValues::TakeFirstPersonParticleStats()
	{
		FirstPersonParticleStats out;
		out.expansions = firstPersonParticleExpansions.exchange(0, std::memory_order_relaxed);
		out.particles = firstPersonParticleCount.exchange(0, std::memory_order_relaxed);
		out.farthest = std::bit_cast<float>(firstPersonParticleFarthest.exchange(0, std::memory_order_relaxed));
		return out;
	}

	void FrameValues::SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_sunEntryNode, const RE::BSShaderProperty* a_fadeProperty,
		BindlessPlacement& a_out)
	{
		StoreTransform(a_geometry.world, a_out.world);
		// Render flag 0x10: the previous transform is the current one (engine notes: SetupGeometry).
		if (GeometryPort::kMainPassRenderFlags & 0x10)
			std::memcpy(a_out.previousWorld, a_out.world, sizeof(a_out.world));
		else
			StoreTransform(DrawnPreviousWorld(a_geometry), a_out.previousWorld);
		a_out.bound[0] = a_geometry.worldBound.center.x;
		a_out.bound[1] = a_geometry.worldBound.center.y;
		a_out.bound[2] = a_geometry.worldBound.center.z;
		a_out.bound[3] = a_geometry.worldBound.radius;
		StoreSunEntry(a_sunEntryNode, a_out.sunEntry);
		StoreFadeNode(a_fadeProperty, a_out.lodFadeNode);
		StoreForwardSort(a_geometry, a_out.forwardSort);
		// A billboard's unturned world, base and pivot (Billboards.h), sampled regardless of the toggle so rows never depend on it.
		Billboards::SampleRow(a_geometry, a_out);
	}

	bool FrameValues::SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out)
	{
		const auto* geometry = a_slot < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		// A layer's row is its base's placement with its layer property's fade node (WriteLayer).
		const RE::BSShaderProperty* fadeProperty = a_tables.IsLayer(a_slot) ? LayerPropertyOf(*geometry) : geometry->GetGeometryRuntimeData().shaderProperty.get();
		SampleRow(*geometry, a_slot < a_tables.sunEntryNode.size() ? a_tables.sunEntryNode[a_slot] : nullptr, fadeProperty, a_out);
		return true;
	}

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
	namespace
	{
		using Plan = PlacementPlan;
		using PaletteRow = FrameValues::PaletteRow;

		/**
		 * @brief The engine's per-frame palette update (AE FUN_140e4ff90), which the native draw's bone setter runs and an owned skin
		 * misses. Idempotent per frame (frameID) under the skin's critical section, so any thread may run it; copies current to
		 * previous, then writes three float4 rows per bone in absolute world space.
		 */
		void UpdateSkin(RE::NiSkinInstance* a_skin, const RE::NiTransform& a_world)
		{
			using UpdateSkinInstance = void (*)(RE::NiSkinInstance*, const RE::NiTransform*);
			static const REL::Relocation<UpdateSkinInstance> updateSkinInstance{ REL::Offset(Engine::Address::kUpdateSkinInstance) };
			updateSkinInstance(a_skin, &a_world);
		}

		using FrameLeases = std::array<org::services::BackingLease, FrameValues::kPools>;

		// Render thread: binds a frame's backings to everything submitted so far (the frame's last submission) and lets them go, so
		// a backing is not reused before that frame completed on the GPU.
		void BindFrameLeases(FrameLeases& a_leases)
		{
			const auto point = SubmittedRetirementPoint();
			for (auto& lease : a_leases)
				if (lease) {
					lease.Bind(point);
					lease.Reset();
				}
		}

		void SampleItem(const Plan::Item& a_item, BindlessPlacement& a_row, BindlessPlacement& a_layer)
		{
			const auto& geometry = *a_item.geometry;
			FrameValues::SampleRow(geometry, a_item.sunEntryNode.get(), geometry.GetGeometryRuntimeData().shaderProperty.get(), a_row);
			if (a_item.layerSlot != kNoObjectSlot) {
				a_layer = a_row;
				StoreFadeNode(LayerPropertyOf(geometry), a_layer.lodFadeNode);
			}
		}

		/**
		 * @brief The move keys a plan's mover or root answers to (read inside the caller's engine-read lease): the object itself, every
		 * reference on its chain (MoveEvents' writers key a reference, a Havok write names the one above the node), and with none every
		 * node above it (a cell's update pass keys its dynamic node, a Havok write the node). A superset of the scene pass's keys
		 * (Tracked::moveKey, moveKeyOuter, the category node), so whatever the light path would take, the producer samples.
		 */
		void KeysOf(const RE::NiAVObject& a_object, std::vector<const void*>& a_out)
		{
			a_out.clear();
			a_out.push_back(&a_object);
			bool reference = false;
			for (const RE::NiAVObject* at = &a_object; at; at = at->parent)
				if (const void* owner = at->GetUserData()) {
					a_out.push_back(owner);
					reference = true;
				}
			if (!reference)
				for (const RE::NiAVObject* at = a_object.parent; at; at = at->parent)
					a_out.push_back(at);
			std::sort(a_out.begin(), a_out.end());
			a_out.erase(std::unique(a_out.begin(), a_out.end()), a_out.end());
		}

		/** @brief The fields two placement rows differ in (the moved-only parity's first difference). */
		std::string PlacementDifferences(const BindlessPlacement& a_expected, const BindlessPlacement& a_actual)
		{
			std::string text;
			const auto field = [&](const char* a_name, const auto& a_e, const auto& a_a) {
				if (std::memcmp(a_e, a_a, sizeof(a_e)) != 0)
					text += fmt::format("{}{}", text.empty() ? "" : ", ", a_name);
			};
			field("world", a_expected.world, a_actual.world);
			field("previousWorld", a_expected.previousWorld, a_actual.previousWorld);
			field("bound", a_expected.bound, a_actual.bound);
			field("sunEntry", a_expected.sunEntry, a_actual.sunEntry);
			field("lodFadeNode", a_expected.lodFadeNode, a_actual.lodFadeNode);
			field("forwardSort", a_expected.forwardSort, a_actual.forwardSort);
			return text;
		}

		// An engine reference a producer is done with, to the render thread (EngineReleases, at Present).
		template <class T>
		void ReleaseLater(RE::NiPointer<T>& a_reference)
		{
			if (a_reference)
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(a_reference)));
		}
	}

	// The skin's palettes after this frame's update; false when its size mismatches its block (the walk rewrites it: KeepSkin).
	bool FrameValues::SkinPalettes(const RE::BSGeometry& a_geometry, std::uint32_t a_rows, const float*& a_current, const float*& a_previous)
	{
		auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
		if (!skin)
			return false;
		a_current = static_cast<const float*>(skin->boneMatrices);
		a_previous = static_cast<const float*>(skin->prevBoneMatrices);
		return a_current && a_previous && skin->numMatrices * 3 == a_rows;
	}

	struct FrameValues::Impl
	{
		enum Part : std::size_t
		{
			kPlacements,
			kPalettes,
			kShading,
			kFadeSeeds,
			kTreeSeeds
		};
		static constexpr std::array<const char*, kPools> kPartNames{ "placements", "palettes", "shading", "fade-seeds", "tree-seeds" };
		static constexpr std::array<std::uint32_t, kPools> kStrides{ sizeof(BindlessPlacement), sizeof(PaletteRow), sizeof(BindlessShading),
			sizeof(FadeRootStatic), sizeof(TreeStatic) };

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
		 * @brief One row set's pool (D6), on DCLF's retirement clock, no caps: the render thread (Kick) its one acquirer, the
		 * producers fill what they are handed. Released backings are destroyed on the graph host's cleanup lane (PooledBackings).
		 */
		struct Pool
		{
			std::shared_ptr<org::services::BackingPool> pool;  // made with Impl
			std::uint64_t capacity = 0;                        // render thread: the class last asked (GrowthPolicy::Doubling(1024))
			std::atomic<std::uint64_t> newestContentEpoch{ 0 };  // the content epoch the newest producer brought its backing to
			std::atomic<std::uint64_t> floor{ 0 };            // the trim floor at the last Kick (diagnostics)
			std::atomic<std::uint64_t> acquires{ 0 }, made{ 0 }, runs{ 0 }, bytes{ 0 };  // since the last take
		};

		Impl()
		{
			for (std::size_t part = 0; part < kPools; ++part)
				pools[part].pool = org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
					{ .name = fmt::format("cs.dclf.frame-values.{}", kPartNames[part]), .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
		}

		// Render thread.
		std::array<Pool, kPools> pools;
		FrameLeases frameLeases;  // the kicked frame's backings, bound at EndFrame
		std::shared_ptr<const Plan> plan;                              // the newest
		std::uint64_t planSerial = 0;                                  // bumped with every newest plan (the producers' index follows it)
		std::vector<const Plan::Item*> firstPersonMovers;              // the newest plan's first-person movers (Kick's samples)
		std::uint64_t firstPersonSerial = 0;                           // the plan serial firstPersonMovers is of
		std::vector<std::shared_ptr<const Plan>> unsampled;           // plans whose written slots no producer has sampled yet
		std::vector<ShadingItem> unsampledShading;        // not yet taken by a producer
		std::vector<WetnessValue> unsampledWetness;
		std::vector<SeedCopy> unsentSeeds;  // not yet taken by a producer, in request order
		std::uint32_t seedRows = 0;         // highest named FadeSeedRow + 1
		std::vector<std::pair<std::uint32_t, TreeStatic>> unsentTreeSeeds;
		std::uint32_t treeSeedRows = 0;
		std::deque<std::pair<std::uint64_t, std::shared_ptr<const Plan>>> plans;  // held per kicked producer's sequence
		std::uint64_t seq = 0;                                          // the last kicked
		bool kicked = false;                                            // this frame
		bool unavailableLogged = false;

		// The producers', one at a time in sequence order.
		KeptArray<BindlessPlacement> rows;
		KeptArray<PaletteRow> palettes;
		KeptArray<BindlessShading> shading;
		KeptArray<FadeRootStatic> seeds;  // by FadeSeedRow
		KeptArray<TreeStatic> treeSeeds;  // by 2 * tree slot + seedOdd
		std::vector<std::uint8_t> listed;                       // dedup marks while listing items: per slot
		std::vector<std::uint8_t> blockListed;                  // per palette block
		std::vector<std::uint8_t> shadingListed;                // per slot, shading items
		// Written slots whose previous transform still differs from the current (a static's first update): resampled until equal.
		std::vector<Plan::Item> settling;
		std::vector<std::uint32_t> listedSlots, listedBlocks;  // the marks set in listed and blockListed (cleared alone)

		// The moved-only sampling (dclf B). The index from move key to the plan's movers and roots (KeysOf), built when a producer
		// first takes a plan: each plan is the whole set, so it is built again, never patched.
		struct Keyed
		{
			std::vector<std::uint32_t> movers, roots;  // indices into the plan's
		};
		ankerl::unordered_dense::map<const void*, Keyed> keyIndex;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> slotRoots;  // a dependent's slot (and its layer's): its roots
		std::uint64_t indexedPlan = 0;  // the plan serial keyIndex is of (0: none)
		// Movers and roots whose keys a refused lease left unread, and particle systems: due every frame of the plan.
		std::vector<std::uint32_t> unkeyedMovers, unkeyedRoots, particleMovers;
		// What the indexed plan's items name, to tell what a new plan adds or changes (raw pointers, compared, never dereferenced).
		struct ItemRecord
		{
			const void* geometry = nullptr;
			const void* sunEntry = nullptr;
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			bool firstPerson = false, particles = false;
			bool operator==(const ItemRecord&) const = default;
		};
		static ItemRecord RecordOf(const Plan::Item& a_item)
		{
			return { a_item.geometry.get(), a_item.sunEntryNode.get(), a_item.slot, a_item.layerSlot, a_item.boneOffset, a_item.boneRows, a_item.firstPerson,
				a_item.particles };
		}
		ankerl::unordered_dense::map<const void*, ItemRecord> moverRecords;              // by geometry
		ankerl::unordered_dense::map<const void*, std::vector<ItemRecord>> rootRecords;  // by root node: its dependents
		std::vector<std::uint32_t> newMovers, newRoots;  // what the newly indexed plan adds or changes: due once
		std::vector<const void*> drainedKeys;  // this producer's keys (scratch)
		std::vector<const void*> lastKeys;     // the previous producer's: the gate is "this frame or the last"
		// Keys due at the next producer: a mover's geometry or a root's node whose sample changed a row or palette (so it settles: a skin's
		// previous palette, a placement's previous world), or whose lease was refused.
		std::vector<const void*> owedKeys;
		std::vector<RE::NiPointer<RE::NiAVObject>> owedNodes;  // node events a refused lease left unnamed
		std::vector<SeedCopy> owedSeeds;                       // fade seeds a refused lease left unread
		std::vector<std::uint8_t> moverDue, rootDue;           // per plan item, set by dueMovers / dueRoots (cleared alone)
		std::vector<std::uint32_t> dueMovers, dueRoots;
		// The moved-only parity (Policy::SampleEveryMover), since its last line.
		struct MovedParity
		{
			std::uint64_t checks = 0, rows = 0, rowsDiffer = 0, palettes = 0, palettesDiffer = 0, rootRows = 0, rootRowsDiffer = 0;
			std::uint64_t movers = 0, moversDue = 0, roots = 0, rootsDue = 0;
			std::string first;
		} movedParity;
		std::atomic<std::uint64_t> done{ 0 };

		// Since the last report.
		std::atomic<std::uint64_t> frames{ 0 }, sampled{ 0 }, rowsChanged{ 0 }, skins{ 0 }, paletteRowsChanged{ 0 }, shadingSampled{ 0 }, shadingChanged{ 0 },
			wetnessChanged{ 0 }, defects{ 0 }, runs{ 0 }, bytes{ 0 }, refused{ 0 }, failures{ 0 }, settled{ 0 }, seeded{ 0 };
		std::atomic<std::uint64_t> movers{ 0 }, moversDue{ 0 }, roots{ 0 }, rootsDue{ 0 }, moveKeys{ 0 }, moveNodes{ 0 };
		std::atomic<std::uint64_t> sampleUs{ 0 }, uploadUs{ 0 }, maxTotalUs{ 0 };

		/**
		 * @brief A first-person member's rows and palettes, sampled by the render thread at Kick, before Main::Draw moves the
		 * skeleton into its root's space.
		 */
		struct FirstPersonSample
		{
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			BindlessPlacement row{}, layer{};
			std::vector<PaletteRow> current, previous;
		};

		struct Job
		{
			std::uint64_t seq = 0;
			const Plan* plan = nullptr;
			std::uint64_t planSerial = 0;  // the render thread's for plan
			bool parity = false;           // Policy::SampleEveryMover
			std::vector<const Plan*> written;  // plans whose written slots it also samples (oldest first)
			std::vector<ShadingItem> shadingItems;  // newest last
			std::vector<WetnessValue> wetness;      // in capture order
			std::vector<SeedCopy> seedValues;  // copied at the frame's start, in request order
			std::uint32_t seedRows = 0;
			std::vector<std::pair<std::uint32_t, TreeStatic>> treeSeedValues;
			std::uint32_t treeSeedRows = 0;
			std::shared_ptr<const FrameGlobals> globals;  // the frame's engine globals (shading rows' tree wind)
			// Per part: the backing the frame's draws read (leased until the producer is done), and the journal's trim floor (the
			// oldest content a backing in circulation held at Kick; nullopt: none holds any, the journal keeps nothing).
			FrameLeases leases;
			std::array<std::optional<std::uint64_t>, kPools> oldestContentEpochs;
			std::shared_ptr<rhi::TimelinePtr> timeline;
			std::shared_ptr<org::runtime::IUploadService> uploads;
			std::vector<FirstPersonSample> firstPerson;  // the render thread's samples (Kick)
			ParticleSnapshots::Camera particleCamera;    // the main camera particle quads face
			// The camera first-person particle quads face: the main camera's rotation (the first-person camera's, FUN_1414cb020)
			// at the first-person eye (DrawWorld +0x00).
			ParticleSnapshots::Camera firstPersonParticleCamera;
		};
		void Run(const Job& a_job);
		void Sample(const Job& a_job);
		void IndexPlan(const Job& a_job);
		void MarkMover(std::uint32_t a_mover);
		void MarkRoot(std::uint32_t a_root);
		void MarkDue(const Job& a_job);
		void CheckEveryMover(const Job& a_job);
		void SampleShadingRows(const Job& a_job, std::size_t a_slots);
		void SampleFadeSeeds(const Job& a_job);
		void Upload(const Job& a_job);
		template <class T>
		void Send(const Job& a_job, Part a_part, const KeptArray<T>& a_rows);
		/** @brief The producer's: a_rows' build opened at the job's floor for a_part (every change forgotten without one). */
		template <class T>
		static void BeginBuild(const Job& a_job, Part a_part, KeptArray<T>& a_rows)
		{
			a_rows.BeginBuild(a_job.oldestContentEpochs[a_part] ? *a_job.oldestContentEpochs[a_part] : a_rows.ContentEpoch());
		}
	};

	void FrameValues::Impl::Run(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues");
		// One producer at a time, in order: each builds on the previous one's rows, and signals rise monotonically.
		for (auto finished = done.load(std::memory_order_acquire); finished + 1 < a_job.seq; finished = done.load(std::memory_order_acquire))
			done.wait(finished, std::memory_order_acquire);
		const auto start = std::chrono::steady_clock::now();
		try {
			Sample(a_job);
			const auto sampledAt = std::chrono::steady_clock::now();
			sampleUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sampledAt - start).count());
			Upload(a_job);
			uploadUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - sampledAt).count());
		} catch (const std::exception& e) {
			if (failures++ == 0)
				logger::error("[DCLF] frame values {} failed: {}; the frame draws the rows its buffers held", a_job.seq, e.what());
		}
		// Always signal: every batch of the frame waits for it.
		if (!a_job.uploads || !a_job.uploads->QueueStreamingSignal(a_job.timeline, a_job.seq))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: frame values {} could not signal the frame's wait; the GPU would wait for it forever", a_job.seq));
		const auto total = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		for (auto max = maxTotalUs.load(std::memory_order_relaxed); total > max && !maxTotalUs.compare_exchange_weak(max, total, std::memory_order_relaxed);) {}
		++frames;
		done.store(a_job.seq, std::memory_order_release);
		done.notify_all();
	}

	void FrameValues::Impl::IndexPlan(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Index");
		const auto& work = *a_job.plan;
		indexedPlan = a_job.planSerial;
		keyIndex.clear();
		slotRoots.clear();
		unkeyedMovers.clear();
		unkeyedRoots.clear();
		particleMovers.clear();
		newMovers.clear();
		newRoots.clear();
		// The marks name the previous plan's indices.
		moverDue.assign(work.movers.size(), 0);
		rootDue.assign(work.roots.size(), 0);
		dueMovers.clear();
		dueRoots.clear();
		std::vector<const void*> keys;
		decltype(moverRecords) records;
		records.reserve(work.movers.size());
		for (std::uint32_t m = 0; m < work.movers.size(); ++m) {
			const auto& item = work.movers[m];
			if (!item.geometry)
				continue;
			const auto record = RecordOf(item);
			records.insert_or_assign(item.geometry.get(), record);
			if (const auto previous = moverRecords.find(item.geometry.get()); previous == moverRecords.end() || !(previous->second == record))
				newMovers.push_back(m);
			if (item.particles) {
				particleMovers.push_back(m);
				continue;
			}
			// A first-person member is the render thread's (Kick).
			if (item.firstPerson)
				continue;
			EngineReadWindow::Lease lease;
			if (!lease) {
				unkeyedMovers.push_back(m);
				continue;
			}
			KeysOf(*item.geometry, keys);
			for (const void* key : keys)
				keyIndex[key].movers.push_back(m);
		}
		moverRecords = std::move(records);
		decltype(rootRecords) byNode;
		byNode.reserve(work.roots.size());
		for (std::uint32_t r = 0; r < work.roots.size(); ++r) {
			const auto& root = work.roots[r];
			if (!root.root)
				continue;
			std::vector<ItemRecord> dependents;
			dependents.reserve(root.dependents.size());
			for (const auto& dependent : root.dependents) {
				dependents.push_back(RecordOf(dependent));
				for (const std::uint32_t slot : { dependent.slot, dependent.layerSlot })
					if (slot != kNoObjectSlot)
						slotRoots[slot].push_back(r);
			}
			if (const auto previous = rootRecords.find(root.root.get()); previous == rootRecords.end() || previous->second != dependents)
				newRoots.push_back(r);
			byNode.insert_or_assign(root.root.get(), std::move(dependents));
			EngineReadWindow::Lease lease;
			if (!lease) {
				unkeyedRoots.push_back(r);
				continue;
			}
			KeysOf(*root.root, keys);
			for (const void* key : keys)
				keyIndex[key].roots.push_back(r);
		}
		rootRecords = std::move(byNode);
	}

	void FrameValues::Impl::MarkMover(std::uint32_t a_mover)
	{
		if (!std::exchange(moverDue[a_mover], 1))
			dueMovers.push_back(a_mover);
	}

	void FrameValues::Impl::MarkRoot(std::uint32_t a_root)
	{
		if (!std::exchange(rootDue[a_root], 1))
			dueRoots.push_back(a_root);
	}

	void FrameValues::Impl::MarkDue(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Due");
		const auto& work = *a_job.plan;
		for (const auto m : dueMovers)
			moverDue[m] = 0;
		for (const auto r : dueRoots)
			rootDue[r] = 0;
		dueMovers.clear();
		dueRoots.clear();
		if (a_job.planSerial != indexedPlan)
			IndexPlan(a_job);
		const auto markKey = [&](const void* a_key) {
			const auto it = keyIndex.find(a_key);
			if (it == keyIndex.end())
				return;
			for (const auto m : it->second.movers)
				MarkMover(m);
			for (const auto r : it->second.roots)
				MarkRoot(r);
		};
		// This frame's keys: the move writers' (a reference's animation pushes several in a row), and for each node Havok wrote the
		// reference above it, named under a lease, and the node itself (an item without a reference answers to the nodes above it).
		drainedKeys.clear();
		const void* last = nullptr;
		moveKeys += FrameValueMoves::keys.Drain([&](const void* a_key) {
			if (a_key != std::exchange(last, a_key))
				drainedKeys.push_back(a_key);
		});
		moveNodes += FrameValueMoves::nodes.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { owedNodes.push_back(std::move(a_node)); });
		// Once a lease is refused (the window closed), the rest wait for the next producer.
		bool closed = false;
		std::size_t kept = 0;
		for (std::size_t i = 0; i < owedNodes.size(); ++i) {
			if (!closed) {
				EngineReadWindow::Lease lease;
				if (lease) {
					const RE::NiAVObject* node = owedNodes[i].get();
					drainedKeys.push_back(node);
					for (const RE::NiAVObject* at = node; at; at = at->parent)
						if (const void* owner = at->GetUserData()) {
							drainedKeys.push_back(owner);
							break;
						}
					ReleaseLater(owedNodes[i]);
					continue;
				}
				closed = true;
			}
			if (kept != i)
				owedNodes[kept] = std::move(owedNodes[i]);
			++kept;
		}
		owedNodes.resize(kept);
		for (const void* key : drainedKeys)
			markKey(key);
		for (const void* key : lastKeys)
			markKey(key);
		for (const void* key : owedKeys)
			markKey(key);
		owedKeys.clear();
		std::swap(lastKeys, drainedKeys);
		// New to the plan (or changed by it): once.
		for (const auto m : newMovers)
			MarkMover(m);
		for (const auto r : newRoots)
			MarkRoot(r);
		newMovers.clear();
		newRoots.clear();
		// Keys a refused lease left unread: due until read (then by their keys).
		std::vector<const void*> keys;
		std::erase_if(unkeyedMovers, [&](std::uint32_t a_mover) {
			MarkMover(a_mover);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*work.movers[a_mover].geometry, keys);
			for (const void* key : keys)
				keyIndex[key].movers.push_back(a_mover);
			return true;
		});
		std::erase_if(unkeyedRoots, [&](std::uint32_t a_root) {
			MarkRoot(a_root);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*work.roots[a_root].root, keys);
			for (const void* key : keys)
				keyIndex[key].roots.push_back(a_root);
			return true;
		});
		// Particle quads face the camera: every frame.
		for (const auto m : particleMovers)
			MarkMover(m);
		// Without the move writers' hooks (not installed, or their install failed) nothing says what moved: everything is due.
		if (!Scene::MoveEventsLive()) {
			for (std::uint32_t m = 0; m < work.movers.size(); ++m)
				MarkMover(m);
			for (std::uint32_t r = 0; r < work.roots.size(); ++r)
				MarkRoot(r);
		}
	}

	void FrameValues::Impl::Sample(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Sample");
		const auto& work = *a_job.plan;
		BeginBuild(a_job, kPlacements, rows);
		BeginBuild(a_job, kPalettes, palettes);
		auto& out = rows.Mutable();
		if (out.size() < work.slots) {
			const std::size_t first = out.size();
			out.resize(work.slots, BindlessPlacement{});
			rows.MarkRange(first, work.slots - first);
		}
		// Two row sets per block: current and previous (PaletteRowsOf).
		auto& paletteOut = palettes.Mutable();
		if (const std::size_t paletteRows = 2ull * work.boneCapacity; paletteOut.size() < paletteRows) {
			const std::size_t first = paletteOut.size();
			paletteOut.resize(paletteRows, PaletteRow{});
			palettes.MarkRange(first, paletteRows - first);
		}
		// The movers and roots that moved (MarkDue).
		MarkDue(a_job);
		// Each slot and palette block once, by its newest item.
		struct Listed
		{
			const Plan::Item* item;
			bool palette;
			bool settles = false;  // a written slot (not a mover): kept while its transforms differ
			bool mover = false;    // a plan's mover: due again next frame when its sample changed a row or palette
		};
		std::vector<Listed> items;
		items.reserve(dueMovers.size() + (a_job.written.empty() ? 0 : a_job.written.back()->written.size()) + settling.size());
		// The marks grow with the rows; only those set are cleared (at the end).
		const std::size_t blocks = paletteOut.size() / 2;
		if (listed.size() < out.size())
			listed.resize(out.size(), 0);
		if (blockListed.size() < blocks)
			blockListed.resize(blocks, 0);
		// First-person items are the render thread's (FirstPersonSample), except particle members (expanded from snapshots).
		auto list = [&](const Plan::Item& a_item, bool a_settles, bool a_mover) {
			if ((a_item.firstPerson && !a_item.particles) || a_item.slot >= out.size() || !a_item.geometry || listed[a_item.slot])
				return;
			listed[a_item.slot] = 1;
			listedSlots.push_back(a_item.slot);
			const bool palette = a_item.boneRows && std::size_t(a_item.boneOffset) + a_item.boneRows <= blocks && !blockListed[a_item.boneOffset];
			if (palette) {
				blockListed[a_item.boneOffset] = 1;
				listedBlocks.push_back(a_item.boneOffset);
			}
			items.push_back({ &a_item, palette, a_settles, a_mover });
		};
		for (const auto m : dueMovers)
			list(work.movers[m], false, true);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				list(item, true, false);
		// Carried settling items last (a slot listed above is newer). They may hold the last engine references, so they go to the
		// render thread when done (EngineReleases, at Present), never dropped here.
		auto carried = std::move(settling);
		settling.clear();
		struct HandOver
		{
			std::vector<Plan::Item>& items;
			~HandOver()
			{
				for (auto& item : items) {
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.geometry)));
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.sunEntryNode)));
				}
			}
		} handOver{ carried };
		for (const auto& item : carried)
			list(item, true, false);
		// A root writes its dependents' sun entry and fade node after their rows: again whenever one of them was sampled.
		for (const auto slot : listedSlots)
			if (const auto it = slotRoots.find(slot); it != slotRoots.end())
				for (const auto r : it->second)
					MarkRoot(r);
		std::uint64_t firstPersonMarked = 0, firstPersonPaletteMarked = 0;
		for (const auto& sample : a_job.firstPerson) {
			if (sample.slot >= out.size())
				continue;
			if (std::memcmp(&out[sample.slot], &sample.row, sizeof(sample.row)) != 0) {
				out[sample.slot] = sample.row;
				rows.Mark(sample.slot);
				++firstPersonMarked;
			}
			if (sample.layerSlot != kNoObjectSlot && sample.layerSlot < out.size() && std::memcmp(&out[sample.layerSlot], &sample.layer, sizeof(sample.layer)) != 0) {
				out[sample.layerSlot] = sample.layer;
				rows.Mark(sample.layerSlot);
				++firstPersonMarked;
			}
			if (!sample.boneRows || sample.current.size() != sample.boneRows || sample.previous.size() != sample.boneRows)
				continue;
			const auto at = PaletteRowsOf(sample.boneOffset, sample.boneRows);
			if (std::size_t(at.previous) + sample.boneRows > paletteOut.size() || std::size_t(at.current) + sample.boneRows > paletteOut.size())
				continue;
			const std::size_t paletteBytes = std::size_t(sample.boneRows) * sizeof(PaletteRow);
			if (std::memcmp(&paletteOut[at.current], sample.current.data(), paletteBytes) != 0 || std::memcmp(&paletteOut[at.previous], sample.previous.data(), paletteBytes) != 0) {
				std::memcpy(&paletteOut[at.current], sample.current.data(), paletteBytes);
				std::memcpy(&paletteOut[at.previous], sample.previous.data(), paletteBytes);
				palettes.MarkRange(at.current, 2 * sample.boneRows);
				firstPersonPaletteMarked += 2 * sample.boneRows;
			}
		}
		auto& executor = SceneScheduler::Executor();
		constexpr std::size_t kGrain = 64;
		// Per chunk, marked afterwards (the journals are single-threaded).
		struct Changed
		{
			std::vector<std::uint32_t> slots;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row, rows)
			std::vector<const Plan::Item*> settling;
			std::vector<const void*> owed;  // movers due again next frame (owedKeys)
			std::uint64_t skins = 0, defects = 0;
		};
		std::vector<Changed> changed((items.size() + kGrain - 1) / kGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		// Particle records, before the parallel loop (ParticleSnapshots::Follow is producer-only).
		std::vector<ParticleSnapshots::Record*> particleRecords(items.size(), nullptr);
		for (std::size_t i = 0; i < items.size(); ++i)
			if (items[i].palette && items[i].item->particles)
				particleRecords[i] = ParticleSnapshots::Get().Follow(items[i].item->geometry.get());
		executor.ParallelFor("CS.DCLF.FrameValues.Items", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Items");
			auto& mine = changed[a_begin / kGrain];
			BindlessPlacement row, layer;
			std::vector<PaletteRow> particleRows;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& [itemPointer, palette, settles, mover] = items[i];
				const auto& item = *itemPointer;
				// A first-person particle member reads no engine memory here: no lease, never settling.
				const bool firstPerson = item.firstPerson;
				std::optional<EngineReadWindow::Lease> lease;
				if (!firstPerson)
					lease.emplace();
				if (lease && !*lease) {
					++refusedHere;
					if (settles)
						mine.settling.push_back(&item);  // not sampled: the next producer takes it
					if (mover)
						mine.owed.push_back(item.geometry.get());
					continue;
				}
				bool changes = false;
				if (firstPerson)
					row = out[item.slot];
				else
					SampleItem(item, row, layer);
				// Particle quads, transforms and bound from the newest snapshot (particle data is never read here).
				auto* particles = particleRecords[i];
				if (particles) {
					particleRows.resize(item.boneRows);
					ParticleSnapshots::Expanded expanded;
					ParticleSnapshots::Get().Expand(*particles, firstPerson ? a_job.firstPersonParticleCamera : a_job.particleCamera,
						item.boneRows / ParticleSnapshots::kRowsPerParticle, particleRows.data()->v, expanded);
					if (expanded.snapshot) {
						std::memcpy(row.world, expanded.world, sizeof(row.world));
						std::memcpy(row.previousWorld, expanded.world, sizeof(row.previousWorld));
						std::memcpy(row.bound, expanded.bound, sizeof(row.bound));
						if (firstPerson) {
							const auto& eye = a_job.firstPersonParticleCamera.eye;
							const float dx = expanded.bound[0] - eye[0], dy = expanded.bound[1] - eye[1], dz = expanded.bound[2] - eye[2];
							const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
							firstPersonParticleExpansions.fetch_add(1, std::memory_order_relaxed);
							firstPersonParticleCount.fetch_add(expanded.particles, std::memory_order_relaxed);
							const std::uint32_t bits = std::bit_cast<std::uint32_t>(distance);
							for (std::uint32_t seen = firstPersonParticleFarthest.load(std::memory_order_relaxed);
								bits > seen && !firstPersonParticleFarthest.compare_exchange_weak(seen, bits, std::memory_order_relaxed);) {}
						}
					}
				}
				if (settles && !firstPerson && std::memcmp(&item.geometry->world, &DrawnPreviousWorld(*item.geometry), sizeof(RE::NiTransform)) != 0)
					mine.settling.push_back(&item);
				if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.slots.push_back(item.slot);
					changes = true;
				}
				if (!firstPerson && item.layerSlot != kNoObjectSlot && item.layerSlot < out.size() && std::memcmp(&out[item.layerSlot], &layer, sizeof(layer)) != 0) {
					out[item.layerSlot] = layer;
					mine.slots.push_back(item.layerSlot);
					changes = true;
				}
				if (palette) {
					if (particles) {
						const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
						const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
						if (std::memcmp(&paletteOut[at.current], particleRows.data(), bytes) != 0) {
							std::memcpy(&paletteOut[at.current], particleRows.data(), bytes);
							mine.paletteRuns.emplace_back(at.current, item.boneRows);
							changes = true;
						}
					} else {
						// Run the engine's palette update, then copy rows that differ (the frame after its motion stops, the update copies the
						// current palette to the previous: the change that keeps the skin due once more, so its motion vectors settle).
						auto& geometry = *item.geometry;
						if (auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get())
							UpdateSkin(skin, geometry.world);
						const float* current = nullptr;
						const float* previous = nullptr;
						if (!SkinPalettes(geometry, item.boneRows, current, previous)) {
							++mine.defects;
						} else {
							++mine.skins;
							const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
							const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
							if (std::memcmp(&paletteOut[at.current], current, bytes) != 0 || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
								std::memcpy(&paletteOut[at.current], current, bytes);
								std::memcpy(&paletteOut[at.previous], previous, bytes);
								mine.paletteRuns.emplace_back(at.current, 2 * item.boneRows);
								changes = true;
							}
						}
					}
				}
				// A mover whose sample changed something is sampled again next frame, until a sample changes nothing: its previous world
				// and previous palette settle on the current ones the frame after its motion stops.
				if (mover && changes)
					mine.owed.push_back(item.geometry.get());
			}
		});
		ParticleSnapshots::Get().EndRun();
		// Roots: their dependents' sun entries and fade nodes, after the items wrote theirs.
		std::vector<std::vector<std::uint32_t>> rootChanged(dueRoots.size());
		std::vector<std::uint8_t> rootRefused(dueRoots.size(), 0);
		executor.ParallelFor("CS.DCLF.FrameValues.Roots", dueRoots.size(), 16, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Roots");
			for (std::size_t r = a_begin; r < a_end; ++r) {
				const auto& root = work.roots[dueRoots[r]];
				EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedHere;
					rootRefused[r] = 1;
					continue;
				}
				float entry[4];
				StoreSunEntry(root.root.get(), entry);
				for (const auto& dependent : root.dependents) {
					if (dependent.firstPerson || dependent.slot >= out.size() || !dependent.geometry)
						continue;
					const auto write = [&](std::uint32_t a_slot, const RE::BSShaderProperty* a_property) {
						auto& row = out[a_slot];
						float fade[4];
						StoreFadeNode(a_property, fade);
						if (std::memcmp(row.sunEntry, entry, sizeof(entry)) == 0 && std::memcmp(row.lodFadeNode, fade, sizeof(fade)) == 0)
							return;
						std::memcpy(row.sunEntry, entry, sizeof(entry));
						std::memcpy(row.lodFadeNode, fade, sizeof(fade));
						rootChanged[r].push_back(a_slot);
					};
					write(dependent.slot, dependent.geometry->GetGeometryRuntimeData().shaderProperty.get());
					if (dependent.layerSlot != kNoObjectSlot && dependent.layerSlot < out.size())
						write(dependent.layerSlot, LayerPropertyOf(*dependent.geometry));
				}
			}
		});
		std::uint64_t marked = 0, paletteMarked = 0;
		for (const auto& chunk : changed) {
			for (const auto* item : chunk.settling)
				settling.push_back(*item);
			owedKeys.insert(owedKeys.end(), chunk.owed.begin(), chunk.owed.end());
		}
		settled += settling.size();
		for (const auto& chunk : changed) {
			for (const auto slot : chunk.slots)
				rows.Mark(slot);
			marked += chunk.slots.size();
			for (const auto& [first, count] : chunk.paletteRuns) {
				palettes.MarkRange(first, count);
				paletteMarked += count;
			}
			skins += chunk.skins;
			defects += chunk.defects;
		}
		for (std::size_t r = 0; r < dueRoots.size(); ++r) {
			for (const auto slot : rootChanged[r])
				rows.Mark(slot);
			marked += rootChanged[r].size();
			// Likewise a root whose sample changed a row (or was refused) is due next frame.
			if (rootRefused[r] || !rootChanged[r].empty())
				owedKeys.push_back(work.roots[dueRoots[r]].root.get());
		}
		sampled += static_cast<std::uint64_t>(items.size() + a_job.firstPerson.size());
		rowsChanged += marked + firstPersonMarked;
		paletteRowsChanged += paletteMarked + firstPersonPaletteMarked;
		movers += work.movers.size();
		moversDue += dueMovers.size();
		roots += work.roots.size();
		rootsDue += dueRoots.size();
		if (const auto refusals = refusedHere.load())
			refused += refusals;
		// Parity (CS_DCLF_PERSISTENT_PARITY): every mover and root sampled again, against what the moved-only sampling left.
		if (a_job.parity)
			CheckEveryMover(a_job);
		// Only the marks set this frame go back to zero.
		for (const auto slot : listedSlots)
			listed[slot] = 0;
		for (const auto block : listedBlocks)
			blockListed[block] = 0;
		listedSlots.clear();
		listedBlocks.clear();
		SampleShadingRows(a_job, work.slots);
		SampleFadeSeeds(a_job);
	}

	void FrameValues::Impl::CheckEveryMover(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.MovedParity");
		// The persistent parity's check of the moved-only sampling (dclf B): every mover's and root's rows as sampling them all now gives
		// them (nothing written), against the rows the producers left. A difference is a move no event named (a frozen row). Particle
		// systems (sampled every frame) and first-person members (the render thread's) are not checked. A skin's palette update is
		// idempotent per frame (a due skin's runs again for nothing); a skin not due runs it here as the native bone setter would.
		const auto& work = *a_job.plan;
		const auto& out = rows.Get();
		const auto& paletteOut = palettes.Get();
		struct Fresh
		{
			BindlessPlacement row{}, layer{};
			bool sampled = false;
		};
		std::vector<Fresh> fresh(work.movers.size());
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> slotMover;  // a checked mover's slot and its layer's: its index
		for (std::uint32_t m = 0; m < work.movers.size(); ++m) {
			const auto& item = work.movers[m];
			if (item.firstPerson || item.particles || !item.geometry || item.slot >= out.size())
				continue;
			slotMover.insert_or_assign(item.slot, m);
			if (item.layerSlot != kNoObjectSlot && item.layerSlot < out.size())
				slotMover.insert_or_assign(item.layerSlot, m);
		}
		std::atomic<std::uint64_t> palettesChecked{ 0 }, palettesDiffer{ 0 };
		std::string firstPalette;
		std::atomic_flag firstPaletteTaken;
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.MovedParity", work.movers.size(), 64, [&](std::size_t a_begin, std::size_t a_end) {
			for (std::size_t m = a_begin; m < a_end; ++m) {
				const auto& item = work.movers[m];
				if (item.firstPerson || item.particles || !item.geometry || item.slot >= out.size())
					continue;
				EngineReadWindow::Lease lease;
				if (!lease)
					continue;
				SampleItem(item, fresh[m].row, fresh[m].layer);
				fresh[m].sampled = true;
				if (!item.boneRows || std::size_t(item.boneOffset) + item.boneRows > paletteOut.size() / 2)
					continue;
				auto& geometry = *item.geometry;
				if (auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get())
					UpdateSkin(skin, geometry.world);
				const float* current = nullptr;
				const float* previous = nullptr;
				if (!SkinPalettes(geometry, item.boneRows, current, previous))
					continue;
				++palettesChecked;
				const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
				const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
				const bool currentDiffers = std::memcmp(&paletteOut[at.current], current, bytes) != 0;
				if (currentDiffers || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
					++palettesDiffer;
					if (!firstPaletteTaken.test_and_set())
						firstPalette = fmt::format("palette of slot {} ('{}', {}): {}", item.slot, geometry.name.c_str(), moverDue[m] ? "due" : "not due",
							currentDiffers ? "current" : "previous");
				}
			}
		});
		// The roots onto the fresh rows of their dependents, as the producer orders them (after the items); a dependent that is no
		// checked mover is compared on the two values the root writes.
		auto& p = movedParity;
		std::string firstRoot;
		for (std::uint32_t r = 0; r < work.roots.size(); ++r) {
			const auto& root = work.roots[r];
			EngineReadWindow::Lease lease;
			if (!lease || !root.root)
				continue;
			float entry[4];
			StoreSunEntry(root.root.get(), entry);
			for (const auto& dependent : root.dependents) {
				if (dependent.firstPerson || dependent.slot >= out.size() || !dependent.geometry)
					continue;
				const auto apply = [&](std::uint32_t a_slot, const RE::BSShaderProperty* a_property) {
					float fade[4];
					StoreFadeNode(a_property, fade);
					if (const auto it = slotMover.find(a_slot); it != slotMover.end() && fresh[it->second].sampled) {
						auto& target = a_slot == work.movers[it->second].slot ? fresh[it->second].row : fresh[it->second].layer;
						std::memcpy(target.sunEntry, entry, sizeof(entry));
						std::memcpy(target.lodFadeNode, fade, sizeof(fade));
						return;
					}
					++p.rootRows;
					if (std::memcmp(out[a_slot].sunEntry, entry, sizeof(entry)) != 0 || std::memcmp(out[a_slot].lodFadeNode, fade, sizeof(fade)) != 0) {
						++p.rootRowsDiffer;
						if (firstRoot.empty())
							firstRoot = fmt::format("root dependent slot {} ('{}', root {}): {}", a_slot, dependent.geometry->name.c_str(), rootDue[r] ? "due" : "not due",
								std::memcmp(out[a_slot].sunEntry, entry, sizeof(entry)) != 0 ? "sunEntry" : "lodFadeNode");
					}
				};
				apply(dependent.slot, dependent.geometry->GetGeometryRuntimeData().shaderProperty.get());
				if (dependent.layerSlot != kNoObjectSlot && dependent.layerSlot < out.size())
					apply(dependent.layerSlot, LayerPropertyOf(*dependent.geometry));
			}
		}
		std::string firstRow;
		for (std::uint32_t m = 0; m < work.movers.size(); ++m) {
			if (!fresh[m].sampled)
				continue;
			const auto& item = work.movers[m];
			const auto compare = [&](std::uint32_t a_slot, const BindlessPlacement& a_fresh) {
				++p.rows;
				if (std::memcmp(&out[a_slot], &a_fresh, sizeof(a_fresh)) == 0)
					return;
				++p.rowsDiffer;
				if (firstRow.empty()) {
					EngineReadWindow::Lease lease;
					firstRow = fmt::format("row of slot {} ('{}', {}): {}", a_slot, lease ? item.geometry->name.c_str() : "", moverDue[m] ? "due" : "not due",
						PlacementDifferences(a_fresh, out[a_slot]));
				}
			};
			compare(item.slot, fresh[m].row);
			if (item.layerSlot != kNoObjectSlot && item.layerSlot < out.size())
				compare(item.layerSlot, fresh[m].layer);
		}
		p.palettes += palettesChecked.load();
		p.palettesDiffer += palettesDiffer.load();
		++p.checks;
		p.movers += work.movers.size();
		p.moversDue += dueMovers.size();
		p.roots += work.roots.size();
		p.rootsDue += dueRoots.size();
		if (p.first.empty())
			p.first = !firstRow.empty() ? firstRow : !firstPalette.empty() ? firstPalette : firstRoot;
		const bool differs = p.rowsDiffer || p.palettesDiffer || p.rootRowsDiffer;
		if (p.checks < 5 && !differs)
			return;
		const double n = static_cast<double>(p.checks);
		logger::info("[DCLF] frame values' moved-only sampling (parity): {} checks; a check {:.0f} movers ({:.1f} due), {:.0f} roots ({:.1f} due); {} rows checked, {} differ; "
					 "{} palettes checked, {} differ; {} root rows checked, {} differ{}{}",
			p.checks, p.movers / n, p.moversDue / n, p.roots / n, p.rootsDue / n, p.rows, p.rowsDiffer, p.palettes, p.palettesDiffer, p.rootRows, p.rootRowsDiffer,
			differs ? " <- DIFFER; first: " : " <- OK", differs ? p.first : std::string());
		p = {};
	}

	void FrameValues::Impl::SampleFadeSeeds(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.FadeSeeds");
		BeginBuild(a_job, kFadeSeeds, seeds);
		auto& out = seeds.Mutable();
		if (out.size() < a_job.seedRows) {
			const std::size_t first = out.size();
			out.resize(a_job.seedRows, FadeRootStatic{});
			seeds.MarkRange(first, a_job.seedRows - first);
		}
		// In request order, those a refused lease left first: a later value for the same row wins. StaticOf reads the copy the frame's
		// start took (and the tree LOD switch it names); once a lease is refused (the window closed) the rest wait, in order, for the next
		// producer.
		auto carried = std::move(owedSeeds);
		owedSeeds.clear();
		bool refusedHere = false;
		const auto seed = [&](const SeedCopy& a_copy) {
			if (!refusedHere) {
				EngineReadWindow::Lease lease;
				if (lease) {
					FrameGlobals::Scope scope(a_copy.globals);
					FadeRootStatic row = FadeState::StaticOf(*reinterpret_cast<const RE::NiAVObject*>(a_copy.bytes.data()));
					row.generation = a_copy.generation;
					if (a_copy.row < out.size()) {
						out[a_copy.row] = row;
						seeds.Mark(a_copy.row);
						++seeded;
					}
					return;
				}
				refusedHere = true;
				++refused;
			}
			owedSeeds.push_back(a_copy);
		};
		for (const auto& copy : carried)
			seed(copy);
		for (const auto& copy : a_job.seedValues)
			seed(copy);
		// The carried copies' references to the render thread (the job's go with the job).
		for (auto& copy : carried)
			ReleaseLater(copy.node);
		BeginBuild(a_job, kTreeSeeds, treeSeeds);
		auto& trees = treeSeeds.Mutable();
		if (trees.size() < a_job.treeSeedRows) {
			const std::size_t first = trees.size();
			trees.resize(a_job.treeSeedRows, TreeStatic{});
			treeSeeds.MarkRange(first, a_job.treeSeedRows - first);
		}
		for (const auto& [at, row] : a_job.treeSeedValues) {
			if (at >= trees.size())
				continue;
			trees[at] = row;
			treeSeeds.Mark(at);
			++seeded;
		}
	}

	void FrameValues::Impl::SampleShadingRows(const Job& a_job, std::size_t a_slots)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Shading");
		BeginBuild(a_job, kShading, shading);
		auto& out = shading.Mutable();
		if (out.size() < a_slots) {
			const std::size_t first = out.size();
			out.resize(a_slots, BindlessShading{});
			shading.MarkRange(first, a_slots - first);
		}
		// Each slot once, by its newest item (the slot may have been reused since).
		std::vector<const ShadingItem*> items;
		items.reserve(a_job.shadingItems.size());
		shadingListed.assign(out.size(), 0);
		for (auto it = a_job.shadingItems.rbegin(); it != a_job.shadingItems.rend(); ++it)
			if (it->slot < out.size() && it->property && !std::exchange(shadingListed[it->slot], 1))
				items.push_back(&*it);
		constexpr std::size_t kGrain = 128;
		std::vector<std::vector<std::uint32_t>> changed((items.size() + kGrain - 1) / kGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.Shading", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Shading");
			FrameGlobals::Scope scope(a_job.globals);  // the tree wind's (SampleShading: DeriveTreeAnim)
			auto& mine = changed[a_begin / kGrain];
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& item = *items[i];
				EngineReadWindow::Lease lease;
				if (!lease) {
					++refusedHere;
					continue;
				}
				BindlessShading row = out[item.slot];
				SampleShading(*item.property.get(), item.pass, item.member, row);
				// Only actors have wetness (from the capture, below).
				if (!item.actor)
					std::fill(std::begin(row.skinPerGeometry), std::end(row.skinPerGeometry), 0.0f);
				if (std::memcmp(&row, &out[item.slot], sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.push_back(item.slot);
				}
			}
		});
		std::uint64_t marked = 0, wetMarked = 0;
		for (const auto& slots : changed) {
			for (const auto slot : slots)
				shading.Mark(slot);
			marked += slots.size();
		}
		for (const auto& [slot, value] : a_job.wetness) {
			if (slot >= out.size() || std::memcmp(out[slot].skinPerGeometry, value.data(), sizeof(value)) == 0)
				continue;
			std::memcpy(out[slot].skinPerGeometry, value.data(), sizeof(value));
			shading.Mark(slot);
			++wetMarked;
		}
		shadingSampled += items.size();
		shadingChanged += marked;
		wetnessChanged += wetMarked;
		if (const auto refusals = refusedHere.load())
			refused += refusals;
	}

	// The catch-up: the journal's runs since the backing's content (all of them for a backing that holds nothing, or less than the
	// journal's floor), into the leased backing (PooledBackingLease: no frame reads it before the one that leased it, which waits for
	// this producer's signal); then the backing is at the journal's content epoch.
	template <class T>
	void FrameValues::Impl::Send(const Job& a_job, Part a_part, const KeptArray<T>& a_rows)
	{
		auto& backing = *a_job.leases[a_part];  // the lease holder fills it (contentEpoch is its)
		auto view = a_rows.View();
		if (view.Count() > backing.capacityClass)
			throw std::runtime_error(fmt::format("its {} backing holds {} rows of {}", kPartNames[a_part], backing.capacityClass, view.Count()));
		const auto* data = view.elements ? view.elements->data() : nullptr;
		std::uint64_t sent = 0, sentBytes = 0;
		view.changes.ForEachRun(backing.contentEpoch.load(std::memory_order_acquire), view.Count(), [&](std::uint64_t a_first, std::uint64_t a_count) {
			const org::StreamingUploadSegment segment{ data + a_first, static_cast<std::size_t>(a_count * sizeof(T)) };
			auto ticket = a_job.uploads->QueueTrackedStreamingUploadSegments({ &segment, 1 }, segment.size,
				org::WorkerOwnedDestination{ backing.buffer, org::WorkerOwnedDestination::Ownership::PooledBackingLease },
				static_cast<std::size_t>(a_first * sizeof(T)));
			if (!ticket || ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled)
				throw std::runtime_error("the dedicated uploader refused its rows");
			++sent;
			sentBytes += segment.size;
		});
		backing.contentEpoch.store(view.ContentEpoch(), std::memory_order_release);
		auto& pool = pools[a_part];
		pool.newestContentEpoch.store(view.ContentEpoch(), std::memory_order_release);
		pool.runs += sent;
		pool.bytes += sentBytes;
		runs += sent;
		bytes += sentBytes;
	}

	void FrameValues::Impl::Upload(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Upload");
		Send(a_job, kPlacements, rows);
		Send(a_job, kPalettes, palettes);
		Send(a_job, kShading, shading);
		Send(a_job, kFadeSeeds, seeds);
		Send(a_job, kTreeSeeds, treeSeeds);
	}

	FrameValues::FrameValues() :
		impl(std::make_unique<Impl>())
	{}

	FrameValues::~FrameValues() = default;

	FrameValues& FrameValues::Get()
	{
		static FrameValues values;
		return values;
	}

	bool FrameValues::Available() const
	{
		auto& runtime = RenderGraphRuntime::Get();
		return runtime.Host() && runtime.FrameWaitTimeline();
	}

	bool FrameValues::Kick(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
		std::vector<WetnessValue> a_wetness, std::vector<FadeSeedItem> a_seeds, std::vector<TreeSeedItem> a_treeSeeds,
		std::shared_ptr<const FrameGlobals> a_globals)
	{
		auto& s = *impl;
		s.kicked = false;
		// A frame whose EndFrame did not come: its backings are bound to what was submitted so far.
		BindFrameLeases(s.frameLeases);
		// Fade seeds: the node as the frame's start has it, before this frame's culls update it (FadeStateCS's first update is this
		// frame's): its bytes copied here, the row made from the copy by the producer (StaticOf, with the frame's globals: its LOD scale).
		for (auto& seed : a_seeds) {
			if (!seed.node)
				continue;
			auto& copy = s.unsentSeeds.emplace_back();
			std::memcpy(copy.bytes.data(), seed.node.get(), FadeState::NodeBytes(FadeState::PlanOf(*seed.node)));
			copy.node = std::move(seed.node);
			copy.globals = a_globals;
			copy.row = seed.row;
			copy.generation = seed.generation;
			s.seedRows = std::max(s.seedRows, seed.row + 1);
		}
		{
			FrameGlobals::Scope scope(a_globals);
			// Tree seeds likewise, but read here: TreeStaticOfNode's near-list test reads the tree's multibound shape's cull result (FUN_14147d640
			// is a pure read, the first child's AsMultiBoundNode; then the shape's +0x10), which this frame's culls write.
			for (const auto& seed : a_treeSeeds) {
				if (!seed.node)
					continue;
				TreeStatic row;
				TreeStaticOfNode(seed.node.get(), row);
				row.generation = seed.generation;
				row.seedOdd = seed.row & 1u;
				s.unsentTreeSeeds.emplace_back(seed.row, row);
				s.treeSeedRows = std::max(s.treeSeedRows, seed.row + 1);
			}
		}
		// Appended after any not yet taken (newest last).
		s.unsampledShading.insert(s.unsampledShading.end(), std::make_move_iterator(a_shading.begin()), std::make_move_iterator(a_shading.end()));
		s.unsampledWetness.insert(s.unsampledWetness.end(), a_wetness.begin(), a_wetness.end());
		auto* host = RenderGraphRuntime::Get().Host();
		auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		auto uploads = host ? host->RetainUploads() : nullptr;
		if (!host || !timeline || !uploads || !uploads->HasDedicatedStreamingQueue()) {
			// Plans wait for the first producer; startup frames precede the upload queue (made by the graph's first build).
			for (auto& plan : a_plans)
				if (plan) {
					s.unsampled.push_back(plan);
					s.plan = std::move(plan);
					++s.planSerial;
				}
			if (host)
				host->SetFrameWaitValue(0);
			// No backing is leased for the frame: its draws have no rows (no index names a backing the pool may release).
			frameIndex = paletteIndex = shadingIndex = seedsIndex = treeSeedsIndex = 0;
			if (!s.unavailableLogged && s.seq) {
				s.unavailableLogged = true;
				logger::error("[DCLF] frame values unavailable ({}): DCLF's draws have no placements", !host ? "no render graph" : "no dedicated upload queue");
			}
			return false;
		}
		// Every plan since the last frame: all written slots are sampled, the newest is drawn with.
		for (auto& plan : a_plans)
			if (plan) {
				s.unsampled.push_back(plan);
				s.plan = std::move(plan);
				++s.planSerial;
			}
		// Release finished producers' plans here on the render thread (they hold engine references).
		const auto finished = s.done.load(std::memory_order_acquire);
		while (!s.plans.empty() && s.plans.front().first <= finished)
			s.plans.pop_front();
		static const Plan kEmpty{};
		const std::uint64_t seq = ++s.seq;
		Impl::Job job;
		job.seq = seq;
		job.plan = s.plan ? s.plan.get() : &kEmpty;
		job.planSerial = s.plan ? s.planSerial : 0;
		job.parity = Policy::SampleEveryMover(seq);
		// Held until the producer is done.
		for (auto& unsampled : s.unsampled) {
			job.written.push_back(unsampled.get());
			s.plans.emplace_back(seq, std::move(unsampled));
		}
		s.unsampled.clear();
		job.shadingItems = std::exchange(s.unsampledShading, {});
		job.wetness = std::exchange(s.unsampledWetness, {});
		job.seedValues = std::exchange(s.unsentSeeds, {});
		job.seedRows = s.seedRows;
		job.treeSeedValues = std::exchange(s.unsentTreeSeeds, {});
		job.treeSeedRows = s.treeSeedRows;
		job.globals = a_globals;
		// Each part's backing, holding every row the producer leaves (a class of GrowthPolicy::Doubling(1024)), the one that holds
		// most of the journal preferred; the floor (the oldest content in circulation, this lease's included) is what the
		// producer's journal keeps.
		const std::array<std::uint64_t, kPools> needed{ s.plan ? s.plan->slots : 0u, s.plan ? 2ull * s.plan->boneCapacity : 0u,
			s.plan ? s.plan->slots : 0u, s.seedRows, s.treeSeedRows };
		for (std::size_t part = 0; part < kPools; ++part) {
			auto& pool = s.pools[part];
			pool.capacity = org::services::GrowthPolicy::Doubling(1024).Next(pool.capacity, needed[part]);
			const auto name = fmt::format("cs.dclf.frame-values.{}", Impl::kPartNames[part]);
			org::services::BackingPool::AcquireInfo info;
			auto lease = pool.pool->Acquire({ .capacityClass = pool.capacity, .stride = Impl::kStrides[part], .name = name },
				pool.newestContentEpoch.load(std::memory_order_acquire), &info);
			if (!lease)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the frame values' {} pool returned no backing", Impl::kPartNames[part]));
			++pool.acquires;
			if (info.allocated)
				++pool.made;
			job.oldestContentEpochs[part] = pool.pool->OldestContentEpoch();
			pool.floor.store(job.oldestContentEpochs[part].value_or(0), std::memory_order_relaxed);
			s.frameLeases[part] = lease;
			job.leases[part] = std::move(lease);
		}
		job.timeline = std::move(timeline);
		job.uploads = std::move(uploads);
		job.particleCamera = ParticleSnapshots::Get().FrameCamera();
		// The first-person eye (DrawWorld +0x00, filled by FUN_1406452c0 before FrameDriver::BeginFrame): where FUN_1414cb020 places
		// the first-person camera, with the world camera's rotation.
		job.firstPersonParticleCamera = job.particleCamera;
		if (const auto* eye = REL::Relocation<const float*>{ REL::Offset(Engine::Address::kDrawWorldEye) }.get(); eye && job.particleCamera.valid)
			std::memcpy(job.firstPersonParticleCamera.eye, eye, sizeof(job.firstPersonParticleCamera.eye));
		// Claimed first-person members, sampled after the engine's update and before Main::Draw's first-person shift
		// (FUN_1414cb020), palettes included: the engine draws none of them, so none of its reads see a world-space palette.
		if (s.firstPersonSerial != job.planSerial) {
			// The newest plan's first-person movers, listed once a plan (Kick visits them alone).
			s.firstPersonSerial = job.planSerial;
			s.firstPersonMovers.clear();
			for (const auto& item : job.plan->movers)
				if (item.firstPerson)
					s.firstPersonMovers.push_back(&item);
		}
		if (const auto claims = std::exchange(firstPersonClaims, nullptr)) {
			std::vector<std::uint8_t> taken;
			auto take = [&](const Plan::Item& a_item) {
				if (!a_item.firstPerson || !a_item.geometry || a_item.slot == kNoObjectSlot || !(claims->PhasesOf(a_item.geometry.get()) & kSetFirstPerson))
					return;
				if (taken.size() <= a_item.slot)
					taken.resize(a_item.slot + 1, 0);
				if (std::exchange(taken[a_item.slot], 1))
					return;
				auto& sample = job.firstPerson.emplace_back();
				sample.slot = a_item.slot;
				sample.layerSlot = a_item.layerSlot;
				SampleItem(a_item, sample.row, sample.layer);
				if (!a_item.boneRows)
					return;
				auto& geometry = *a_item.geometry;
				if (auto* skin = geometry.GetGeometryRuntimeData().skinInstance.get())
					UpdateSkin(skin, geometry.world);
				const float* current = nullptr;
				const float* previous = nullptr;
				if (!SkinPalettes(geometry, a_item.boneRows, current, previous))
					return;
				sample.boneOffset = a_item.boneOffset;
				sample.boneRows = a_item.boneRows;
				const auto* currentRows = reinterpret_cast<const PaletteRow*>(current);
				const auto* previousRows = reinterpret_cast<const PaletteRow*>(previous);
				sample.current.assign(currentRows, currentRows + a_item.boneRows);
				sample.previous.assign(previousRows, previousRows + a_item.boneRows);
			};
			for (const auto* item : s.firstPersonMovers)
				take(*item);
			for (auto it = job.written.rbegin(); it != job.written.rend(); ++it)
				for (const auto& item : (*it)->written)
					take(item);
		}
		if (s.plan)
			s.plans.emplace_back(seq, s.plan);
		frameIndex = s.frameLeases[Impl::kPlacements]->srvIndex;
		paletteIndex = s.frameLeases[Impl::kPalettes]->srvIndex;
		shadingIndex = s.frameLeases[Impl::kShading]->srvIndex;
		seedsIndex = s.frameLeases[Impl::kFadeSeeds]->srvIndex;
		treeSeedsIndex = s.frameLeases[Impl::kTreeSeeds]->srvIndex;
		// Batches submitted from here wait for the frame's values; the producer always signals.
		host->SetFrameWaitValue(seq);
		s.kicked = true;
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
				"frame values", [&s, job = std::move(job)](const auto&) mutable {
					s.Run(job);
					// The producer's leases go; the frame's (frameLeases) keep the backings until EndFrame binds them.
					job.leases = {};
					// Engine references go to the render thread (EngineReleases, at Present), never dropped on this worker.
					for (auto& item : job.shadingItems)
						EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.property)));
					for (auto& seed : job.seedValues)
						ReleaseLater(seed.node);
				}))
			stl::report_and_fail("Drawcall Limit Fix: the frame values' producer was refused by DCLF's executor");
		return true;
	}

	void FrameValues::Skip()
	{
		impl->kicked = false;
		BindFrameLeases(impl->frameLeases);
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->SetFrameWaitValue(0);
	}

	void FrameValues::EndFrame()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		s.kicked = false;
		BindFrameLeases(s.frameLeases);
	}

	const std::vector<TreeStatic>* FrameValues::TreeSeedsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!FadeSeedsIfDone(a_wait))
			return nullptr;
		return &s.treeSeeds.Get();
	}

	const std::vector<FadeRootStatic>* FrameValues::FadeSeedsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.seeds.Get();
	}

	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.rows.Get();
	}

	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.palettes.Get();
	}

	const std::vector<BindlessShading>* FrameValues::ShadingIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return &s.shading.Get();
	}

	FrameValues::Counters FrameValues::TakeCounters()
	{
		auto& s = *impl;
		Counters c;
		c.frames = s.frames.exchange(0);
		if (!c.frames)
			return c;
		c.sampled = s.sampled.exchange(0);
		c.settled = s.settled.exchange(0);
		c.seeded = s.seeded.exchange(0);
		c.rowsChanged = s.rowsChanged.exchange(0);
		c.skins = s.skins.exchange(0);
		c.paletteRowsChanged = s.paletteRowsChanged.exchange(0);
		c.shadingSampled = s.shadingSampled.exchange(0);
		c.shadingChanged = s.shadingChanged.exchange(0);
		c.wetnessChanged = s.wetnessChanged.exchange(0);
		c.runs = s.runs.exchange(0);
		c.bytes = s.bytes.exchange(0);
		c.sampleUs = s.sampleUs.exchange(0);
		c.uploadUs = s.uploadUs.exchange(0);
		c.maxTotalUs = s.maxTotalUs.exchange(0);
		c.refused = s.refused.exchange(0);
		c.defects = s.defects.exchange(0);
		c.failures = s.failures.exchange(0);
		c.movers = s.movers.exchange(0);
		c.moversDue = s.moversDue.exchange(0);
		c.roots = s.roots.exchange(0);
		c.rootsDue = s.rootsDue.exchange(0);
		c.moveKeys = s.moveKeys.exchange(0);
		c.moveNodes = s.moveNodes.exchange(0);
		return c;
	}

	std::array<FrameValues::PoolStats, FrameValues::kPools> FrameValues::TakePoolStats()
	{
		auto& s = *impl;
		std::array<PoolStats, kPools> out;
		for (std::size_t part = 0; part < kPools; ++part) {
			auto& pool = s.pools[part];
			auto& stats = out[part];
			stats.name = Impl::kPartNames[part];
			const auto census = pool.pool->Count();
			stats.live = census.live;
			stats.leased = census.leased;
			stats.free = census.free;
			stats.liveBytes = census.bytes;
			stats.allocated = census.allocated;
			stats.reused = census.reused;
			stats.released = census.released;
			stats.acquires = pool.acquires.exchange(0, std::memory_order_relaxed);
			stats.made = pool.made.exchange(0, std::memory_order_relaxed);
			stats.runs = pool.runs.exchange(0, std::memory_order_relaxed);
			stats.bytes = pool.bytes.exchange(0, std::memory_order_relaxed);
			stats.floor = pool.floor.load(std::memory_order_relaxed);
		}
		return out;
	}
#else
	struct FrameValues::Impl
	{};
	FrameValues::FrameValues() = default;
	FrameValues::~FrameValues() = default;
	FrameValues& FrameValues::Get()
	{
		static FrameValues values;
		return values;
	}
	bool FrameValues::Available() const { return false; }
	bool FrameValues::Kick(std::vector<std::shared_ptr<const PlacementPlan>>, std::vector<ShadingItem>, std::vector<WetnessValue>,
		std::vector<FadeSeedItem>, std::vector<TreeSeedItem>, std::shared_ptr<const FrameGlobals>) { return false; }
	const std::vector<TreeStatic>* FrameValues::TreeSeedsIfDone(bool) const { return nullptr; }
	const std::vector<FadeRootStatic>* FrameValues::FadeSeedsIfDone(bool) const { return nullptr; }
	void FrameValues::Skip() {}
	void FrameValues::EndFrame() {}
	const std::vector<BindlessPlacement>* FrameValues::RowsIfDone(bool) const { return nullptr; }
	const std::vector<FrameValues::PaletteRow>* FrameValues::PalettesIfDone(bool) const { return nullptr; }
	const std::vector<BindlessShading>* FrameValues::ShadingIfDone(bool) const { return nullptr; }
	FrameValues::Counters FrameValues::TakeCounters() { return {}; }
	std::array<FrameValues::PoolStats, FrameValues::kPools> FrameValues::TakePoolStats() { return {}; }
	bool FrameValues::SkinPalettes(const RE::BSGeometry&, std::uint32_t, const float*&, const float*&) { return false; }
#endif
}
