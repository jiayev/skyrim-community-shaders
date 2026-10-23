// FrameValues (see the header): the render thread's Kick and the producers it dispatches onto the preparation pool, one at a time.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"

#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PlacementRows/BillboardRow.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <bit>
#include <cmath>
#include <optional>
#include <cstring>
#include <limits>

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValueMoves.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/Runtime/IUploadService.h>
#include <Resources/Buffers/Buffer.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <rhi.h>

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <chrono>

namespace DCLF
{
	namespace
	{
		// The palette rows classes' first-person writes (Api::PaletteRowsChunk::NoteFirstPerson), for the first-person report.
		Api::PaletteRowsFirstPerson firstPersonPalette;

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

		// A listed root's bound (the sun entry test's: the row's sunEntry), or unbounded (radius +max, inside every process).
		void StoreListedRoot(const RE::NiAVObject* a_node, float (&a_out)[4])
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

		// The ordered-node interleave (BindlessPlacement::forwardSort; dclf-architecture.md, "Ordered nodes"): a blended leaf's nearest
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

	FrameValues::FirstPersonPaletteStats FrameValues::TakeFirstPersonPaletteStats()
	{
		FirstPersonPaletteStats out;
		out.writes = firstPersonPalette.writes.exchange(0, std::memory_order_relaxed);
		out.units = firstPersonPalette.units.exchange(0, std::memory_order_relaxed);
		out.farthest = std::bit_cast<float>(firstPersonPalette.farthest.exchange(0, std::memory_order_relaxed));
		return out;
	}

	void FrameValues::SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_listedRoot, const RE::BSShaderProperty* a_fadeProperty,
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
		StoreListedRoot(a_listedRoot, a_out.sunEntry);
		StoreFadeNode(a_fadeProperty, a_out.lodFadeNode);
		StoreForwardSort(a_geometry, a_out.forwardSort);
		// A billboard's unturned world, base and pivot (BillboardRow.h), sampled regardless of the toggle so rows never depend on it.
		PlacementRows::SampleBillboardRow(a_geometry, a_out);
	}

	bool FrameValues::SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out)
	{
		const auto* geometry = a_slot < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		// A layer's row is its base's placement with its layer property's fade node (WriteLayer).
		const RE::BSShaderProperty* fadeProperty = a_tables.IsLayer(a_slot) ? LayerPropertyOf(*geometry) : geometry->GetGeometryRuntimeData().shaderProperty.get();
		SampleRow(*geometry, a_slot < a_tables.listedRoot.size() ? a_tables.listedRoot[a_slot] : nullptr, fadeProperty, a_out);
		return true;
	}

	namespace
	{
		using Plan = PlacementPlan;
		using PaletteRow = FrameValues::PaletteRow;

		using FrameLeases = std::array<org::services::BackingLease, FrameValues::kBuiltInPools>;

		// Render thread: binds a frame's backings (the built-in parts' and the registered pools') to everything submitted so far (the
		// frame's last submission) and lets them go, so a backing is not reused before that frame completed on the GPU.
		void BindFrameLeases(FrameLeases& a_leases, std::vector<org::services::BackingLease>& a_registered)
		{
			const auto point = SubmittedRetirementPoint();
			const auto bind = [&](org::services::BackingLease& a_lease) {
				if (a_lease) {
					a_lease.Bind(point);
					a_lease.Reset();
				}
			};
			for (auto& lease : a_leases)
				bind(lease);
			for (auto& lease : a_registered)
				bind(lease);
		}

		void SampleItem(const Plan::Item& a_item, BindlessPlacement& a_row, BindlessPlacement& a_layer)
		{
			const auto& geometry = *a_item.geometry;
			FrameValues::SampleRow(geometry, a_item.listedRoot.get(), geometry.GetGeometryRuntimeData().shaderProperty.get(), a_row);
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

	struct FrameValues::Impl
	{
		enum Part : std::size_t
		{
			kPlacements,
			kPalettes,
			kShading
		};
		static constexpr std::array<const char*, kBuiltInPools> kPartNames{ "placements", "palettes", "shading" };
		static constexpr std::array<std::uint32_t, kBuiltInPools> kStrides{ sizeof(BindlessPlacement), sizeof(PaletteRow), sizeof(BindlessShading) };

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

		static std::shared_ptr<org::services::BackingPool> MakePool(std::string_view a_name)
		{
			return org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
				{ .name = fmt::format("cs.dclf.frame-values.{}", a_name), .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
		}

		Impl()
		{
			for (std::size_t part = 0; part < kBuiltInPools; ++part)
				pools[part].pool = MakePool(kPartNames[part]);
		}

		/**
		 * @brief A registered pool (Extensions::RegisteredPools, by index): its entry and its Pool, made by the first Kick that leases
		 * (MakeRegistered), whatever the order of FrameValues' construction and the features' install.
		 */
		struct Registered
		{
			const Extensions::FrameValuePoolEntry* entry = nullptr;
			Pool pool;
			std::uint64_t needed = 0;  // render thread: the rows its kick asked for this frame
		};

		// Render thread.
		std::array<Pool, kBuiltInPools> pools;
		FrameLeases frameLeases;  // the kicked frame's backings, bound at EndFrame
		std::unique_ptr<Registered[]> registered;                        // made once (MakeRegistered)
		std::atomic<std::size_t> registeredMade{ 0 };                     // their count, published once made (TakePoolStats reads it)
		std::vector<org::services::BackingLease> registeredFrameLeases;  // the kicked frame's, by registered pool, bound at EndFrame
		std::vector<std::uint32_t> registeredIndex;                       // the descriptor of each one's frame backing (PoolIndex)
		std::shared_ptr<const Plan> plan;                              // the newest (its slots and palette capacity)
		std::uint64_t planSerial = 0;                                  // bumped with every newest plan
		// The kept set's root-view movers (the first person's; Kick's samples): every plan applied in order as Kick receives it, as the producers apply it.
		ankerl::unordered_dense::map<const void*, Plan::Item> rootViewMovers;
		std::vector<std::shared_ptr<const Plan>> unsampled;           // plans no producer has applied (and sampled the written slots of) yet
		std::vector<ShadingItem> unsampledShading;        // not yet taken by a producer
		/**
		 * @brief Shading items a producer could not sample (its engine-read lease refused), handed back to the next Kick: a slot is named
		 * once (a static object's accumulate patch; NameShading), so an item dropped here would leave its row unsampled for good (a
		 * zero row: MaterialData.z 0, and a blended decal draws nothing). A lock-free stack the producers push and Kick takes whole.
		 */
		struct RefusedShading
		{
			std::vector<ShadingItem> items;
			RefusedShading* next = nullptr;
		};
		std::atomic<RefusedShading*> refusedShading{ nullptr };
		void PushRefusedShading(std::vector<ShadingItem> a_items)
		{
			auto* node = new RefusedShading{ std::move(a_items), refusedShading.load(std::memory_order_relaxed) };
			while (!refusedShading.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
			}
		}
		/** @brief Every refused item, oldest first (the stack holds the newest push on top). */
		std::vector<ShadingItem> TakeRefusedShading()
		{
			std::vector<RefusedShading*> nodes;
			for (auto* node = refusedShading.exchange(nullptr, std::memory_order_acquire); node; node = node->next)
				nodes.push_back(node);
			std::vector<ShadingItem> items;
			for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
				items.insert(items.end(), std::make_move_iterator((*it)->items.begin()), std::make_move_iterator((*it)->items.end()));
				delete *it;
			}
			return items;
		}
		~Impl() { (void)TakeRefusedShading(); }
		std::vector<ShadingFieldValue> unsampledFieldValues;
		std::deque<std::pair<std::uint64_t, std::shared_ptr<const Plan>>> plans;  // held per kicked producer's sequence
		std::uint64_t seq = 0;                                          // the last kicked
		bool kicked = false;                                            // this frame
		bool unavailableLogged = false;

		// The producers', one at a time in sequence order.
		KeptWholeArray<BindlessPlacement> rows;
		KeptWholeArray<PaletteRow> palettes;
		KeptWholeArray<BindlessShading> shading;
		// The newest Kick's sequence (render thread's store): a registered pool's producer reads what its frame's publication holds only
		// while it is the newest (FrameValuePoolProducer::Newest), so within the window that publication is installed or was until this
		// frame's start.
		std::atomic<std::uint64_t> kickedSeq{ 0 };
		std::vector<std::uint8_t> listed;                       // dedup marks while listing items: per slot
		std::vector<std::uint8_t> blockListed;                  // per palette block
		std::vector<std::uint8_t> shadingListed;                // per slot, shading items
		// Written slots whose previous transform still differs from the current (a static's first update): resampled until equal.
		std::vector<Plan::Item> settling;
		std::vector<std::uint32_t> listedSlots, listedBlocks;  // the marks set in listed and blockListed (cleared alone)

		// The moved-only sampling (dclf B) over the kept placement set: the plans applied in order (ApplyPlans), each a delta, onto movers
		// and roots kept at stable indices (a freed one reused), and the index from move key to them (KeysOf) patched as they join,
		// change and leave. The references are the kept items' copies, handed to the render thread when they leave (EngineReleases).
		struct KeptMover
		{
			Plan::Item item;
			std::vector<const void*> keys;  // its entries in keyIndex
			bool live = false, keyed = false, everyFrame = false;
		};
		struct KeptRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::vector<Plan::Item> dependents;
			ankerl::unordered_dense::map<const void*, std::uint32_t> dependentAt;  // by geometry: its index in dependents
			std::vector<const void*> keys;
			bool live = false, keyed = false;
		};
		std::vector<KeptMover> keptMovers;
		std::vector<KeptRoot> keptRoots;
		std::vector<std::uint32_t> freeMovers, freeRoots;
		ankerl::unordered_dense::map<const void*, std::uint32_t> moverAt, rootAt;  // by geometry, by root node
		std::size_t liveMovers = 0, liveRoots = 0;
		struct Keyed
		{
			std::vector<std::uint32_t> movers, roots;  // indices into keptMovers, keptRoots
		};
		ankerl::unordered_dense::map<const void*, Keyed> keyIndex;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> slotRoots;  // a dependent's slot (and its layer's): its roots
		// Movers and roots whose keys a refused lease left unread (checked live and unkeyed when taken), and a palette rows class's items:
		// due every frame.
		std::vector<std::uint32_t> unkeyedMovers, unkeyedRoots, everyFrameMovers;
		std::vector<std::uint32_t> newMovers, newRoots;  // what the applied plans added or changed: due once
		// The plans applied since the last take, their resets and items; the kept set's parity (a plan's check) since its last line.
		std::atomic<std::uint64_t> plansApplied{ 0 }, planResets{ 0 }, planItems{ 0 };
		struct KeptParity
		{
			std::uint64_t checks = 0, differ = 0;
			std::string first;
		} keptParity;
		void ApplyPlan(const Plan& a_plan);
		void ClearKept();
		void KeyMover(std::uint32_t a_mover);
		void UnkeyMover(std::uint32_t a_mover);
		void KeyRoot(std::uint32_t a_root);
		void UnkeyRoot(std::uint32_t a_root);
		void UpsertMover(const Plan::Item& a_item);
		void RemoveMover(const void* a_geometry);
		void UpsertRoot(const Plan::Root& a_root);
		void RemoveRoot(const void* a_node);
		void RemoveDependent(const void* a_node, const void* a_geometry);
		void ListSlotRoot(const Plan::Item& a_item, std::uint32_t a_root);
		void UnlistSlotRoot(const Plan::Item& a_item, std::uint32_t a_root);
		void CheckKept(const Plan::Check& a_check);
		std::vector<const void*> drainedKeys;  // this producer's keys (scratch)
		std::vector<const void*> lastKeys;     // the previous producer's: the gate is "this frame or the last"
		// Keys due at the next producer: a mover's geometry or a root's node whose sample changed a row or palette (so it settles: a skin's
		// previous palette, a placement's previous world), or whose lease was refused.
		std::vector<const void*> owedKeys;
		std::vector<RE::NiPointer<RE::NiAVObject>> owedNodes;  // node events a refused lease left unnamed
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
		std::atomic<std::uint64_t> sampledSeq{ 0 };  // the newest producer whose sampling is done (WaitSampled), before its done
		void MarkSampled(std::uint64_t a_seq)
		{
			sampledSeq.store(a_seq, std::memory_order_release);
			sampledSeq.notify_all();
		}

		// Since the last report.
		std::atomic<std::uint64_t> frames{ 0 }, sampled{ 0 }, rowsChanged{ 0 }, skins{ 0 }, paletteRowsChanged{ 0 }, shadingSampled{ 0 }, shadingChanged{ 0 },
			wetnessChanged{ 0 }, defects{ 0 }, runs{ 0 }, bytes{ 0 }, refused{ 0 }, failures{ 0 }, settled{ 0 }, seeded{ 0 };
		std::atomic<std::uint64_t> movers{ 0 }, moversDue{ 0 }, roots{ 0 }, rootsDue{ 0 }, moveKeys{ 0 }, moveNodes{ 0 };
		std::atomic<std::uint64_t> sampleUs{ 0 }, uploadUs{ 0 }, maxTotalUs{ 0 };

		/**
		 * @brief A root view's member's rows and palettes (the first person's), sampled by the render thread at Kick, before Main::Draw
		 * moves the skeleton into its root's space.
		 */
		struct RootViewSample
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
			std::vector<ShadingFieldValue> fieldValues;  // in capture order
			// The shading rows the producer leaves: the plan's slots, or past them to the newest slot an item or field value names. A slot
			// binds (and is named, once: the accumulate patch's NameShading) before a placement plan covers it, and a dropped item is never
			// named again; its row would stay zero (MaterialData.z 0: a blended decal draws nothing).
			std::size_t shadingSlots = 0;
			// The shadingItems indices the producer's lease refusals left unsampled (RefusedShading), handed back once it is done.
			mutable std::vector<std::size_t> refusedShading;
			std::shared_ptr<const FrameGlobals> globals;  // the frame's engine globals (the shading rows' contributions)
			// Per part: the backing the frame's draws read (leased until the producer is done), and the journal's trim floor (the
			// oldest content a backing in circulation held at Kick; nullopt: none holds any, the journal keeps nothing).
			FrameLeases leases;
			std::array<std::optional<std::uint64_t>, kBuiltInPools> oldestContentEpochs;
			// Per registered pool: its kick's input (held until the producer is done), its backing and its journal's floor, as above.
			struct RegisteredPart
			{
				std::shared_ptr<const void> input;
				org::services::BackingLease lease;
				std::optional<std::uint64_t> oldestContentEpoch;
			};
			std::vector<RegisteredPart> registered;
			std::shared_ptr<rhi::TimelinePtr> timeline;
			std::shared_ptr<org::runtime::IUploadService> uploads;
			std::vector<RootViewSample> rootViewSamples;  // the render thread's samples (Kick)
			// The palette rows classes' frame inputs (their kicks'; by class index - 1).
			std::array<Api::PaletteRowsInput, Api::kPaletteRowsClasses> paletteInputs{};
		};
		// Sample's stages.
		struct Listed
		{
			const Plan::Item* item;
			bool palette;
			bool settles = false;  // a written slot (not a mover): kept while its transforms differ
			bool mover = false;    // a plan's mover: due again next frame when its sample changed a row or palette
		};
		static constexpr std::size_t kSampleGrain = 64;
		// Per chunk, marked afterwards (the journals are single-threaded).
		struct Changed
		{
			std::vector<std::uint32_t> slots;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row, rows)
			std::vector<const Plan::Item*> settling;
			std::vector<const void*> owed;  // movers due again next frame (owedKeys)
			std::uint64_t skins = 0, defects = 0;
			void Clear()
			{
				slots.clear();
				paletteRuns.clear();
				settling.clear();
				owed.clear();
				skins = defects = 0;
			}
		};
		// The producers' scratch, kept warm across frames (no allocation per frame once grown): Sample's items and per-chunk changes,
		// SampleShadingRows' items, SendRows' runs for one backing.
		std::vector<Listed> sampleItems;
		std::vector<Changed> sampleChanged;
		std::vector<const ShadingItem*> shadingItemsScratch;
		std::vector<org::StreamingUploadRange> uploadRanges;
		// The build's rows and palettes, after its one Mutable() each (written from many threads after it).
		struct SampleOut
		{
			std::vector<BindlessPlacement>& out;
			std::vector<PaletteRow>& paletteOut;
		};
		// The palette rows classes' runs (Api::PaletteRowsContribution), by class index - 1: the entry (null: none registered) and the
		// run's items of the class with a block, in the run's order.
		struct PaletteRun
		{
			const Extensions::PaletteRowsEntry* entry = nullptr;
			std::vector<Api::PaletteRowsItem> items;
		};
		using PaletteRuns = std::array<PaletteRun, Api::kPaletteRowsClasses>;
		// A class's item in a chunk, sampled and waiting for the class's write: where its marks go in the chunk's lists (its place among
		// the chunk's items), whether its layer changed, its row as sampled, its class, and its write and block in the chunk's.
		struct Deferred
		{
			const Listed* listed = nullptr;
			std::size_t slotsAt = 0, paletteRunsAt = 0, owedAt = 0;
			std::uint32_t paletteClass = 0, write = 0, paletteAt = 0;
			void* state = nullptr;  // its class's (Api::PaletteRowsItem::state)
			bool layerChanged = false;
			BindlessPlacement row{};
		};
		// A chunk's deferred items in the run's order, their writes (each class's together, in the run's order) and blocks.
		struct DeferredChunk
		{
			std::array<Deferred, kSampleGrain> items;
			std::array<Api::PaletteRowsWrite, kSampleGrain> writes;
			std::uint32_t count = 0, rowCount = 0;
			std::vector<PaletteRow> rows;
		};
		// A chunk's scratch (SampleListed).
		struct ItemScratch
		{
			BindlessPlacement row, layer;
			std::unique_ptr<DeferredChunk> deferred;  // made at the chunk's first deferred item
			std::array<std::size_t, Api::kPaletteRowsClasses> cursors{};  // per class: its next item in the run's
		};
		void ListItem(const Plan::Item& a_item, bool a_settles, bool a_mover, std::size_t a_rows, std::size_t a_blocks, std::vector<Listed>& a_items);
		void MarkSampledRoots();
		void ApplyRootViewSamples(const Job& a_job, const SampleOut& a_out, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked);
		void SampleItems(const Job& a_job, const std::vector<Listed>& a_items, const SampleOut& a_out, std::vector<Changed>& a_changed,
			std::atomic<std::uint64_t>& a_refused);
		static void BeginPaletteRuns(const Job& a_job, const std::vector<Listed>& a_items, PaletteRuns& a_runs);
		static void SampleChunk(const Job& a_job, const std::vector<Listed>& a_items, const PaletteRuns& a_runs, std::size_t a_begin, std::size_t a_end,
			const SampleOut& a_out, Changed& a_mine, std::atomic<std::uint64_t>& a_refused);
		static void SampleListed(const Listed& a_listed, const PaletteRuns& a_runs, const SampleOut& a_out, Changed& a_mine, ItemScratch& a_scratch,
			bool a_leased, std::atomic<std::uint64_t>& a_refused);
		static Deferred& Defer(const Listed& a_listed, std::uint32_t a_class, const Api::PaletteRowsItem& a_classItem, ItemScratch& a_scratch);
		static void WriteDeferred(const Job& a_job, const PaletteRuns& a_runs, DeferredChunk& a_deferred);
		static void FinishDeferred(const SampleOut& a_out, Changed& a_mine, DeferredChunk& a_deferred);
		static bool SamplePalette(const Plan::Item& a_item, const SampleOut& a_out, Changed& a_mine);
		void SampleRoots(const SampleOut& a_out, std::vector<std::vector<std::uint32_t>>& a_rootChanged, std::vector<std::uint8_t>& a_rootRefused,
			std::atomic<std::uint64_t>& a_refused);
		void MarkSampled(const std::vector<Changed>& a_changed, const std::vector<std::vector<std::uint32_t>>& a_rootChanged,
			const std::vector<std::uint8_t>& a_rootRefused, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked);
		// CheckEveryMover's stages.
		struct Fresh
		{
			BindlessPlacement row{}, layer{};
			bool sampled = false;
		};
		void SampleFreshMovers(const std::vector<BindlessPlacement>& a_out, std::vector<Fresh>& a_fresh, std::atomic<std::uint64_t>& a_palettesChecked,
			std::atomic<std::uint64_t>& a_palettesDiffer, std::string& a_firstPalette);
		void ApplyFreshRoots(const std::vector<BindlessPlacement>& a_out, const ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>& a_slotMover,
			std::vector<Fresh>& a_fresh, std::string& a_firstRoot);
		void CompareFreshRows(const std::vector<BindlessPlacement>& a_out, const std::vector<Fresh>& a_fresh, std::string& a_firstRow);
		void ReportMovedParity(std::uint64_t a_palettesChecked, std::uint64_t a_palettesDiffer, const std::string& a_firstRow,
			const std::string& a_firstPalette, const std::string& a_firstRoot);
		// Kick's stages (render thread).
		static void FrameStartRegistered(const std::shared_ptr<const FrameGlobals>& a_globals);
		void ApplyRootViewPlans(const std::vector<std::shared_ptr<const Plan>>& a_plans);
		void QueuePlans(std::vector<std::shared_ptr<const Plan>>& a_plans);
		void LeaseBackings(Job& a_job);
		void LeasePool(Pool& a_pool, std::string_view a_name, std::uint32_t a_stride, std::uint64_t a_needed, org::services::BackingLease& a_frameLease,
			org::services::BackingLease& a_jobLease, std::optional<std::uint64_t>& a_oldestContentEpoch);
		void MakeRegistered();
		void KickRegistered(Job& a_job, const FrameGlobals* a_globals);
		void SampleRootViews(Job& a_job, const SetSnapshot& a_claims);
		void Run(const Job& a_job);
		void Sample(const Job& a_job);
		void ApplyPlans(const Job& a_job);
		void MarkMover(std::uint32_t a_mover);
		void MarkRoot(std::uint32_t a_root);
		void MarkDue(const Job& a_job);
		void CheckEveryMover(const Job& a_job);
		void SampleShadingRows(const Job& a_job, std::size_t a_slots);
		// The registered pools' producer calls (sample, send): a_call(entry, producer) per pool, in order.
		template <class Call>
		void ForEachRegistered(const Job& a_job, Call&& a_call);
		void Upload(const Job& a_job);
		template <class T>
		void Send(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows);
		void SendRows(const org::services::BackingLease& a_lease, Pool& a_pool, std::string_view a_name, const Job& a_job, std::size_t a_count,
			std::uint64_t a_contentEpoch, const void* a_view, Api::FrameValuePoolProducer::EmitRowsFn a_emit);
		/** @brief The producer's: a_rows' build opened at the job's floor for a_part (every change forgotten without one). */
		template <class T>
		static void BeginBuild(const Job& a_job, Part a_part, KeptWholeArray<T>& a_rows)
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
		bool marked = false;
		try {
			Sample(a_job);
			MarkSampled(a_job.seq);
			marked = true;
			const auto sampledAt = std::chrono::steady_clock::now();
			sampleUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sampledAt - start).count());
			Upload(a_job);
			uploadUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - sampledAt).count());
		} catch (const std::exception& e) {
			if (failures++ == 0)
				logger::error("[DCLF] frame values {} failed: {}; the frame draws the rows its buffers held", a_job.seq, e.what());
		}
		if (!marked)
			MarkSampled(a_job.seq);
		// Always signal: every batch of the frame waits for it.
		if (!a_job.uploads || !a_job.uploads->QueueStreamingSignal(a_job.timeline, a_job.seq))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: frame values {} could not signal the frame's wait; the GPU would wait for it forever", a_job.seq));
		const auto total = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		for (auto max = maxTotalUs.load(std::memory_order_relaxed); total > max && !maxTotalUs.compare_exchange_weak(max, total, std::memory_order_relaxed);) {}
		++frames;
		done.store(a_job.seq, std::memory_order_release);
		done.notify_all();
	}

	namespace
	{
		// An item's references to the render thread (EngineReleases, at Present): a producer never drops one.
		void ReleaseItem(Plan::Item& a_item)
		{
			ReleaseLater(a_item.geometry);
			ReleaseLater(a_item.listedRoot);
		}

		template <class Index>
		void EraseIndex(std::vector<Index>& a_list, Index a_index)
		{
			if (const auto at = std::find(a_list.begin(), a_list.end(), a_index); at != a_list.end()) {
				*at = a_list.back();
				a_list.pop_back();
			}
		}
	}

	void FrameValues::Impl::ApplyPlans(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.ApplyPlans");
		// Every plan since the last producer, oldest first: each a delta of the set the one before left.
		for (const auto* written : a_job.written)
			ApplyPlan(*written);
	}

	void FrameValues::Impl::ApplyPlan(const Plan& a_plan)
	{
		++plansApplied;
		if (a_plan.reset) {
			++planResets;
			ClearKept();
		}
		planItems += a_plan.moversLeft.size() + a_plan.rootsLeft.size() + a_plan.dependentsLeft.size() + a_plan.movers.size() + a_plan.roots.size();
		for (const void* geometry : a_plan.moversLeft)
			RemoveMover(geometry);
		for (const void* node : a_plan.rootsLeft)
			RemoveRoot(node);
		for (const auto& [node, geometry] : a_plan.dependentsLeft)
			RemoveDependent(node, geometry);
		for (const auto& item : a_plan.movers)
			UpsertMover(item);
		for (const auto& root : a_plan.roots)
			UpsertRoot(root);
		if (a_plan.check)
			CheckKept(*a_plan.check);
	}

	void FrameValues::Impl::ClearKept()
	{
		for (auto& mover : keptMovers)
			ReleaseItem(mover.item);
		for (auto& root : keptRoots) {
			ReleaseLater(root.root);
			for (auto& dependent : root.dependents)
				ReleaseItem(dependent);
		}
		keptMovers.clear();
		keptRoots.clear();
		freeMovers.clear();
		freeRoots.clear();
		moverAt.clear();
		rootAt.clear();
		liveMovers = liveRoots = 0;
		keyIndex.clear();
		slotRoots.clear();
		unkeyedMovers.clear();
		unkeyedRoots.clear();
		everyFrameMovers.clear();
		newMovers.clear();
		newRoots.clear();
	}

	void FrameValues::Impl::KeyMover(std::uint32_t a_mover)
	{
		auto& mover = keptMovers[a_mover];
		// A palette rows class's item (particle quads face the camera): due every frame, no keys. A first-person member is the render
		// thread's (Kick).
		if (mover.item.itemClass != Api::FrameValueItemClass::None) {
			mover.everyFrame = true;
			everyFrameMovers.push_back(a_mover);
			return;
		}
		if (mover.item.firstPerson)
			return;
		EngineReadWindow::Lease lease;
		if (!lease) {
			unkeyedMovers.push_back(a_mover);
			return;
		}
		KeysOf(*mover.item.geometry, mover.keys);
		for (const void* key : mover.keys)
			keyIndex[key].movers.push_back(a_mover);
		mover.keyed = true;
	}

	void FrameValues::Impl::UnkeyMover(std::uint32_t a_mover)
	{
		auto& mover = keptMovers[a_mover];
		if (std::exchange(mover.everyFrame, false))
			EraseIndex(everyFrameMovers, a_mover);
		if (std::exchange(mover.keyed, false))
			for (const void* key : mover.keys)
				if (const auto it = keyIndex.find(key); it != keyIndex.end()) {
					EraseIndex(it->second.movers, a_mover);
					if (it->second.movers.empty() && it->second.roots.empty())
						keyIndex.erase(it);
				}
		mover.keys.clear();
	}

	void FrameValues::Impl::KeyRoot(std::uint32_t a_root)
	{
		auto& root = keptRoots[a_root];
		EngineReadWindow::Lease lease;
		if (!lease) {
			unkeyedRoots.push_back(a_root);
			return;
		}
		KeysOf(*root.root, root.keys);
		for (const void* key : root.keys)
			keyIndex[key].roots.push_back(a_root);
		root.keyed = true;
	}

	void FrameValues::Impl::UnkeyRoot(std::uint32_t a_root)
	{
		auto& root = keptRoots[a_root];
		if (std::exchange(root.keyed, false))
			for (const void* key : root.keys)
				if (const auto it = keyIndex.find(key); it != keyIndex.end()) {
					EraseIndex(it->second.roots, a_root);
					if (it->second.movers.empty() && it->second.roots.empty())
						keyIndex.erase(it);
				}
		root.keys.clear();
	}

	void FrameValues::Impl::UpsertMover(const Plan::Item& a_item)
	{
		if (!a_item.geometry)
			return;
		std::uint32_t m;
		if (const auto it = moverAt.find(a_item.geometry.get()); it != moverAt.end()) {
			// Changed: keyed again (its chain is read anew, as a new plan's was).
			m = it->second;
			UnkeyMover(m);
			ReleaseItem(keptMovers[m].item);
		} else {
			if (!freeMovers.empty()) {
				m = freeMovers.back();
				freeMovers.pop_back();
			} else {
				m = static_cast<std::uint32_t>(keptMovers.size());
				keptMovers.emplace_back();
			}
			moverAt.emplace(a_item.geometry.get(), m);
			keptMovers[m].live = true;
			++liveMovers;
		}
		keptMovers[m].item = a_item;
		if (moverDue.size() < keptMovers.size())
			moverDue.resize(keptMovers.size(), 0);
		KeyMover(m);
		newMovers.push_back(m);
	}

	void FrameValues::Impl::RemoveMover(const void* a_geometry)
	{
		const auto it = moverAt.find(a_geometry);
		if (it == moverAt.end())
			return;
		const std::uint32_t m = it->second;
		moverAt.erase(it);
		UnkeyMover(m);
		auto& mover = keptMovers[m];
		ReleaseItem(mover.item);
		mover.item = {};
		mover.live = false;
		freeMovers.push_back(m);
		--liveMovers;
	}

	void FrameValues::Impl::ListSlotRoot(const Plan::Item& a_item, std::uint32_t a_root)
	{
		for (const std::uint32_t slot : { a_item.slot, a_item.layerSlot })
			if (slot != kNoObjectSlot) {
				auto& rootsOf = slotRoots[slot];
				if (std::find(rootsOf.begin(), rootsOf.end(), a_root) == rootsOf.end())
					rootsOf.push_back(a_root);
			}
	}

	void FrameValues::Impl::UnlistSlotRoot(const Plan::Item& a_item, std::uint32_t a_root)
	{
		for (const std::uint32_t slot : { a_item.slot, a_item.layerSlot })
			if (const auto it = slot != kNoObjectSlot ? slotRoots.find(slot) : slotRoots.end(); it != slotRoots.end()) {
				EraseIndex(it->second, a_root);
				if (it->second.empty())
					slotRoots.erase(it);
			}
	}

	void FrameValues::Impl::UpsertRoot(const Plan::Root& a_root)
	{
		std::uint32_t r;
		if (const auto it = rootAt.find(a_root.root.get()); it != rootAt.end()) {
			r = it->second;
		} else {
			// A root handed out without its reference (the coordinator's defect, counted there) is left out with its dependents.
			if (!a_root.root)
				return;
			if (!freeRoots.empty()) {
				r = freeRoots.back();
				freeRoots.pop_back();
			} else {
				r = static_cast<std::uint32_t>(keptRoots.size());
				keptRoots.emplace_back();
			}
			rootAt.emplace(a_root.root.get(), r);
			auto& root = keptRoots[r];
			root.root = a_root.root;
			root.live = true;
			++liveRoots;
			if (rootDue.size() < keptRoots.size())
				rootDue.resize(keptRoots.size(), 0);
			KeyRoot(r);
		}
		auto& root = keptRoots[r];
		for (const auto& dependent : a_root.dependents) {
			if (!dependent.geometry)
				continue;
			if (const auto at = root.dependentAt.find(dependent.geometry.get()); at != root.dependentAt.end()) {
				auto& kept = root.dependents[at->second];
				UnlistSlotRoot(kept, r);
				ReleaseItem(kept);
				kept = dependent;
			} else {
				root.dependentAt.emplace(dependent.geometry.get(), static_cast<std::uint32_t>(root.dependents.size()));
				root.dependents.push_back(dependent);
			}
			ListSlotRoot(dependent, r);
		}
		newRoots.push_back(r);
	}

	void FrameValues::Impl::RemoveDependent(const void* a_node, const void* a_geometry)
	{
		const auto it = rootAt.find(a_node);
		if (it == rootAt.end())
			return;
		const std::uint32_t r = it->second;
		auto& root = keptRoots[r];
		const auto at = root.dependentAt.find(a_geometry);
		if (at == root.dependentAt.end())
			return;
		const std::uint32_t index = at->second;
		root.dependentAt.erase(at);
		UnlistSlotRoot(root.dependents[index], r);
		ReleaseItem(root.dependents[index]);
		if (index + 1 != root.dependents.size()) {
			root.dependents[index] = std::move(root.dependents.back());
			root.dependentAt.insert_or_assign(root.dependents[index].geometry.get(), index);
		}
		root.dependents.pop_back();
		// Its remaining dependents' rows are sampled again (a changed root was due once before as well).
		newRoots.push_back(r);
	}

	void FrameValues::Impl::RemoveRoot(const void* a_node)
	{
		const auto it = rootAt.find(a_node);
		if (it == rootAt.end())
			return;
		const std::uint32_t r = it->second;
		rootAt.erase(it);
		UnkeyRoot(r);
		auto& root = keptRoots[r];
		for (auto& dependent : root.dependents) {
			UnlistSlotRoot(dependent, r);
			ReleaseItem(dependent);
		}
		ReleaseLater(root.root);
		root = {};
		freeRoots.push_back(r);
		--liveRoots;
	}

	void FrameValues::Impl::CheckKept(const Plan::Check& a_check)
	{
		// Parity: the kept set (every plan applied in order) against the coordinator's whole rebuild after the same plan.
		ZoneScopedN("CS.DCLF.FrameValues.KeptParity");
		using Key = Plan::Key;
		auto byObject = [](const Key& a_left, const Key& a_right) { return a_left.object < a_right.object; };
		Plan::Check kept;
		for (const auto& mover : keptMovers)
			if (mover.live)
				kept.movers.push_back(Key::Of(mover.item));
		std::sort(kept.movers.begin(), kept.movers.end(), byObject);
		for (const auto& root : keptRoots) {
			if (!root.live)
				continue;
			auto& [node, keys] = kept.roots.emplace_back(root.root.get(), std::vector<Key>{});
			for (const auto& dependent : root.dependents)
				keys.push_back(Key::Of(dependent));
			std::sort(keys.begin(), keys.end(), byObject);
		}
		std::sort(kept.roots.begin(), kept.roots.end(), [](const auto& a_left, const auto& a_right) { return a_left.first < a_right.first; });
		auto& p = keptParity;
		++p.checks;
		const bool differs = kept.movers != a_check.movers || kept.roots != a_check.roots;
		if (differs) {
			++p.differ;
			if (p.first.empty()) {
				std::size_t m = 0;
				while (m < kept.movers.size() && m < a_check.movers.size() && kept.movers[m] == a_check.movers[m])
					++m;
				std::size_t r = 0;
				while (r < kept.roots.size() && r < a_check.roots.size() && kept.roots[r] == a_check.roots[r])
					++r;
				p.first = fmt::format("movers {} kept / {} rebuilt (first difference at {}), roots {} kept / {} rebuilt (first difference at {}: {} vs {})", kept.movers.size(),
					a_check.movers.size(), m, kept.roots.size(), a_check.roots.size(), r, r < kept.roots.size() ? kept.roots[r].first : nullptr,
					r < a_check.roots.size() ? a_check.roots[r].first : nullptr);
			}
		}
		if (p.checks < 8 && !differs)
			return;
		logger::info("[DCLF] frame values' kept placement set (parity): {} checks, {} differ{}{}", p.checks, p.differ, p.differ ? " <- KEPT SET DIFFERS; first: " : " <- OK",
			p.first);
		p = {};
	}

	void FrameValues::Impl::MarkMover(std::uint32_t a_mover)
	{
		if (a_mover >= keptMovers.size() || !keptMovers[a_mover].live)
			return;
		if (!std::exchange(moverDue[a_mover], 1))
			dueMovers.push_back(a_mover);
	}

	void FrameValues::Impl::MarkRoot(std::uint32_t a_root)
	{
		if (a_root >= keptRoots.size() || !keptRoots[a_root].live)
			return;
		if (!std::exchange(rootDue[a_root], 1))
			dueRoots.push_back(a_root);
	}

	void FrameValues::Impl::MarkDue(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Due");
		for (const auto m : dueMovers)
			moverDue[m] = 0;
		for (const auto r : dueRoots)
			rootDue[r] = 0;
		dueMovers.clear();
		dueRoots.clear();
		// The plans since the last producer onto the kept set (their joined and changed items due once, below).
		ApplyPlans(a_job);
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
		// One lease for them all: refused (the window closed), they wait for the next producer.
		if (!owedNodes.empty()) {
			EngineReadWindow::Lease lease;
			if (lease) {
				for (auto& owed : owedNodes) {
					const RE::NiAVObject* node = owed.get();
					drainedKeys.push_back(node);
					for (const RE::NiAVObject* at = node; at; at = at->parent)
						if (const void* owner = at->GetUserData()) {
							drainedKeys.push_back(owner);
							break;
						}
					ReleaseLater(owed);
				}
				owedNodes.clear();
			}
		}
		for (const void* key : drainedKeys)
			markKey(key);
		for (const void* key : lastKeys)
			markKey(key);
		for (const void* key : owedKeys)
			markKey(key);
		owedKeys.clear();
		std::swap(lastKeys, drainedKeys);
		// New to the set (or changed by a plan): once.
		for (const auto m : newMovers)
			MarkMover(m);
		for (const auto r : newRoots)
			MarkRoot(r);
		newMovers.clear();
		newRoots.clear();
		// Keys a refused lease left unread: due until read (then by their keys).
		// An entry freed, or keyed since (a later upsert), is dropped.
		std::erase_if(unkeyedMovers, [&](std::uint32_t a_mover) {
			auto& mover = keptMovers[a_mover];
			if (!mover.live || mover.keyed)
				return true;
			MarkMover(a_mover);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*mover.item.geometry, mover.keys);
			for (const void* key : mover.keys)
				keyIndex[key].movers.push_back(a_mover);
			mover.keyed = true;
			return true;
		});
		std::erase_if(unkeyedRoots, [&](std::uint32_t a_root) {
			auto& root = keptRoots[a_root];
			if (!root.live || root.keyed)
				return true;
			MarkRoot(a_root);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*root.root, root.keys);
			for (const void* key : root.keys)
				keyIndex[key].roots.push_back(a_root);
			root.keyed = true;
			return true;
		});
		// A palette rows class's items (particle quads face the camera): every frame.
		for (const auto m : everyFrameMovers)
			MarkMover(m);
		// Without the move writers' hooks (not installed, or their install failed) nothing says what moved: everything is due.
		if (!Scene::MoveEventsLive()) {
			for (std::uint32_t m = 0; m < keptMovers.size(); ++m)
				MarkMover(m);
			for (std::uint32_t r = 0; r < keptRoots.size(); ++r)
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
		auto& items = sampleItems;
		items.clear();
		items.reserve(dueMovers.size() + (a_job.written.empty() ? 0 : a_job.written.back()->written.size()) + settling.size());
		// The marks grow with the rows; only those set are cleared (at the end).
		const std::size_t blocks = paletteOut.size() / 2;
		if (listed.size() < out.size())
			listed.resize(out.size(), 0);
		if (blockListed.size() < blocks)
			blockListed.resize(blocks, 0);
		const std::size_t slots = out.size();
		for (const auto m : dueMovers)
			ListItem(keptMovers[m].item, false, true, slots, blocks, items);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				ListItem(item, true, false, slots, blocks, items);
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
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.listedRoot)));
				}
			}
		} handOver{ carried };
		for (const auto& item : carried)
			ListItem(item, true, false, slots, blocks, items);
		MarkSampledRoots();
		const SampleOut sampleOut{ out, paletteOut };
		std::uint64_t rootViewMarked = 0, rootViewPaletteMarked = 0;
		ApplyRootViewSamples(a_job, sampleOut, rootViewMarked, rootViewPaletteMarked);
		auto& changed = sampleChanged;
		for (auto& chunk : changed)
			chunk.Clear();
		changed.resize((items.size() + kSampleGrain - 1) / kSampleGrain);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		SampleItems(a_job, items, sampleOut, changed, refusedHere);
		// Roots: their dependents' sun entries and fade nodes, after the items wrote theirs.
		std::vector<std::vector<std::uint32_t>> rootChanged(dueRoots.size());
		std::vector<std::uint8_t> rootRefused(dueRoots.size(), 0);
		SampleRoots(sampleOut, rootChanged, rootRefused, refusedHere);
		std::uint64_t marked = 0, paletteMarked = 0;
		MarkSampled(changed, rootChanged, rootRefused, marked, paletteMarked);
		sampled += static_cast<std::uint64_t>(items.size() + a_job.rootViewSamples.size());
		rowsChanged += marked + rootViewMarked;
		paletteRowsChanged += paletteMarked + rootViewPaletteMarked;
		movers += liveMovers;
		moversDue += dueMovers.size();
		roots += liveRoots;
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
		SampleShadingRows(a_job, a_job.shadingSlots);
		ForEachRegistered(a_job, [](const Extensions::FrameValuePoolEntry& a_entry, Api::FrameValuePoolProducer& a_producer) { a_entry.sample(a_producer); });
	}

	// First-person items are the render thread's (RootViewSample), except a palette rows class's (particle members, expanded from
	// snapshots).
	void FrameValues::Impl::ListItem(const Plan::Item& a_item, bool a_settles, bool a_mover, std::size_t a_rows, std::size_t a_blocks, std::vector<Listed>& a_items)
	{
		if ((a_item.firstPerson && a_item.itemClass == Api::FrameValueItemClass::None) || a_item.slot >= a_rows || !a_item.geometry || listed[a_item.slot])
			return;
		listed[a_item.slot] = 1;
		listedSlots.push_back(a_item.slot);
		const bool palette = a_item.boneRows && std::size_t(a_item.boneOffset) + a_item.boneRows <= a_blocks && !blockListed[a_item.boneOffset];
		if (palette) {
			blockListed[a_item.boneOffset] = 1;
			listedBlocks.push_back(a_item.boneOffset);
		}
		a_items.push_back({ &a_item, palette, a_settles, a_mover });
	}

	// A root writes its dependents' sun entry and fade node after their rows: again whenever one of them was sampled.
	void FrameValues::Impl::MarkSampledRoots()
	{
		for (const auto slot : listedSlots)
			if (const auto it = slotRoots.find(slot); it != slotRoots.end())
				for (const auto r : it->second)
					MarkRoot(r);
	}

	// The render thread's root-view samples (Kick) onto the rows and palettes.
	void FrameValues::Impl::ApplyRootViewSamples(const Job& a_job, const SampleOut& a_out, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked)
	{
		auto& out = a_out.out;
		auto& paletteOut = a_out.paletteOut;
		for (const auto& sample : a_job.rootViewSamples) {
			if (sample.slot >= out.size())
				continue;
			if (std::memcmp(&out[sample.slot], &sample.row, sizeof(sample.row)) != 0) {
				out[sample.slot] = sample.row;
				rows.Mark(sample.slot);
				++a_marked;
			}
			if (sample.layerSlot != kNoObjectSlot && sample.layerSlot < out.size() && std::memcmp(&out[sample.layerSlot], &sample.layer, sizeof(sample.layer)) != 0) {
				out[sample.layerSlot] = sample.layer;
				rows.Mark(sample.layerSlot);
				++a_marked;
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
				a_paletteMarked += 2 * sample.boneRows;
			}
		}
	}

	void FrameValues::Impl::SampleItems(const Job& a_job, const std::vector<Listed>& a_items, const SampleOut& a_out, std::vector<Changed>& a_changed,
		std::atomic<std::uint64_t>& a_refused)
	{
		auto& executor = SceneScheduler::Executor();
		// The palette rows classes' runs, before the parallel loop (a class's beginRun is the producer's alone: Features/Particles follows
		// each particle member's snapshot record).
		PaletteRuns paletteRuns;
		BeginPaletteRuns(a_job, a_items, paletteRuns);
		executor.ParallelFor("CS.DCLF.FrameValues.Items", a_items.size(), kSampleGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Items");
			SampleChunk(a_job, a_items, paletteRuns, a_begin, a_end, a_out, a_changed[a_begin / kSampleGrain], a_refused);
		});
		for (std::size_t c = 0; c < paletteRuns.size(); ++c)
			if (paletteRuns[c].entry)
				paletteRuns[c].entry->contribution.endRun({ a_job.paletteInputs[c], paletteRuns[c].items });
	}

	// Each registered class's items with a block, in the run's order, handed to its beginRun.
	void FrameValues::Impl::BeginPaletteRuns(const Job& a_job, const std::vector<Listed>& a_items, PaletteRuns& a_runs)
	{
		std::array<std::size_t, Api::kPaletteRowsClasses> counts{};
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			a_runs[c].entry = Extensions::PaletteRows(static_cast<Api::FrameValueItemClass>(c + 1));
		const auto classOf = [&](const Listed& a_listed) -> std::size_t {
			const auto c = static_cast<std::size_t>(a_listed.item->itemClass);
			return a_listed.palette && c != 0 && c <= a_runs.size() && a_runs[c - 1].entry ? c : 0;
		};
		for (const auto& listed : a_items)
			if (const auto c = classOf(listed))
				++counts[c - 1];
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			a_runs[c].items.reserve(counts[c]);
		for (std::uint32_t i = 0; i < a_items.size(); ++i)
			if (const auto c = classOf(a_items[i]))
				a_runs[c - 1].items.push_back({ a_items[i].item->geometry.get(), nullptr, i });
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			if (a_runs[c].entry)
				a_runs[c].entry->contribution.beginRun({ a_job.paletteInputs[c], a_runs[c].items });
	}

	// A chunk of the run's items: each sampled in order (a palette rows class's item deferred), then each class's write of its deferred
	// items (its sampleChunk, once), then those finished where each stood, so the chunk's lists keep the run's order.
	void FrameValues::Impl::SampleChunk(const Job& a_job, const std::vector<Listed>& a_items, const PaletteRuns& a_runs, std::size_t a_begin, std::size_t a_end,
		const SampleOut& a_out, Changed& a_mine, std::atomic<std::uint64_t>& a_refused)
	{
		ItemScratch scratch;
		// Each class's first item in the chunk (its items are in the run's order).
		for (std::size_t c = 0; c < a_runs.size(); ++c) {
			const auto& items = a_runs[c].items;
			const auto first = std::lower_bound(items.begin(), items.end(), a_begin,
				[](const Api::PaletteRowsItem& a_item, std::size_t a_listed) { return a_item.listed < a_listed; });
			scratch.cursors[c] = static_cast<std::size_t>(first - items.begin());
		}
		{
			// One engine-read lease for the chunk's items, taken at its first item that reads the engine (a first-person class item
			// reads none): refused, every such item of the chunk is refused. Let go before the classes' writes, which read no engine.
			std::optional<EngineReadWindow::Lease> lease;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				if (!lease && !a_items[i].item->firstPerson)
					lease.emplace();
				SampleListed(a_items[i], a_runs, a_out, a_mine, scratch, lease && *lease, a_refused);
			}
		}
		if (!scratch.deferred)
			return;
		WriteDeferred(a_job, a_runs, *scratch.deferred);
		FinishDeferred(a_out, a_mine, *scratch.deferred);
	}

	void FrameValues::Impl::SampleListed(const Listed& a_listed, const PaletteRuns& a_runs, const SampleOut& a_out, Changed& a_mine, ItemScratch& a_scratch,
		bool a_leased, std::atomic<std::uint64_t>& a_refused)
	{
		auto& out = a_out.out;
		auto& row = a_scratch.row;
		auto& layer = a_scratch.layer;
		const auto& [itemPointer, palette, settles, mover] = a_listed;
		const auto& item = *itemPointer;
		// A palette rows class's item with a block (a particle member): its block and placement are its class's write, after the chunk's
		// samples (Defer); its class's run listed it, refused or not.
		const auto paletteClass = static_cast<std::uint32_t>(item.itemClass);
		const bool deferred = palette && paletteClass != 0 && paletteClass <= a_runs.size() && a_runs[paletteClass - 1].entry;
		const Api::PaletteRowsItem* classItem = deferred ? &a_runs[paletteClass - 1].items[a_scratch.cursors[paletteClass - 1]++] : nullptr;
		// A first-person class item reads no engine memory here: no lease, never settling. Any other is read under its chunk's
		// lease (a_leased: held).
		const bool firstPerson = item.firstPerson;
		if (!firstPerson && !a_leased) {
			++a_refused;
			if (settles)
				a_mine.settling.push_back(&item);  // not sampled: the next producer takes it
			if (mover)
				a_mine.owed.push_back(item.geometry.get());
			return;
		}
		bool changes = false;
		if (firstPerson)
			row = out[item.slot];
		else
			SampleItem(item, row, layer);
		if (settles && !firstPerson && std::memcmp(&item.geometry->world, &DrawnPreviousWorld(*item.geometry), sizeof(RE::NiTransform)) != 0)
			a_mine.settling.push_back(&item);
		// A deferred item's row waits for its class's placement (a particle snapshot's transforms and bound: its data is never read here).
		Deferred* waiting = classItem ? &Defer(a_listed, paletteClass - 1, *classItem, a_scratch) : nullptr;
		if (waiting) {
			waiting->row = row;
			waiting->slotsAt = a_mine.slots.size();
		} else if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
			out[item.slot] = row;
			a_mine.slots.push_back(item.slot);
			changes = true;
		}
		if (!firstPerson && item.layerSlot != kNoObjectSlot && item.layerSlot < out.size() && std::memcmp(&out[item.layerSlot], &layer, sizeof(layer)) != 0) {
			out[item.layerSlot] = layer;
			a_mine.slots.push_back(item.layerSlot);
			changes = true;
		}
		if (waiting) {
			waiting->layerChanged = changes;
			waiting->paletteRunsAt = a_mine.paletteRuns.size();
			waiting->owedAt = a_mine.owed.size();
			return;
		}
		if (palette && SamplePalette(item, a_out, a_mine))
			changes = true;
		// A mover whose sample changed something is sampled again next frame, until a sample changes nothing: its previous world
		// and previous palette settle on the current ones the frame after its motion stops.
		if (mover && changes)
			a_mine.owed.push_back(item.geometry.get());
	}

	// A deferred item's place in the chunk's: its class, its class's state for it, and its block in the chunk's rows.
	FrameValues::Impl::Deferred& FrameValues::Impl::Defer(const Listed& a_listed, std::uint32_t a_class, const Api::PaletteRowsItem& a_classItem, ItemScratch& a_scratch)
	{
		if (!a_scratch.deferred)
			a_scratch.deferred = std::make_unique<DeferredChunk>();
		auto& chunk = *a_scratch.deferred;
		auto& deferred = chunk.items[chunk.count++];
		deferred.listed = &a_listed;
		deferred.paletteClass = a_class;
		deferred.state = a_classItem.state;
		deferred.paletteAt = chunk.rowCount;
		chunk.rowCount += a_listed.item->boneRows;
		return deferred;
	}

	// The chunk's blocks, then each class's writes (its items together, in the run's order), handed to its sampleChunk once.
	void FrameValues::Impl::WriteDeferred(const Job& a_job, const PaletteRuns& a_runs, DeferredChunk& a_deferred)
	{
		auto& chunk = a_deferred;
		chunk.rows.resize(chunk.rowCount);
		std::uint32_t next = 0;
		for (std::uint32_t c = 0; c < a_runs.size(); ++c) {
			const std::uint32_t first = next;
			for (std::uint32_t k = 0; k < chunk.count; ++k) {
				auto& deferred = chunk.items[k];
				if (deferred.paletteClass != c)
					continue;
				const auto& item = *deferred.listed->item;
				deferred.write = next;
				chunk.writes[next++] = { deferred.state, item.firstPerson, item.boneRows, chunk.rows[deferred.paletteAt].v };
			}
			if (next != first)
				a_runs[c].entry->contribution.sampleChunk({ a_job.paletteInputs[c], std::span(chunk.writes).subspan(first, next - first), firstPersonPalette });
		}
	}

	// The deferred items in reverse, each compared and marked where it stood among the chunk's items: an insertion at a later place first
	// leaves every earlier place as it was, so the chunk's lists hold what sampling the items one by one in order gave.
	void FrameValues::Impl::FinishDeferred(const SampleOut& a_out, Changed& a_mine, DeferredChunk& a_deferred)
	{
		auto& out = a_out.out;
		auto& paletteOut = a_out.paletteOut;
		for (std::uint32_t k = a_deferred.count; k-- > 0;) {
			auto& deferred = a_deferred.items[k];
			const auto& write = a_deferred.writes[deferred.write];
			const auto& item = *deferred.listed->item;
			auto& row = deferred.row;
			// The class's placement (a particle snapshot's: its world, the previous the same, and its bound).
			if (write.placed) {
				std::memcpy(row.world, write.world, sizeof(row.world));
				std::memcpy(row.previousWorld, write.world, sizeof(row.previousWorld));
				std::memcpy(row.bound, write.bound, sizeof(row.bound));
			}
			bool changes = deferred.layerChanged;
			if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
				out[item.slot] = row;
				a_mine.slots.insert(a_mine.slots.begin() + static_cast<std::ptrdiff_t>(deferred.slotsAt), item.slot);
				changes = true;
			}
			// Its block's current rows (a class's block has no previous rows).
			const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
			const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
			if (std::memcmp(&paletteOut[at.current], write.palette, bytes) != 0) {
				std::memcpy(&paletteOut[at.current], write.palette, bytes);
				a_mine.paletteRuns.emplace(a_mine.paletteRuns.begin() + static_cast<std::ptrdiff_t>(deferred.paletteRunsAt), at.current, item.boneRows);
				changes = true;
			}
			if (deferred.listed->mover && changes)
				a_mine.owed.insert(a_mine.owed.begin() + static_cast<std::ptrdiff_t>(deferred.owedAt), item.geometry.get());
		}
	}

	// A listed skin's palette block (true when any row changed).
	bool FrameValues::Impl::SamplePalette(const Plan::Item& a_item, const SampleOut& a_out, Changed& a_mine)
	{
		const auto& item = a_item;
		auto& paletteOut = a_out.paletteOut;
		// Run the engine's palette update, then copy rows that differ (the frame after its motion stops, the update copies the
		// current palette to the previous: the change that keeps the skin due once more, so its motion vectors settle).
		auto& geometry = *item.geometry;
		const float* current = nullptr;
		const float* previous = nullptr;
		if (!DCLF::SkinPalettes::Sample(geometry, item.boneRows, current, previous)) {
			++a_mine.defects;
			return false;
		}
		++a_mine.skins;
		const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
		const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
		if (std::memcmp(&paletteOut[at.current], current, bytes) != 0 || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
			std::memcpy(&paletteOut[at.current], current, bytes);
			std::memcpy(&paletteOut[at.previous], previous, bytes);
			a_mine.paletteRuns.emplace_back(at.current, 2 * item.boneRows);
			return true;
		}
		return false;
	}
	void FrameValues::Impl::SampleRoots(const SampleOut& a_out, std::vector<std::vector<std::uint32_t>>& a_rootChanged, std::vector<std::uint8_t>& a_rootRefused,
		std::atomic<std::uint64_t>& a_refused)
	{
		auto& out = a_out.out;
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.Roots", dueRoots.size(), 16, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Roots");
			// One engine-read lease for the chunk: refused, each of its roots is.
			EngineReadWindow::Lease lease;
			for (std::size_t r = a_begin; r < a_end; ++r) {
				const auto& root = keptRoots[dueRoots[r]];
				if (!lease) {
					++a_refused;
					a_rootRefused[r] = 1;
					continue;
				}
				float entry[4];
				StoreListedRoot(root.root.get(), entry);
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
						a_rootChanged[r].push_back(a_slot);
					};
					write(dependent.slot, dependent.geometry->GetGeometryRuntimeData().shaderProperty.get());
					if (dependent.layerSlot != kNoObjectSlot && dependent.layerSlot < out.size())
						write(dependent.layerSlot, LayerPropertyOf(*dependent.geometry));
				}
			}
		});
	}

	// The chunks' and roots' changes into the journals (single-threaded), and what is due again next frame.
	void FrameValues::Impl::MarkSampled(const std::vector<Changed>& a_changed, const std::vector<std::vector<std::uint32_t>>& a_rootChanged,
		const std::vector<std::uint8_t>& a_rootRefused, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked)
	{
		for (const auto& chunk : a_changed) {
			for (const auto* item : chunk.settling)
				settling.push_back(*item);
			owedKeys.insert(owedKeys.end(), chunk.owed.begin(), chunk.owed.end());
		}
		settled += settling.size();
		for (const auto& chunk : a_changed) {
			for (const auto slot : chunk.slots)
				rows.Mark(slot);
			a_marked += chunk.slots.size();
			for (const auto& [first, count] : chunk.paletteRuns) {
				palettes.MarkRange(first, count);
				a_paletteMarked += count;
			}
			skins += chunk.skins;
			defects += chunk.defects;
		}
		for (std::size_t r = 0; r < dueRoots.size(); ++r) {
			for (const auto slot : a_rootChanged[r])
				rows.Mark(slot);
			a_marked += a_rootChanged[r].size();
			// Likewise a root whose sample changed a row (or was refused) is due next frame.
			if (a_rootRefused[r] || !a_rootChanged[r].empty())
				owedKeys.push_back(keptRoots[dueRoots[r]].root.get());
		}
	}

	void FrameValues::Impl::CheckEveryMover(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.MovedParity");
		// The persistent parity's check of the moved-only sampling (dclf B): every mover's and root's rows as sampling them all now gives
		// them (nothing written), against the rows the producers left. A difference is a move no event named (a frozen row). A palette
		// rows class's items (sampled every frame: particle systems) and first-person members (the render thread's) are not checked. A
		// skin's palette update is idempotent per frame (a due skin's runs again for nothing); a skin not due runs it here as the native
		// bone setter would.
		(void)a_job;
		const auto& out = rows.Get();
		std::vector<Fresh> fresh(keptMovers.size());
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> slotMover;  // a checked mover's slot and its layer's: its index
		for (std::uint32_t m = 0; m < keptMovers.size(); ++m) {
			const auto& item = keptMovers[m].item;
			if (!keptMovers[m].live || item.firstPerson || item.itemClass != Api::FrameValueItemClass::None || !item.geometry || item.slot >= out.size())
				continue;
			slotMover.insert_or_assign(item.slot, m);
			if (item.layerSlot != kNoObjectSlot && item.layerSlot < out.size())
				slotMover.insert_or_assign(item.layerSlot, m);
		}
		std::atomic<std::uint64_t> palettesChecked{ 0 }, palettesDiffer{ 0 };
		std::string firstPalette;
		SampleFreshMovers(out, fresh, palettesChecked, palettesDiffer, firstPalette);
		std::string firstRoot;
		ApplyFreshRoots(out, slotMover, fresh, firstRoot);
		std::string firstRow;
		CompareFreshRows(out, fresh, firstRow);
		ReportMovedParity(palettesChecked.load(), palettesDiffer.load(), firstRow, firstPalette, firstRoot);
	}

	// Every checked mover sampled fresh (nothing written), and its palette against the one the producers left.
	void FrameValues::Impl::SampleFreshMovers(const std::vector<BindlessPlacement>& a_out, std::vector<Fresh>& a_fresh, std::atomic<std::uint64_t>& a_palettesChecked,
		std::atomic<std::uint64_t>& a_palettesDiffer, std::string& a_firstPalette)
	{
		const auto& out = a_out;
		const auto& paletteOut = palettes.Get();
		auto& fresh = a_fresh;
		std::atomic_flag firstPaletteTaken;
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.MovedParity", keptMovers.size(), 64, [&](std::size_t a_begin, std::size_t a_end) {
			for (std::size_t m = a_begin; m < a_end; ++m) {
				const auto& item = keptMovers[m].item;
				if (!keptMovers[m].live || item.firstPerson || item.itemClass != Api::FrameValueItemClass::None || !item.geometry || item.slot >= out.size())
					continue;
				EngineReadWindow::Lease lease;
				if (!lease)
					continue;
				SampleItem(item, fresh[m].row, fresh[m].layer);
				fresh[m].sampled = true;
				if (!item.boneRows || std::size_t(item.boneOffset) + item.boneRows > paletteOut.size() / 2)
					continue;
				auto& geometry = *item.geometry;
				const float* current = nullptr;
				const float* previous = nullptr;
				if (!DCLF::SkinPalettes::Sample(geometry, item.boneRows, current, previous))
					continue;
				++a_palettesChecked;
				const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
				const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
				const bool currentDiffers = std::memcmp(&paletteOut[at.current], current, bytes) != 0;
				if (currentDiffers || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
					++a_palettesDiffer;
					if (!firstPaletteTaken.test_and_set())
						a_firstPalette = fmt::format("palette of slot {} ('{}', {}): {}", item.slot, geometry.name.c_str(), moverDue[m] ? "due" : "not due",
							currentDiffers ? "current" : "previous");
				}
			}
		});
	}

	// The roots onto the fresh rows of their dependents, as the producer orders them (after the items); a dependent that is no
	// checked mover is compared on the two values the root writes.
	void FrameValues::Impl::ApplyFreshRoots(const std::vector<BindlessPlacement>& a_out, const ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>& a_slotMover,
		std::vector<Fresh>& a_fresh, std::string& a_firstRoot)
	{
		const auto& out = a_out;
		const auto& slotMover = a_slotMover;
		auto& fresh = a_fresh;
		auto& firstRoot = a_firstRoot;
		auto& p = movedParity;
		for (std::uint32_t r = 0; r < keptRoots.size(); ++r) {
			const auto& root = keptRoots[r];
			EngineReadWindow::Lease lease;
			if (!lease || !root.live || !root.root)
				continue;
			float entry[4];
			StoreListedRoot(root.root.get(), entry);
			for (const auto& dependent : root.dependents) {
				if (dependent.firstPerson || dependent.slot >= out.size() || !dependent.geometry)
					continue;
				const auto apply = [&](std::uint32_t a_slot, const RE::BSShaderProperty* a_property) {
					float fade[4];
					StoreFadeNode(a_property, fade);
					if (const auto it = slotMover.find(a_slot); it != slotMover.end() && fresh[it->second].sampled) {
						auto& target = a_slot == keptMovers[it->second].item.slot ? fresh[it->second].row : fresh[it->second].layer;
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
	}

	// Every fresh row against the row the producers left.
	void FrameValues::Impl::CompareFreshRows(const std::vector<BindlessPlacement>& a_out, const std::vector<Fresh>& a_fresh, std::string& a_firstRow)
	{
		const auto& out = a_out;
		const auto& fresh = a_fresh;
		auto& firstRow = a_firstRow;
		auto& p = movedParity;
		for (std::uint32_t m = 0; m < keptMovers.size(); ++m) {
			if (!fresh[m].sampled)
				continue;
			const auto& item = keptMovers[m].item;
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
	}

	void FrameValues::Impl::ReportMovedParity(std::uint64_t a_palettesChecked, std::uint64_t a_palettesDiffer, const std::string& a_firstRow,
		const std::string& a_firstPalette, const std::string& a_firstRoot)
	{
		auto& p = movedParity;
		p.palettes += a_palettesChecked;
		p.palettesDiffer += a_palettesDiffer;
		++p.checks;
		p.movers += liveMovers;
		p.moversDue += dueMovers.size();
		p.roots += liveRoots;
		p.rootsDue += dueRoots.size();
		if (p.first.empty())
			p.first = !a_firstRow.empty() ? a_firstRow : !a_firstPalette.empty() ? a_firstPalette : a_firstRoot;
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

	namespace
	{
		// The shading fields' bytes in a row (by Api::ShadingRowField: BindlessShading's layout).
		struct ShadingFieldRange
		{
			std::size_t offset = 0, bytes = 0;
		};
		constexpr std::array<ShadingFieldRange, Extensions::kShadingRowFields> kShadingFieldRanges{ {
			{ offsetof(BindlessShading, skinPerGeometry), sizeof(BindlessShading::skinPerGeometry) },
		} };
		static_assert(sizeof(BindlessShading::skinPerGeometry) <= sizeof(ShadingFieldValue::value));
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
		// Each slot once, by its newest item (the slot may have been reused since). The marks grow with the rows; only those set are
		// cleared (the items' slots).
		auto& items = shadingItemsScratch;
		items.clear();
		items.reserve(a_job.shadingItems.size());
		if (shadingListed.size() < out.size())
			shadingListed.resize(out.size(), 0);
		for (auto it = a_job.shadingItems.rbegin(); it != a_job.shadingItems.rend(); ++it)
			if (it->slot < out.size() && it->property && !std::exchange(shadingListed[it->slot], 1))
				items.push_back(&*it);
		for (const auto* item : items)
			shadingListed[item->slot] = 0;
		constexpr std::size_t kGrain = 128;
		std::vector<std::vector<std::uint32_t>> changed((items.size() + kGrain - 1) / kGrain);
		std::vector<std::vector<std::size_t>> refusedItems(changed.size());
		std::atomic<std::uint64_t> refusedHere{ 0 };
		const auto fields = Extensions::ShadingFields();
		SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.Shading", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.FrameValues.Shading");
			FrameGlobals::Scope scope(a_job.globals);  // the features' globals (SampleShading: the shading contributions)
			auto& mine = changed[a_begin / kGrain];
			// One engine-read lease for the chunk: refused, each of its items is, and goes back to the next frame's producer.
			EngineReadWindow::Lease lease;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& item = *items[i];
				if (!lease) {
					++refusedHere;
					refusedItems[a_begin / kGrain].push_back(static_cast<std::size_t>(items[i] - a_job.shadingItems.data()));
					continue;
				}
				BindlessShading row = out[item.slot];
				SampleShading(*item.property.get(), item.pass, item.member, row);
				// A shading field is its owner's (from the capture, below): zero for a slot it names no member (only actors have wetness).
				for (const auto& field : fields) {
					const auto which = static_cast<std::uint32_t>(field.contribution.field);
					if (!(item.fieldsOwned & (1u << which)))
						std::memset(reinterpret_cast<std::byte*>(&row) + kShadingFieldRanges[which].offset, 0, kShadingFieldRanges[which].bytes);
				}
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
		for (const auto& [slot, field, value] : a_job.fieldValues) {
			if (slot >= out.size())
				continue;
			const auto& range = kShadingFieldRanges[static_cast<std::size_t>(field)];
			auto* fieldBytes = reinterpret_cast<std::byte*>(&out[slot]) + range.offset;
			if (std::memcmp(fieldBytes, value.data(), range.bytes) == 0)
				continue;
			std::memcpy(fieldBytes, value.data(), range.bytes);
			shading.Mark(slot);
			++wetMarked;
		}
		for (const auto& chunk : refusedItems)
			a_job.refusedShading.insert(a_job.refusedShading.end(), chunk.begin(), chunk.end());
		shadingSampled += items.size();
		shadingChanged += marked;
		wetnessChanged += wetMarked;
		if (const auto refusals = refusedHere.load())
			refused += refusals;
	}

	// The catch-up: the journal's runs since the backing's content (all of them for a backing that holds nothing, or less than the
	// journal's floor), into the leased backing (PooledBackingLease: no frame reads it before the one that leased it, which waits for
	// this producer's signal); then the backing is at the journal's content epoch. a_view is the rows (a KeptWholeArray), a_emit its Emit.
	void FrameValues::Impl::SendRows(const org::services::BackingLease& a_lease, Pool& a_pool, std::string_view a_name, const Job& a_job, std::size_t a_count,
		std::uint64_t a_contentEpoch, const void* a_view, Api::FrameValuePoolProducer::EmitRowsFn a_emit)
	{
		auto& backing = *a_lease;  // the lease holder fills it (contentEpoch is its)
		if (a_count > backing.capacityClass)
			throw std::runtime_error(fmt::format("its {} backing holds {} rows of {}", a_name, backing.capacityClass, a_count));
		// The runs gathered (the scratch keeps its capacity), then one batched upload into the backing: one ticket, its staging page
		// mapped once, one wake of the uploader, a copy region per run. The runs point into the rows, which nothing writes meanwhile.
		auto& ranges = uploadRanges;
		ranges.clear();
		std::uint64_t sentBytes = 0;
		{
			ZoneScopedN("CS.DCLF.FrameValues.Send.Emit");
			auto put = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				ranges.push_back({ a_data, a_bytes, a_offset });
				sentBytes += a_bytes;
			};
			using Put = decltype(put);
			const Api::FrameValuePoolProducer::RowSink sink{ &put, [](void* a_owner, const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
																(*static_cast<Put*>(a_owner))(a_data, a_bytes, a_offset);
															} };
			a_emit(a_view, backing.contentEpoch.load(std::memory_order_acquire), sink);
		}
		if (!ranges.empty()) {
			ZoneScopedN("CS.DCLF.FrameValues.Send.Queue");
			auto ticket = a_job.uploads->QueueTrackedStreamingUploadRanges(ranges,
				org::WorkerOwnedDestination{ backing.buffer, org::WorkerOwnedDestination::Ownership::PooledBackingLease });
			if (!ticket || ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled)
				throw std::runtime_error("the dedicated uploader refused its rows");
		}
		const std::uint64_t sent = ranges.size();
		backing.contentEpoch.store(a_contentEpoch, std::memory_order_release);
		a_pool.newestContentEpoch.store(a_contentEpoch, std::memory_order_release);
		a_pool.runs += sent;
		a_pool.bytes += sentBytes;
		runs += sent;
		bytes += sentBytes;
	}

	// Emitted from the rows themselves (KeptArray::Emit: no view's copy of the journal), which no producer writes while it sends.
	template <class T>
	void FrameValues::Impl::Send(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows)
	{
		SendRows(a_job.leases[a_part], pools[a_part], kPartNames[a_part], a_job, a_rows.Size(), a_rows.ContentEpoch(), &a_rows,
			[](const void* a_view, std::uint64_t a_contentEpoch, const Api::FrameValuePoolProducer::RowSink& a_sink) {
				static_cast<const KeptWholeArray<T>*>(a_view)->Emit(a_contentEpoch, a_sink);
			});
	}

	template <class Call>
	void FrameValues::Impl::ForEachRegistered(const Job& a_job, Call&& a_call)
	{
		// The pools' sends reach their backings through here (FrameValuePoolProducer::Send).
		struct Owner
		{
			Impl& impl;
			const Job& job;
		} owner{ *this, a_job };
		const auto send = [](void* a_owner, std::size_t a_pool, std::size_t a_count, std::uint64_t a_contentEpoch, const void* a_view,
							  Api::FrameValuePoolProducer::EmitRowsFn a_emit) {
			auto& [impl, job] = *static_cast<Owner*>(a_owner);
			impl.SendRows(job.registered[a_pool].lease, impl.registered[a_pool].pool, impl.registered[a_pool].entry->name, job, a_count, a_contentEpoch, a_view, a_emit);
		};
		for (std::size_t i = 0; i < a_job.registered.size(); ++i) {
			const auto& part = a_job.registered[i];
			Api::FrameValuePoolProducer producer;
			producer.seq = a_job.seq;
			producer.parity = a_job.parity;
			producer.globals = a_job.globals.get();
			producer.input = &part.input;
			producer.oldestContentEpoch = part.oldestContentEpoch;
			producer.kickedSeq = &kickedSeq;
			producer.send = send;
			producer.owner = &owner;
			producer.pool = i;
			producer.seeded = &seeded;
			a_call(*registered[i].entry, producer);
		}
	}

	void FrameValues::Impl::Upload(const Job& a_job)
	{
		ZoneScopedN("CS.DCLF.FrameValues.Upload");
		{
			ZoneScopedN("CS.DCLF.FrameValues.Send.placements");
			Send(a_job, kPlacements, rows);
		}
		{
			ZoneScopedN("CS.DCLF.FrameValues.Send.palettes");
			Send(a_job, kPalettes, palettes);
		}
		{
			ZoneScopedN("CS.DCLF.FrameValues.Send.shading");
			Send(a_job, kShading, shading);
		}
		// A registered pool's send: one zone name (Tracy's are static), the pool's name its text.
		ForEachRegistered(a_job, [](const Extensions::FrameValuePoolEntry& a_entry, Api::FrameValuePoolProducer& a_producer) {
			ZoneScopedN("CS.DCLF.FrameValues.Send.Registered");
			ZoneText(a_entry.name.data(), a_entry.name.size());
			a_entry.send(a_producer);
		});
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

	void FrameValues::Impl::FrameStartRegistered(const std::shared_ptr<const FrameGlobals>& a_globals)
	{
		const Api::FrameValuePoolFrameStart start{ a_globals };
		for (const auto& entry : Extensions::RegisteredPools())
			if (entry.frameStart)
				entry.frameStart(start);
	}

	// The kept set's root-view movers, every plan applied in order (the producers apply the same plans to the whole set).
	void FrameValues::Impl::ApplyRootViewPlans(const std::vector<std::shared_ptr<const Plan>>& a_plans)
	{
		for (const auto& applied : a_plans) {
			if (!applied)
				continue;
			auto release = [](Plan::Item& a_item) {
				ReleaseLater(a_item.geometry);
				ReleaseLater(a_item.listedRoot);
			};
			if (applied->reset) {
				for (auto& [geometry, item] : rootViewMovers)
					release(item);
				rootViewMovers.clear();
			}
			for (const void* geometry : applied->moversLeft)
				if (const auto it = rootViewMovers.find(geometry); it != rootViewMovers.end()) {
					release(it->second);
					rootViewMovers.erase(it);
				}
			for (const auto& item : applied->movers) {
				if (!item.geometry)
					continue;
				const auto it = rootViewMovers.find(item.geometry.get());
				if (it != rootViewMovers.end()) {
					release(it->second);
					if (item.firstPerson)
						it->second = item;
					else
						rootViewMovers.erase(it);
				} else if (item.firstPerson) {
					rootViewMovers.emplace(item.geometry.get(), item);
				}
			}
		}
	}

	void FrameValues::Impl::QueuePlans(std::vector<std::shared_ptr<const Plan>>& a_plans)
	{
		for (auto& queued : a_plans)
			if (queued) {
				unsampled.push_back(queued);
				plan = std::move(queued);
				++planSerial;
			}
	}

	void FrameValues::Impl::LeaseBackings(Job& a_job)
	{
		const std::array<std::uint64_t, kBuiltInPools> needed{ plan ? plan->slots : 0u, plan ? 2ull * plan->boneCapacity : 0u, a_job.shadingSlots };
		for (std::size_t part = 0; part < kBuiltInPools; ++part)
			LeasePool(pools[part], kPartNames[part], kStrides[part], needed[part], frameLeases[part], a_job.leases[part], a_job.oldestContentEpochs[part]);
		for (std::size_t i = 0; i < a_job.registered.size(); ++i) {
			auto& entry = registered[i];
			auto& part = a_job.registered[i];
			LeasePool(entry.pool, entry.entry->name, entry.entry->stride, entry.needed, registeredFrameLeases[i], part.lease, part.oldestContentEpoch);
		}
	}

	void FrameValues::Impl::LeasePool(Pool& a_pool, std::string_view a_name, std::uint32_t a_stride, std::uint64_t a_needed, org::services::BackingLease& a_frameLease,
		org::services::BackingLease& a_jobLease, std::optional<std::uint64_t>& a_oldestContentEpoch)
	{
		a_pool.capacity = org::services::GrowthPolicy::Doubling(1024).Next(a_pool.capacity, a_needed);
		const auto name = fmt::format("cs.dclf.frame-values.{}", a_name);
		org::services::BackingPool::AcquireInfo info;
		auto lease = a_pool.pool->Acquire({ .capacityClass = a_pool.capacity, .stride = a_stride, .name = name },
			a_pool.newestContentEpoch.load(std::memory_order_acquire), &info);
		if (!lease)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: the frame values' {} pool returned no backing", a_name));
		++a_pool.acquires;
		if (info.allocated)
			++a_pool.made;
		a_oldestContentEpoch = a_pool.pool->OldestContentEpoch();
		a_pool.floor.store(a_oldestContentEpoch.value_or(0), std::memory_order_relaxed);
		a_frameLease = lease;
		a_jobLease = std::move(lease);
	}

	void FrameValues::Impl::MakeRegistered()
	{
		if (registered)
			return;
		const auto entries = Extensions::RegisteredPools();
		registered = std::make_unique<Registered[]>(entries.size());
		for (std::size_t i = 0; i < entries.size(); ++i) {
			registered[i].entry = &entries[i];
			registered[i].pool.pool = MakePool(entries[i].name);
		}
		registeredFrameLeases.resize(entries.size());
		registeredIndex.assign(entries.size(), 0u);
		registeredMade.store(entries.size(), std::memory_order_release);
	}

	void FrameValues::Impl::KickRegistered(Job& a_job, const FrameGlobals* a_globals)
	{
		MakeRegistered();
		const std::size_t count = registeredMade.load(std::memory_order_relaxed);
		a_job.registered.resize(count);
		const Api::FrameValuePoolKick kick{ a_job.seq, a_globals };
		for (std::size_t i = 0; i < count; ++i) {
			auto input = registered[i].entry->kick(kick);
			a_job.registered[i].input = std::move(input.input);
			registered[i].needed = input.rows;
		}
	}

	void FrameValues::Impl::SampleRootViews(Job& a_job, const SetSnapshot& a_claims)
	{
		std::vector<std::uint8_t> taken;
		// A CategoryRoot view's member (the first person's phase: Extensions::Views, resolved at install).
		const std::uint8_t rootPhases = Extensions::Views().rootPhases;
		auto take = [&](const Plan::Item& a_item) {
			if (!a_item.firstPerson || !a_item.geometry || a_item.slot == kNoObjectSlot || !(a_claims.PhasesOf(a_item.geometry.get()) & rootPhases))
				return;
			if (taken.size() <= a_item.slot)
				taken.resize(a_item.slot + 1, 0);
			if (std::exchange(taken[a_item.slot], 1))
				return;
			auto& sample = a_job.rootViewSamples.emplace_back();
			sample.slot = a_item.slot;
			sample.layerSlot = a_item.layerSlot;
			SampleItem(a_item, sample.row, sample.layer);
			if (!a_item.boneRows)
				return;
			auto& geometry = *a_item.geometry;
			const float* current = nullptr;
			const float* previous = nullptr;
			if (!DCLF::SkinPalettes::Sample(geometry, a_item.boneRows, current, previous))
				return;
			sample.boneOffset = a_item.boneOffset;
			sample.boneRows = a_item.boneRows;
			const auto* currentRows = reinterpret_cast<const PaletteRow*>(current);
			const auto* previousRows = reinterpret_cast<const PaletteRow*>(previous);
			sample.current.assign(currentRows, currentRows + a_item.boneRows);
			sample.previous.assign(previousRows, previousRows + a_item.boneRows);
		};
		for (const auto& [geometry, item] : rootViewMovers)
			take(item);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				take(item);
	}

	bool FrameValues::Kick(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
		std::vector<ShadingFieldValue> a_fieldValues, std::shared_ptr<const FrameGlobals> a_globals)
	{
		auto& s = *impl;
		s.kicked = false;
		// A frame whose EndFrame did not come: its backings are bound to what was submitted so far.
		BindFrameLeases(s.frameLeases, s.registeredFrameLeases);
		// The registered pools' frame-start reads (Api::FrameValuePool::frameStart), whether or not the frame has a producer: Features/Fades'
		// seeds' the last.
		Impl::FrameStartRegistered(a_globals);
		// The refused items first (the oldest), then those not yet taken, then the frame's (newest last).
		if (auto refused = s.TakeRefusedShading(); !refused.empty())
			s.unsampledShading.insert(s.unsampledShading.begin(), std::make_move_iterator(refused.begin()), std::make_move_iterator(refused.end()));
		s.unsampledShading.insert(s.unsampledShading.end(), std::make_move_iterator(a_shading.begin()), std::make_move_iterator(a_shading.end()));
		s.unsampledFieldValues.insert(s.unsampledFieldValues.end(), a_fieldValues.begin(), a_fieldValues.end());
		s.ApplyRootViewPlans(a_plans);
		auto* host = RenderGraphRuntime::Get().Host();
		auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		auto uploads = host ? host->RetainUploads() : nullptr;
		if (!host || !timeline || !uploads || !uploads->HasDedicatedStreamingQueue()) {
			// Plans wait for the first producer; startup frames precede the upload queue (made by the graph's first build).
			s.QueuePlans(a_plans);
			if (host)
				host->SetFrameWaitValue(0);
			// No backing is leased for the frame: its draws have no rows (no index names a backing the pool may release).
			frameIndex = paletteIndex = shadingIndex = 0;
			std::fill(s.registeredIndex.begin(), s.registeredIndex.end(), 0u);
			if (!s.unavailableLogged && s.seq) {
				s.unavailableLogged = true;
				logger::error("[DCLF] frame values unavailable ({}): DCLF's draws have no placements", !host ? "no render graph" : "no dedicated upload queue");
			}
			return false;
		}
		// Every plan since the last frame, in order: the producer applies each onto its kept set and samples its written slots.
		s.QueuePlans(a_plans);
		// Release finished producers' plans here on the render thread (they hold engine references).
		const auto finished = s.done.load(std::memory_order_acquire);
		while (!s.plans.empty() && s.plans.front().first <= finished)
			s.plans.pop_front();
		static const Plan kEmpty{};
		const std::uint64_t seq = ++s.seq;
		s.kickedSeq.store(seq, std::memory_order_release);
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
		job.fieldValues = std::exchange(s.unsampledFieldValues, {});
		job.shadingSlots = job.plan->slots;
		for (const auto& item : job.shadingItems)
			if (item.slot != kNoObjectSlot)
				job.shadingSlots = std::max<std::size_t>(job.shadingSlots, std::size_t(item.slot) + 1);
		for (const auto& value : job.fieldValues)
			if (value.slot != kNoObjectSlot)
				job.shadingSlots = std::max<std::size_t>(job.shadingSlots, std::size_t(value.slot) + 1);
		job.globals = a_globals;
		// The registered pools' inputs for the frame's producer, and the rows each asks for.
		s.KickRegistered(job, a_globals.get());
		// Each part's backing, holding every row the producer leaves (a class of GrowthPolicy::Doubling(1024)), the one that holds
		// most of the journal preferred; the floor (the oldest content in circulation, this lease's included) is what the
		// producer's journal keeps.
		s.LeaseBackings(job);
		job.timeline = std::move(timeline);
		job.uploads = std::move(uploads);
		// The palette rows classes' frame inputs (Features/Particles: the cameras its quads face, the main one and the first person's).
		for (std::size_t c = 0; c < job.paletteInputs.size(); ++c)
			if (const auto* entry = Extensions::PaletteRows(static_cast<Api::FrameValueItemClass>(c + 1)); entry && entry->contribution.kick)
				entry->contribution.kick({ seq, job.paletteInputs[c] });
		// Claimed first-person members, sampled after the engine's update and before Main::Draw's first-person shift
		// (FUN_1414cb020), palettes included: the engine draws none of them, so none of its reads see a world-space palette.
		if (const auto claims = std::exchange(rootViewClaims, nullptr))
			s.SampleRootViews(job, *claims);
		if (s.plan)
			s.plans.emplace_back(seq, s.plan);
		frameIndex = s.frameLeases[Impl::kPlacements]->srvIndex;
		paletteIndex = s.frameLeases[Impl::kPalettes]->srvIndex;
		shadingIndex = s.frameLeases[Impl::kShading]->srvIndex;
		for (std::size_t i = 0; i < s.registeredIndex.size(); ++i)
			s.registeredIndex[i] = s.registeredFrameLeases[i]->srvIndex;
		// Batches submitted from here wait for the frame's values; the producer always signals.
		host->SetFrameWaitValue(seq);
		s.kicked = true;
		if (!SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
				"frame values", [&s, job = std::move(job)](const auto&) mutable {
					s.Run(job);
					// The items a lease refusal left unsampled, to the next producer (their engine references with them).
					if (!job.refusedShading.empty()) {
						std::vector<ShadingItem> refused;
						refused.reserve(job.refusedShading.size());
						for (const auto i : job.refusedShading)
							refused.push_back(std::move(job.shadingItems[i]));
						s.PushRefusedShading(std::move(refused));
					}
					// The producer's leases go; the frame's (frameLeases) keep the backings until EndFrame binds them.
					job.leases = {};
					for (auto& part : job.registered)
						part.lease = org::services::BackingLease{};
					// Engine references go to the render thread (EngineReleases, at Present), never dropped on this worker.
					for (auto& item : job.shadingItems)
						if (item.property)  // a refused item's went back with it
							EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.property)));
				}))
			stl::report_and_fail("Drawcall Limit Fix: the frame values' producer was refused by DCLF's executor");
		return true;
	}

	void FrameValues::Skip()
	{
		impl->kicked = false;
		BindFrameLeases(impl->frameLeases, impl->registeredFrameLeases);
		if (auto* host = RenderGraphRuntime::Get().Host())
			host->SetFrameWaitValue(0);
	}

	void FrameValues::EndFrame()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		s.kicked = false;
		BindFrameLeases(s.frameLeases, s.registeredFrameLeases);
	}

	bool FrameValues::ProducerDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return false;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		return s.done.load(std::memory_order_acquire) >= s.seq;
	}

	void FrameValues::WaitSampled() const
	{
		auto& s = *impl;
		const auto kicked = s.kickedSeq.load(std::memory_order_acquire);
		for (auto sampled = s.sampledSeq.load(std::memory_order_acquire); sampled < kicked; sampled = s.sampledSeq.load(std::memory_order_acquire))
			s.sampledSeq.wait(sampled, std::memory_order_acquire);
	}

	void FrameValues::NoteRefused()
	{
		++impl->refused;
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
		c.plans = s.plansApplied.exchange(0);
		c.planResets = s.planResets.exchange(0);
		c.planItems = s.planItems.exchange(0);
		c.movers = s.movers.exchange(0);
		c.moversDue = s.moversDue.exchange(0);
		c.roots = s.roots.exchange(0);
		c.rootsDue = s.rootsDue.exchange(0);
		c.moveKeys = s.moveKeys.exchange(0);
		c.moveNodes = s.moveNodes.exchange(0);
		return c;
	}

	std::uint32_t FrameValues::PoolIndex(Api::FrameValuePoolId a_pool) const
	{
		const auto& indices = impl->registeredIndex;
		return a_pool.index < indices.size() ? indices[a_pool.index] : 0u;
	}

	std::vector<FrameValues::PoolStats> FrameValues::TakePoolStats()
	{
		auto& s = *impl;
		const std::size_t registered = s.registeredMade.load(std::memory_order_acquire);
		std::vector<PoolStats> out(kBuiltInPools + registered);
		for (std::size_t part = 0; part < out.size(); ++part) {
			auto& pool = part < kBuiltInPools ? s.pools[part] : s.registered[part - kBuiltInPools].pool;
			auto& stats = out[part];
			stats.name = part < kBuiltInPools ? Impl::kPartNames[part] : s.registered[part - kBuiltInPools].entry->name.c_str();
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
}
