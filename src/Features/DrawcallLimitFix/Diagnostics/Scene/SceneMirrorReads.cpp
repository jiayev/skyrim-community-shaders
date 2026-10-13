// CS_DCLF_MIRROR_PARITY, the reads: each of the lane's reads ported to the mirror checked against the live read (the object alive: the lane
// holds it), under a live check's lease; a difference is checked again after the next batch (Coordinator's DeferredRead: lagged, or it
// differs). The passes run their evaluations on their own thread while this observes (Policy::SerialFanout).

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;
		using MirrorRead = V::MirrorRead;

		void OnCategoryNode(const CategoryNodeRead& a_event)
		{
			const auto& store = a_event.store;
			const auto lease = V::LiveCheckLease(store, MirrorRead::CategoryNode);
			if (!lease)
				return;
			auto* a_object = a_event.object;
			const auto* found = a_event.found;
			const Ineligible reason = a_event.reason;
			const bool unmirrored = a_event.unmirrored;
			Ineligible liveReason = Ineligible::None;
			const auto* live = V::FindCategoryNodeLive(store, a_object, &liveReason);
			V::NoteMirrorRead(store, MirrorRead::CategoryNode, live != found || (found && liveReason != reason), [&] {
				// Both chains, to where they part (the live one named).
				std::string chains;
				const auto mirrored = V::MirrorChain(store, a_object, nullptr);
				std::size_t at = 0;
				for (const RE::NiAVObject* object = a_object; object && at < 16; object = object->parent, ++at) {
					const void* other = at < mirrored.size() ? mirrored[at] : nullptr;
					chains += fmt::format(" {}'{}'{}", at ? "<- " : "", object->name.c_str() ? object->name.c_str() : "", other == object ? "" : fmt::format(" (mirror {})", other));
				}
				return fmt::format("'{}' {}: mirror {} (reason {}), live {} (reason {}){}; live chain:{}; mirror chain {} long", a_object->name.c_str() ? a_object->name.c_str() : "",
					static_cast<const void*>(a_object), static_cast<const void*>(found), static_cast<int>(reason), static_cast<const void*>(live), static_cast<int>(liveReason),
					unmirrored ? ", a record missing on the way" : "", chains, mirrored.size());
			}, [&store, key = static_cast<const void*>(a_object), live, liveReason] {
				Ineligible r = Ineligible::None;
				bool u = false;
				const auto* now = V::MirrorCategoryNode(store, key, r, u);
				return now == live && (!now || r == liveReason);
			});
		}

		void OnSunEntry(const SunEntryRead& a_event)
		{
			// The mirror's entry against the live chain's: a live walk's path gives the live one, the lane's is checked under a lease.
			const auto& store = a_event.store;
			const auto& a_geometry = a_event.geometry;
			const void* entry = a_event.entry;
			const RE::NiAVObject* live = a_event.live;
			if (!a_event.liveKnown) {
				const auto lease = V::LiveCheckLease(store, MirrorRead::SunEntry);
				if (!lease)
					return;
				live = V::LiveSunEntry(a_geometry);
			}
			V::NoteMirrorRead(store, MirrorRead::SunEntry, live != entry, [&] { return fmt::format("'{}' {}: mirror {}, live {}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "",
				static_cast<const void*>(&a_geometry), entry, static_cast<const void*>(live)); },
				[&store, key = static_cast<const void*>(&a_geometry), live] { return V::MirrorSunEntry(store, key) == live; });
		}

		void OnLightEntry(const LightEntryRead& a_event)
		{
			const auto& store = a_event.store;
			const auto& a_geometry = a_event.geometry;
			const auto* category = a_event.category;
			const void* root = a_event.root;
			const bool actorRoot = a_event.actorRoot;
			const RE::NiAVObject* live = a_event.live;
			bool liveActor = a_event.liveActor;
			if (!a_event.liveKnown) {
				const auto lease = V::LiveCheckLease(store, MirrorRead::LightEntry);
				if (!lease)
					return;
				live = V::LiveLightRoot(a_geometry, category, liveActor);
			}
			V::NoteMirrorRead(store, MirrorRead::LightEntry, liveActor != actorRoot || (actorRoot && live != root), [&] {
				return fmt::format("'{}' {}: mirror {} (actor {}), live {} (actor {})", a_geometry.name.c_str() ? a_geometry.name.c_str() : "", static_cast<const void*>(&a_geometry), root,
					actorRoot, static_cast<const void*>(live), liveActor);
			}, [&store, key = static_cast<const void*>(&a_geometry), category, live = static_cast<const void*>(live), liveActor] {
				const auto chain = V::MirrorChain(store, key, category);
				const auto* record = chain.size() > 1 ? V::mirror(store).Node(chain.back()) : nullptr;
				const bool actor = record && record->parent == category && record->IsActorCharacter();
				return actor == liveActor && (!actor || chain.back() == live);
			});
		}

		void OnSubtree(const SubtreeRead& a_event)
		{
			const auto& store = a_event.store;
			const auto lease = V::LiveCheckLease(store, MirrorRead::Subtree);
			if (!lease)
				return;
			auto* a_root = a_event.root;
			const Ineligible reasonAbove = a_event.reasonAbove;
			const std::vector<std::pair<RE::BSGeometry*, Ineligible>> found(a_event.found.begin(), a_event.found.end());
			std::vector<std::pair<RE::BSGeometry*, Ineligible>> live;
			std::vector<std::pair<RE::NiAVObject*, Ineligible>> stack;
			stack.emplace_back(a_root, reasonAbove);
			while (!stack.empty()) {
				auto [object, reason] = stack.back();
				stack.pop_back();
				if (!object)
					continue;
				if (auto* geometry = object->AsGeometry()) {
					live.emplace_back(geometry, reason);
					continue;
				}
				if (auto* node = object->AsNode()) {
					const Ineligible below = Eligibility::CombineParentReasons(reason, Eligibility::ParentReason(node));
					for (auto& child : node->GetChildren())
						if (child)
							stack.emplace_back(child.get(), below);
				}
			}
			V::NoteMirrorRead(store, MirrorRead::Subtree, live != found, [&] {
				return fmt::format("'{}' {}: mirror {} geometries, live {}{}", a_root->name.c_str() ? a_root->name.c_str() : "", static_cast<const void*>(a_root), found.size(),
					live.size(), a_event.lacking ? ", a record missing on the way" : "");
			}, [&store, key = static_cast<const void*>(a_root), reasonAbove, live] {
				std::vector<std::pair<RE::BSGeometry*, Ineligible>> now;
				bool missing = false;
				V::MirrorSubtree(store, key, reasonAbove, now, missing);
				return now == live;
			});
		}

		/** @brief The tracked geometries below a_root, live and by the mirror (each walk bounded as an event's), sorted. */
		template <class Tracked>
		std::pair<std::vector<const void*>, std::vector<const void*>> TrackedBelow(const SceneMirror& a_mirror, RE::NiAVObject* a_root, const Tracked& a_tracked)
		{
			std::pair<std::vector<const void*>, std::vector<const void*>> out;
			Engine::ForEachLive(a_root, [&](RE::NiAVObject& a_object, std::uint32_t) {
				if (auto* geometry = a_object.AsGeometry(); geometry && a_tracked.contains(geometry))
					out.first.push_back(geometry);
			}, kMaxEventNodes);
			ForEachMirror(a_mirror, a_root, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
				if ((a_record.kind & SceneCapture::kKindGeometry) && a_tracked.contains(static_cast<RE::BSGeometry*>(const_cast<void*>(a_key))))
					out.second.push_back(a_key);
			}, kMaxEventNodes);
			std::ranges::sort(out.first);
			std::ranges::sort(out.second);
			return out;
		}

		void OnTrackedBelow(const TrackedBelowRead& a_event)
		{
			// A walk below a root: the tracked geometries the mirror gives against the live ones.
			const auto& store = a_event.store;
			const auto lease = V::LiveCheckLease(store, a_event.read);
			if (!lease)
				return;
			auto* a_root = a_event.root;
			const auto& mirror = V::mirror(store);
			const auto [live, mirrored] = TrackedBelow(mirror, a_root, V::tracked(store));
			V::NoteMirrorRead(store, a_event.read, live != mirrored, [&] {
				return fmt::format("'{}' {}: {} tracked geometries below by the mirror, {} live", a_root->name.c_str() ? a_root->name.c_str() : "", static_cast<const void*>(a_root),
					mirrored.size(), live.size());
			}, [&mirror, key = static_cast<const void*>(a_root), live] {
				// The mirror's walk again, the live list as tracked then (what was tracked since is not the read's).
				std::vector<const void*> now;
				ForEachMirror(mirror, key, [&](const void* a_key, const SceneCapture::NodeRecord& a_record) {
					if ((a_record.kind & SceneCapture::kKindGeometry) && std::ranges::binary_search(live, a_key))
						now.push_back(a_key);
				}, kMaxEventNodes);
				std::ranges::sort(now);
				return now == live;
			});
		}

		void OnHiddenChain(const HiddenChainRead& a_event)
		{
			const auto& store = a_event.store;
			const auto lease = V::LiveCheckLease(store, MirrorRead::HiddenChain);
			if (!lease)
				return;
			const auto* a_geometry = a_event.geometry;
			const auto& chain = a_event.chain;
			std::vector<const void*> live;
			for (const RE::NiAVObject* object = a_geometry; object && object != a_event.category; object = object->parent)
				live.push_back(object);
			const bool differ = !std::ranges::equal(live, chain);
			V::NoteMirrorRead(store, MirrorRead::HiddenChain, differ, [&] { return fmt::format("'{}' {}: {} objects by the mirror, {} live", a_geometry->name.c_str() ? a_geometry->name.c_str() : "",
				static_cast<const void*>(a_geometry), chain.size(), live.size()); },
				[&store, key = static_cast<const void*>(a_geometry), category = static_cast<const void*>(a_event.category), live] { return V::MirrorChain(store, key, category) == live; });
		}

		void OnMoveKey(const MoveKeyRead& a_event)
		{
			const auto& store = a_event.store;
			const auto lease = V::LiveCheckLease(store, MirrorRead::MoveKey);
			if (!lease)
				return;
			const auto& a_geometry = a_event.geometry;
			const auto* a_categoryNode = a_event.category;
			const void* key = a_event.key;
			// The one reference on the live chain up to the category node (none: none, or more than one): MoveKeyOf's rule.
			const void* live = nullptr;
			for (const RE::NiAVObject* object = &a_geometry; object && object != a_categoryNode; object = object->parent) {
				const void* reference = object->GetUserData();
				if (!reference)
					continue;
				if (live && live != reference) {
					live = nullptr;
					break;
				}
				live = reference;
			}
			V::NoteMirrorRead(store, MirrorRead::MoveKey, live != key, [&] { return fmt::format("'{}' {}: mirror {}, live {}", a_geometry.name.c_str() ? a_geometry.name.c_str() : "",
				static_cast<const void*>(&a_geometry), key, live); }, [&store, geometry = static_cast<const void*>(&a_geometry), a_categoryNode, live] {
				const void* now = nullptr;
				for (const void* at : V::MirrorChain(store, geometry, a_categoryNode)) {
					const auto* record = V::mirror(store).Node(at);
					const void* reference = record ? record->userData : nullptr;
					if (!reference)
						continue;
					if (now && now != reference)
						return live == nullptr;
					now = reference;
				}
				return now == live;
			});
		}
	}

	void InstallMirrorReads()
	{
		Subscribe<CategoryNodeRead>(OnCategoryNode);
		Subscribe<SunEntryRead>(OnSunEntry);
		Subscribe<LightEntryRead>(OnLightEntry);
		Subscribe<SubtreeRead>(OnSubtree);
		Subscribe<TrackedBelowRead>(OnTrackedBelow);
		Subscribe<HiddenChainRead>(OnHiddenChain);
		Subscribe<MoveKeyRead>(OnMoveKey);
	}
}
