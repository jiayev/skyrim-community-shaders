// SceneMirror's record maintenance: captures applied newest-wins with the later updates replayed over them, detaches, evictions
// and hook updates. Scene-work thread (the coordinator's event application).

#include "Features/DrawcallLimitFix/Scene/Model/SceneMirror.h"

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

	void SceneMirror::Apply(const Records& a_records)
	{
		++writes;
		ApplyCapture(a_records);
		if (replaying || !a_records.sequence)
			return;
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

	void SceneMirror::ApplyCapture(const Records& a_records)
	{
		++applied;
		// A record a newer capture took stays (two loader threads capture subtrees naming one shared property or ancestor; the
		// older capture can be pushed last).
		const std::uint64_t sequence = a_records.sequence;
		auto take = [&](auto& a_record, const auto& a_captured) {
			if (a_record.key && a_record.sequence > sequence) {
				++superseded;
				return;
			}
			a_record = a_captured;
			a_record.sequence = sequence;
			if constexpr (requires { a_record.writer; }) {
				a_record.writer = 1;
				a_record.writtenFrame = SceneCapture::Frame();
			}
		};
		// Properties and alphas first (their values), then the geometries that count their uses.
		for (const auto& p : a_records.properties)
			take(properties[p.key].record, p);
		for (const auto& a : a_records.alphas)
			take(alphas[a.key].record, a);
		for (const auto& g : a_records.geometries) {
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
			auto [it, inserted] = geometries.try_emplace(g.key, g);
			if (!inserted) {
				Use(it->second.property, it->second.layerProperty, it->second.alpha, -1);
				if (it->second.skin && it->second.skin != g.skin)
					geometryBySkin.erase(it->second.skin);
				it->second = g;
			}
			it->second.sequence = a_records.sequence;
			if (g.skin)
				geometryBySkin[g.skin] = g.key;
		}
		// A property or alpha the capture took that no geometry counts (none can be: each comes with its geometry) is not kept.
		std::erase_if(properties, [](const auto& a_entry) { return a_entry.second.uses == 0; });
		std::erase_if(alphas, [](const auto& a_entry) { return a_entry.second.uses == 0; });
		for (const auto& n : a_records.nodes)
			take(nodes[n.key], n);
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
		std::erase_if(recent, [this](auto& a_entry) {
			std::erase_if(a_entry.second, [this](const Recent& a_recent) { return a_recent.batch + 2 < batch; });
			return a_entry.second.empty();
		});
	}

	std::pair<std::uint8_t, const void*> SceneMirror::Update(const SceneCapture::Update& a_update)
	{
		++writes;
		++updates;
		const auto result = UpdateRecord(a_update);
		// Kept by the key it names (a geometry named by its skin, by its key once resolved).
		const auto [type, key] = TypeOf(a_update);
		if (const void* object = result.second ? result.second : key)
			recent[Key(object, type)].push_back({ batch, a_update });
		return result;
	}

	void SceneMirror::Replay(std::uintptr_t a_key, std::uint64_t a_after)
	{
		const auto it = recent.find(a_key);
		if (it == recent.end())
			return;
		// In their order (a later update may have been pushed first).
		std::vector<const SceneCapture::Update*> later;
		for (const auto& r : it->second)
			if (r.update.sequence > a_after)
				later.push_back(&r.update);
		std::ranges::sort(later, {}, [](const auto* a_update) { return a_update->sequence; });
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
		for (std::uint32_t f = 0; f < kMaxFields; ++f)
			updated[type][f] += (a_update.fields >> f) & 1u;
		return { type, key };
	}

	void SceneMirror::Clear()
	{
		++writes;
		nodes.clear();
		recent.clear();
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
