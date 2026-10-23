// SceneMirror's record maintenance: captures applied newest-wins with the later updates replayed over them, detaches, evictions
// and hook updates. Scene-work thread (the coordinator's event application).

#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneMirror.h"

#include <Tracy/Tracy.hpp>
#include <algorithm>
#include <bit>
#include <cassert>
#include <fmt/ranges.h>

#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/MirrorEvents.h"

namespace DCLF
{
	namespace
	{
		using namespace SceneCapture;

	}

	void SceneMirror::Use(const void* a_property, const void* a_layer, const void* a_alpha, int a_delta)
	{
		auto adjust = [a_delta](auto& a_map, const void* a_key) {
			if (!a_key)
				return;
			const auto it = a_map.find(a_key);
			if (it == a_map.end())
				return;
			it->second.uses = static_cast<std::uint32_t>(static_cast<int>(it->second.uses) + a_delta);
			if (!it->second.uses)
				a_map.erase(it);
		};
		adjust(properties, a_property);
		adjust(properties, a_layer);
		adjust(alphas, a_alpha);
	}

	void SceneMirror::Prepare(const Records& a_records, std::uint32_t a_kind, std::uint32_t a_frame, Prepared& a_out)
	{
		// What ApplyCapture's take stamps, done here; it only moves the records in (a superseded one is dropped there).
		auto copy = [&a_records, a_frame](const auto& a_from, auto& a_into) {
			a_into.assign(a_from.begin(), a_from.end());
			for (auto& record : a_into) {
				record.sequence = a_records.sequence;
				if constexpr (requires { record.writer; }) {
					record.writer = 1;
					record.writtenFrame = a_frame;
				}
			}
		};
		switch (a_kind) {
		case 0:
			copy(a_records.nodes, a_out.nodes);
			break;
		case 1:
			copy(a_records.geometries, a_out.geometries);
			break;
		case 2:
			copy(a_records.properties, a_out.properties);
			break;
		default:
			copy(a_records.alphas, a_out.alphas);
			break;
		}
	}

	std::size_t SceneMirror::UnusedRecords() const
	{
		std::size_t unused = 0;
		for (const auto& [key, entry] : properties)
			unused += entry.uses == 0 ? 1 : 0;
		for (const auto& [key, entry] : alphas)
			unused += entry.uses == 0 ? 1 : 0;
		return unused;
	}

	void SceneMirror::Apply(const Records& a_records, Prepared* a_prepared)
	{
		++writes;
		ApplyCapture(a_records, a_prepared);
		if (replaying || !a_records.sequence)
			return;
		// Numbered at or after every kept update: none is newer than what it read.
		if (a_records.sequence >= recentNewest) {
			++work.replaysSkipped;
			if (checking) {
				// Parity: the walk the skip saves finds nothing.
				++work.skipsChecked;
				std::vector<const SceneCapture::Update*> later;
				auto walk = [&](std::uintptr_t a_key) { Later(a_key, a_records.sequence, later); };
				for (const auto& n : a_records.nodes)
					walk(Key(n.key, 0));
				for (const auto& g : a_records.geometries)
					walk(Key(g.key, 1));
				for (const auto& p : a_records.properties)
					walk(Key(p.key, 2));
				for (const auto& a : a_records.alphas)
					walk(Key(a.key, 3));
				work.skipsWrong += later.empty() ? 0 : 1;
			}
			return;
		}
		++work.replaysWalked;
		ZoneScopedN("CS.DCLF.Scene.Mirror.Replay");
		// The updates numbered after this capture's start: their writes may be newer than what it read.
		replaying = true;
		for (const auto& n : a_records.nodes)
			Replay(Key(n.key, 0), a_records.sequence);
		for (const auto& g : a_records.geometries)
			Replay(Key(g.key, 1), a_records.sequence);
		for (const auto& p : a_records.properties)
			Replay(Key(p.key, 2), a_records.sequence);
		for (const auto& a : a_records.alphas)
			Replay(Key(a.key, 3), a_records.sequence);
		replaying = false;
	}

	namespace
	{
		/** @brief ApplyCapture's parity: every prepared record is its capture's, stamped with the capture's sequence. */
		void CheckPrepared(const Records& a_records, const SceneMirror::Prepared& a_prepared, std::uint64_t& a_checked, std::uint64_t& a_wrong)
		{
			const std::uint64_t sequence = a_records.sequence;
			++a_checked;
			bool wrong = a_prepared.nodes.size() != a_records.nodes.size() || a_prepared.geometries.size() != a_records.geometries.size() ||
			             a_prepared.properties.size() != a_records.properties.size() || a_prepared.alphas.size() != a_records.alphas.size();
			auto compare = [&](const auto& a_ready, const auto& a_captured) {
				for (std::size_t i = 0; !wrong && i < a_ready.size(); ++i)
					wrong = a_ready[i].key != a_captured[i].key || a_ready[i].sequence != sequence || a_ready[i].Differ(a_captured[i]) != 0;
			};
			if (!wrong) {
				compare(a_prepared.nodes, a_records.nodes);
				compare(a_prepared.geometries, a_records.geometries);
				compare(a_prepared.properties, a_records.properties);
				compare(a_prepared.alphas, a_records.alphas);
			}
			a_wrong += wrong ? 1 : 0;
		}
	}

	void SceneMirror::ApplyCapture(const Records& a_records, Prepared* a_prepared)
	{
		ZoneScopedN("CS.DCLF.Scene.Mirror.Capture");
		++applied;
		// A record a newer capture took stays (two loader threads capture subtrees naming one shared property or ancestor; the
		// older capture can be pushed last). A prepared record is stamped already, and moved in.
		const std::uint64_t sequence = a_records.sequence;
		auto take = [&](auto& a_record, const auto& a_captured, auto* a_ready) {
			if (a_record.key && a_record.sequence > sequence) {
				++superseded;
				return;
			}
			if (a_ready) {
				a_record = std::move(*a_ready);
				return;
			}
			a_record = a_captured;
			a_record.sequence = sequence;
			if constexpr (requires { a_record.writer; }) {
				a_record.writer = 1;
				a_record.writtenFrame = SceneCapture::Frame();
			}
		};
		assert(!a_prepared || (a_prepared->nodes.size() == a_records.nodes.size() && a_prepared->geometries.size() == a_records.geometries.size() &&
								  a_prepared->properties.size() == a_records.properties.size() && a_prepared->alphas.size() == a_records.alphas.size()));
		if (a_prepared && checking)
			CheckPrepared(a_records, *a_prepared, work.preparedChecked, work.preparedWrong);
		// Properties and alphas first (their values), then the geometries that count their uses.
		for (std::size_t i = 0; i < a_records.properties.size(); ++i)
			take(properties[a_records.properties[i].key].record, a_records.properties[i], a_prepared ? &a_prepared->properties[i] : nullptr);
		for (std::size_t i = 0; i < a_records.alphas.size(); ++i)
			take(alphas[a_records.alphas[i].key].record, a_records.alphas[i], a_prepared ? &a_prepared->alphas[i] : nullptr);
		for (std::size_t i = 0; i < a_records.geometries.size(); ++i) {
			const auto& g = a_records.geometries[i];
			if (const auto it = geometries.find(g.key); it != geometries.end() && it->second.sequence > sequence) {
				++superseded;
				continue;
			}
			// The new record's uses first, then the old one's let go: a property both name keeps its record.
			for (const void* key : { g.property, g.layerProperty })
				if (key)
					++properties[key].uses;
			if (g.alpha)
				++alphas[g.alpha].uses;
			auto [it, inserted] = geometries.try_emplace(g.key);
			if (!inserted) {
				Use(it->second.property, it->second.layerProperty, it->second.alpha, -1);
				if (it->second.skin && it->second.skin != g.skin)
					geometryBySkin.erase(it->second.skin);
			}
			if (a_prepared)
				it->second = std::move(a_prepared->geometries[i]);
			else
				it->second = g;
			it->second.sequence = a_records.sequence;
			if (g.skin)
				geometryBySkin[g.skin] = g.key;
		}
		// A property or alpha the capture took that no geometry counts (none can be: each comes with its geometry) is not kept. Only
		// this capture's keys can be uncounted (every other record is a geometry's, and Use drops one at its last use): by key, not a
		// walk over the maps (UnusedRecords' parity).
		for (const auto& p : a_records.properties)
			if (const auto it = properties.find(p.key); it != properties.end() && !it->second.uses)
				properties.erase(it);
		for (const auto& a : a_records.alphas)
			if (const auto it = alphas.find(a.key); it != alphas.end() && !it->second.uses)
				alphas.erase(it);
		for (std::size_t i = 0; i < a_records.nodes.size(); ++i)
			take(nodes[a_records.nodes[i].key], a_records.nodes[i], a_prepared ? &a_prepared->nodes[i] : nullptr);
		Diagnostics::Notify(Diagnostics::MirrorCaptureApplied{ a_records });
	}

	void SceneMirror::Detach(const void* a_root, std::span<RE::BSGeometry* const> a_geometries, std::span<const void* const> a_nodes)
	{
		++writes;
		++detached;
		// The root leaves its parent's children (the engine's detach, which the hook ran before): its slot null, the trailing nulls
		// trimmed (the capture's).
		if (const auto root = nodes.find(a_root); root != nodes.end())
			if (const auto parent = nodes.find(root->second.parent); parent != nodes.end()) {
				auto& children = parent->second.children;
				std::replace(children.begin(), children.end(), a_root, static_cast<const void*>(nullptr));
				while (!children.empty() && !children.back())
					children.pop_back();
			}
		for (const auto* geometry : a_geometries) {
			if (const auto it = geometries.find(geometry); it != geometries.end()) {
				Use(it->second.property, it->second.layerProperty, it->second.alpha, -1);
				if (it->second.skin)
					geometryBySkin.erase(it->second.skin);
				geometries.erase(it);
			}
			nodes.erase(geometry);
		}
		for (const void* node : a_nodes)
			nodes.erase(node);
		Diagnostics::Notify(Diagnostics::MirrorDetached{ a_geometries, a_nodes });
	}

	void SceneMirror::Evict(const void* a_key, std::string_view a_what)
	{
		++writes;
		const auto* root = Node(a_key);
		if (!root)
			return;
		std::vector<RE::BSGeometry*> geometriesBelow;
		std::vector<const void*> nodesBelow;
		std::vector<const void*> stack{ a_key };
		while (!stack.empty()) {
			const void* key = stack.back();
			stack.pop_back();
			const auto* record = Node(key);
			if (!record)
				continue;
			if (record->kind & SceneCapture::kKindGeometry)
				geometriesBelow.push_back(static_cast<RE::BSGeometry*>(const_cast<void*>(key)));
			else
				nodesBelow.push_back(key);
			for (const void* child : record->children)
				if (child)
					stack.push_back(child);
		}
		// The RTTI is static data: its name is safe to read; the record's own name may not be.
		const auto* rtti = static_cast<const RE::NiRTTI*>(root->rtti);
		// A light (NiPointLight, NiSpotLight, NiAmbientLight, NiDirectionalLight) leaves by ShadowSceneNode's queued removal, which
		// detaches it by no hooked function, and a camera likewise; DCLF draws neither, so they are counted apart.
		if (rtti && rtti->name && std::string_view(rtti->name).starts_with("Ni") &&
			(std::string_view(rtti->name).ends_with("Light") || std::string_view(rtti->name) == "NiCamera")) {
			++lightEvictions;
			Detach(a_key, geometriesBelow, nodesBelow);
			--detached;
			return;
		}
		++evictions;
		evictedRecords += geometriesBelow.size() + nodesBelow.size();
		Diagnostics::Notify(Diagnostics::MirrorEvicted{ a_key, rtti ? rtti->name : nullptr, geometriesBelow.size() + nodesBelow.size(), a_what });
		if (firstEviction.empty()) {
			std::size_t depth = 0;
			for (const void* at = root->parent; at && depth < 64; at = Node(at) ? Node(at)->parent : nullptr)
				++depth;
			firstEviction = fmt::format("{} held as a {} {} deep (captured frame {}, {} records under it), now {}", a_key, rtti && rtti->name ? rtti->name : "?", depth,
				root->capturedFrame, geometriesBelow.size() + nodesBelow.size(), a_what);
		}
		Detach(a_key, geometriesBelow, nodesBelow);
		--detached;
	}

	std::pair<std::uint8_t, const void*> SceneMirror::TypeOf(const SceneCapture::Update& a_update)
	{
		return std::visit(
			[&](const auto& a_record) -> std::pair<std::uint8_t, const void*> {
				using T = std::decay_t<decltype(a_record)>;
				return { static_cast<std::uint8_t>(std::is_same_v<T, NodeRecord> ? 0 : std::is_same_v<T, GeometryRecord> ? 1 : std::is_same_v<T, PropertyRecord> ? 2 : 3),
					a_record.key };
			},
			a_update.record);
	}

	void SceneMirror::BeginBatch()
	{
		++batch;
		// The log is in push order, so batch order: the batches older than two leave from its front (O(those), not a walk over the kept).
		while (!recent.empty() && recent.front().batch + 2 < batch) {
			if (const auto it = recentLast.find(recent.front().key); it != recentLast.end() && it->second == recentBase)
				recentLast.erase(it);
			recent.pop_front();
			++recentBase;
			++work.forgotten;
		}
		if (recent.empty())
			recentNewest = 0;
		if (checking) {
			// Parity: the chains from recentLast cover the log exactly, each entry once under its own key, and no kept update is newer
			// than recentNewest.
			++work.logsChecked;
			std::size_t chained = 0;
			bool wrong = false;
			for (const auto& [key, last] : recentLast) {
				wrong |= last < recentBase;
				for (std::uint64_t at = last; at != ~0ull && at >= recentBase;) {
					if (at - recentBase >= recent.size() || recent[static_cast<std::size_t>(at - recentBase)].key != key) {
						wrong = true;
						break;
					}
					++chained;
					at = recent[static_cast<std::size_t>(at - recentBase)].previous;
				}
			}
			for (const auto& r : recent)
				wrong |= r.update.sequence > recentNewest || r.batch + 2 < batch;
			work.logsWrong += wrong || chained != recent.size() ? 1 : 0;
		}
	}

	void SceneMirror::Remember(std::uintptr_t a_key, SceneCapture::Update&& a_update)
	{
		const std::uint64_t index = recentBase + recent.size();
		const auto [it, inserted] = recentLast.try_emplace(a_key, index);
		const std::uint64_t previous = inserted ? ~0ull : std::exchange(it->second, index);
		recentNewest = std::max(recentNewest, a_update.sequence);
		++work.remembered;
		recent.push_back(Recent{ batch, a_key, previous, std::move(a_update) });
	}

	std::pair<std::uint8_t, const void*> SceneMirror::Update(const SceneCapture::Update& a_update)
	{
		SceneCapture::Update copy = a_update;
		return UpdateTaking(copy);
	}

	std::pair<std::uint8_t, const void*> SceneMirror::UpdateTaking(SceneCapture::Update& a_update)
	{
		++writes;
		++updates;
		const auto result = UpdateRecord(a_update);
		// Kept by the key it names (a geometry named by its skin, by its key once resolved): the record moved, the rest copied (the
		// caller's event still reads them).
		const auto [type, key] = TypeOf(a_update);
		if (const void* object = result.second ? result.second : key) {
			SceneCapture::Update kept;
			kept.fields = a_update.fields;
			kept.record = std::move(a_update.record);
			kept.leaf = a_update.leaf;
			kept.sequence = a_update.sequence;
			kept.hiddenSite = a_update.hiddenSite;
			Remember(Key(object, type), std::move(kept));
		}
		return result;
	}

	void SceneMirror::Later(std::uintptr_t a_key, std::uint64_t a_after, std::vector<const SceneCapture::Update*>& a_out) const
	{
		const auto it = recentLast.find(a_key);
		if (it == recentLast.end())
			return;
		// The key's chain, newest first, back to the log's front.
		for (std::uint64_t at = it->second; at != ~0ull && at >= recentBase;) {
			const auto& r = recent[static_cast<std::size_t>(at - recentBase)];
			if (r.update.sequence > a_after)
				a_out.push_back(&r.update);
			at = r.previous;
		}
	}

	void SceneMirror::Replay(std::uintptr_t a_key, std::uint64_t a_after)
	{
		// Then in their order (a later update may have been pushed first).
		std::vector<const SceneCapture::Update*> later;
		Later(a_key, a_after, later);
		if (later.empty())
			return;
		std::ranges::reverse(later);
		std::ranges::stable_sort(later, {}, [](const auto* a_update) { return a_update->sequence; });
		for (const auto* update : later) {
			++replayed;
			UpdateRecord(*update);
		}
	}

	std::pair<std::uint8_t, const void*> SceneMirror::UpdateRecord(const SceneCapture::Update& a_update)
	{
		auto [type, key] = TypeOf(a_update);
		bool held = false;
		std::visit(
			[&](const auto& a_record) {
				using T = std::decay_t<decltype(a_record)>;
				auto assign = [&](auto& a_map, auto a_get) {
					if (const auto it = a_map.find(key); it != a_map.end()) {
						auto& record = a_get(it->second);
						held = true;
						// Written before the record's capture started: the capture has it, and may have newer values.
						if (a_update.sequence && a_update.sequence < record.sequence) {
							++stale;
							return;
						}
						// A leaf is applied whole below (Apply counts the old record's uses off before it replaces it).
						if (!a_update.leaf)
							record.Assign(a_record, a_update.fields);
					}
				};
				if constexpr (std::is_same_v<T, NodeRecord>) {
					assign(nodes, [](auto& a_value) -> NodeRecord& { return a_value; });
				} else if constexpr (std::is_same_v<T, GeometryRecord>) {
					if (!key)
						if (const auto it = geometryBySkin.find(a_record.skin); it != geometryBySkin.end())
							key = it->second;
					assign(geometries, [](auto& a_value) -> GeometryRecord& { return a_value; });
				} else if constexpr (std::is_same_v<T, PropertyRecord>) {
					assign(properties, [](auto& a_value) -> PropertyRecord& { return a_value.record; });
				} else {
					assign(alphas, [](auto& a_value) -> AlphaRecord& { return a_value.record; });
				}
			},
			a_update.record);
		if (!held) {
			++updatesUnheld;
			return { type, nullptr };
		}
		if (a_update.leaf && !(a_update.sequence && type == 1 && geometries.contains(key) && a_update.sequence < geometries.find(key)->second.sequence)) {
			Apply(*a_update.leaf);
			--applied;
		}
		for (std::uint32_t bits = a_update.fields; bits; bits &= bits - 1)
			++updated[type][std::countr_zero(bits)];
		return { type, key };
	}

	void SceneMirror::Clear()
	{
		++writes;
		nodes.clear();
		recent.clear();
		recentLast.clear();
		recentBase = 0;
		recentNewest = 0;
		geometries.clear();
		geometryBySkin.clear();
		properties.clear();
		alphas.clear();
		Diagnostics::Notify(Diagnostics::MirrorCleared{});
	}

	const NodeRecord* SceneMirror::Node(const void* a_key) const
	{
		const auto it = nodes.find(a_key);
		return it != nodes.end() ? &it->second : nullptr;
	}

	const GeometryRecord* SceneMirror::Geometry(const void* a_key) const
	{
		const auto it = geometries.find(a_key);
		return it != geometries.end() ? &it->second : nullptr;
	}

	const PropertyRecord* SceneMirror::Property(const void* a_key) const
	{
		const auto it = properties.find(a_key);
		return it != properties.end() ? &it->second.record : nullptr;
	}

	const AlphaRecord* SceneMirror::Alpha(const void* a_key) const
	{
		const auto it = alphas.find(a_key);
		return it != alphas.end() ? &it->second.record : nullptr;
	}

	SceneCapture::LeafView SceneMirror::Leaf(const void* a_key) const
	{
		SceneCapture::LeafView view;
		view.node = Node(a_key);
		view.geometry = Geometry(a_key);
		if (!view.node || !view.geometry)
			return view;
		view.parent = view.node->parent ? Node(view.node->parent) : nullptr;
		view.property = view.geometry->property ? Property(view.geometry->property) : nullptr;
		view.layer = view.geometry->Layer() ? Property(view.geometry->Layer()) : nullptr;
		view.alpha = view.geometry->alpha ? Alpha(view.geometry->alpha) : nullptr;
		view.fadeNode = view.property && view.property->fadeNode ? Node(view.property->fadeNode) : nullptr;
		view.layerFadeNode = view.layer && view.layer->fadeNode ? Node(view.layer->fadeNode) : nullptr;
		return view;
	}
}
