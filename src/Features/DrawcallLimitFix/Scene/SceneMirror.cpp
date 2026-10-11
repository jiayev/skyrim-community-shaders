#include "SceneMirror.h"

#include <fmt/ranges.h>

#include "Features/DrawcallLimitFix/Diagnostics/MirrorWatch.h"

namespace DCLF
{
	namespace
	{
		using namespace SceneCapture;

		constexpr std::array<const char*, 4> kTypeNames{ "node", "geometry", "property", "alpha" };

		template <class Record>
		std::string Fields(std::uint32_t a_fields)
		{
			std::string text;
			for (std::uint32_t f = 0; f < Record::kFieldCount; ++f)
				if (a_fields & (1u << f))
					text += fmt::format("{}{}", text.empty() ? "" : "+", Record::kFieldNames[f]);
			return text;
		}
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
		for (const auto& n : a_records.nodes) {
			take(nodes[n.key], n);
			named.erase(n.key);
		}
		for (const auto& g : a_records.geometries)
			named.erase(g.key);
		for (const auto& p : a_records.properties)
			named.erase(p.key);
		for (const auto& a : a_records.alphas)
			named.erase(a.key);
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
			named.erase(geometry);
		}
		for (const void* node : a_nodes) {
			nodes.erase(node);
			named.erase(node);
		}
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
		pending.clear();
		named.clear();
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

	void SceneMirror::Check(const Records& a_probe, const KeySet& a_eventKeys, const FieldMap& a_eventFields)
	{
		auto& t = tally;
		// The last probe's: an event without values since names the object, or updates its fields (late), or nothing does (missed).
		for (auto& [pendingKey, p] : pending) {
			const void* key = reinterpret_cast<const void*>(pendingKey & ~std::uintptr_t(7));
			const auto fields = a_eventFields.find(pendingKey);
			const bool late = a_eventKeys.contains(key) || (fields != a_eventFields.end() && (fields->second & p.fields) == p.fields);
			auto& into = late ? t.late[p.type] : t.missed[p.type];
			for (std::uint32_t f = 0; f < kMaxFields; ++f)
				if (p.fields & (1u << f)) {
					++into[f];
					if (!late && t.firstMissedBy[p.type][f].empty()) {
						t.firstMissedBy[p.type][f] = p.what;
						// A property has no name: a geometry naming it (once per field and report).
						if (p.type == 2)
							for (const auto& [geometryKey, geometry] : geometries)
								if (geometry.property == key || geometry.layerProperty == key) {
									const auto* node = Node(geometryKey);
									t.firstMissedBy[p.type][f] += fmt::format(" (geometry {} '{}')", geometryKey, node && node->name ? node->name : "");
									break;
								}
					}
				}
			if (!late)
				for (std::uint32_t b = 0; b < 32; ++b)
					t.flagBitsMissed[b] += (p.flagBits >> b) & 1u;
			if (!late && p.type == 2 && (p.fields & MirrorWatch::PropertyFields()))
				missedProperty = key;
			if (!late && p.type == 3)
				missedAlpha = key;
			if (!late && t.firstMissed.empty())
				t.firstMissed = std::move(p.what);
		}
		pending.clear();
		// Only what the mirror holds (an event names many objects out of the world).
		for (const void* key : a_eventKeys)
			if (nodes.contains(key) || properties.contains(key) || alphas.contains(key))
				named.insert(key);
		++t.probes;
		auto compare = [&](std::uint8_t a_type, const auto& a_live, const auto* a_mirror, auto a_fields, const char* a_name, std::uint32_t a_flagBits) {
			++t.checked;
			if (!a_mirror) {
				if (t.absent++ == 0)
					t.firstAbsent = fmt::format("{} {} '{}'", kTypeNames[a_type], a_live.key, a_name ? a_name : "");
				return;
			}
			const std::uint32_t differ = a_mirror->Differ(a_live);
			if (!differ)
				return;
			std::string what = fmt::format("{} {} '{}': {}", kTypeNames[a_type], a_live.key, a_name ? a_name : "", a_fields(differ));
			// The values, mirror -> live, of the fields worth seeing.
			if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, NodeRecord>) {
				if (differ & (NodeRecord::kFadeNear | NodeRecord::kFadeFar | NodeRecord::kFadeType))
					what += fmt::format(" (near {} -> {}, far {} -> {}, type {} -> {}; captured frame {} thread {}, updated frame {}, probed frame {})", a_mirror->fadeNear,
						a_live.fadeNear, a_mirror->fadeFar, a_live.fadeFar, a_mirror->fadeType, a_live.fadeType, a_mirror->capturedFrame, a_mirror->thread,
						a_mirror->updatedFrame, a_live.capturedFrame);
				if (differ & NodeRecord::kFlags)
					what += fmt::format(" (flags {:#x} -> {:#x})", a_mirror->flags, a_live.flags);
				if (differ & (NodeRecord::kFadeCurrent | NodeRecord::kFadeLevel | NodeRecord::kFadeDoor))
					what += fmt::format(" (current fade {} -> {}, LOD level {} -> {}, screen door {} -> {}; captured frame {} thread {}, updated frame {}, probed frame {})",
						a_mirror->currentFade, a_live.currentFade, a_mirror->fadeLevel, a_live.fadeLevel, a_mirror->fadeDoor, a_live.fadeDoor, a_mirror->capturedFrame,
						a_mirror->thread, a_mirror->updatedFrame, a_live.capturedFrame);
				if (differ & NodeRecord::kBody) {
					// Diagnostic (the parity's, F3): the live chain NonFixedBody reads, from the probe's key (alive: the probe just read it).
					const auto* node = static_cast<const RE::NiAVObject*>(a_live.key);
					const auto* collision = node->collisionObject.get();
					const auto* ni = collision ? const_cast<RE::NiCollisionObject*>(collision)->AsBhkNiCollisionObject() : nullptr;
					const auto* body = ni ? ni->body.get() : nullptr;
					const auto* rtti = body ? const_cast<RE::bhkWorldObject*>(body)->GetRTTI() : nullptr;
					const auto* entity = body ? reinterpret_cast<const std::byte*>(body->referencedObject.get()) : nullptr;
					what += fmt::format(" (live: collision {} body {} '{}' entity {} motion {})", static_cast<const void*>(collision), static_cast<const void*>(body),
						rtti && rtti->name ? rtti->name : "", static_cast<const void*>(entity), entity ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(entity + 0x160)) : -1);
				}
				if (differ & (NodeRecord::kBody | NodeRecord::kFade109))
					what += fmt::format(" (body {} -> {}, +0x109 {:#x} -> {:#x}; captured frame {} thread {}, updated frame {})", a_mirror->body, a_live.body, a_mirror->fade109,
						a_live.fade109, a_mirror->capturedFrame, a_mirror->thread, a_mirror->updatedFrame);
			}
			if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, AlphaRecord>)
				what += fmt::format(" (flags {:#x} -> {:#x}, threshold {} -> {})", a_mirror->flags, a_live.flags, a_mirror->threshold, a_live.threshold);
			if constexpr (std::is_same_v<std::decay_t<decltype(a_live)>, PropertyRecord>) {
				if (differ & PropertyRecord::kFlags)
					what += fmt::format(" (flags {:#x} -> {:#x}, bits {:#x}; captured frame {} thread {} sequence {}, last written by {} at frame {}; now frame {} sequence {})",
						a_mirror->flags, a_live.flags, a_mirror->flags ^ a_live.flags, a_mirror->capturedFrame, a_mirror->thread, a_mirror->sequence,
						a_mirror->writer == 1 ? "a capture" : a_mirror->writer == 2 ? "an update" : "?", a_mirror->writtenFrame, SceneCapture::Frame(),
						SceneCapture::NextSequence());
				if (differ & (PropertyRecord::kAlphaValue | PropertyRecord::kProjected | PropertyRecord::kLandBlend))
					what += fmt::format(" (alpha {} -> {}, projected [{}] -> [{}], land blend [{}] -> [{}]; captured frame {} thread {}, last written by {} at frame {}; now frame {})",
						a_mirror->alpha, a_live.alpha, fmt::join(a_mirror->projected, " "), fmt::join(a_live.projected, " "), fmt::join(a_mirror->landBlend, " "),
						fmt::join(a_live.landBlend, " "), a_mirror->capturedFrame, a_mirror->thread, a_mirror->writer == 1 ? "a capture" : a_mirror->writer == 2 ? "an update" : "?",
						a_mirror->writtenFrame, SceneCapture::Frame());
			}
			if (a_type == 0 && (differ & NodeRecord::kFadeCurrent) && MirrorWatch::CurrentFromParity())
				missedNode = a_live.key;
			if (named.contains(a_live.key)) {
				for (std::uint32_t b = 0; b < 32; ++b)
					t.flagBitsEvented[b] += (a_flagBits >> b) & 1u;
				for (std::uint32_t f = 0; f < kMaxFields; ++f)
					if (differ & (1u << f))
						++t.evented[a_type][f];
				if (t.firstEvented.empty())
					t.firstEvented = what;
				return;
			}
			auto& p = pending[reinterpret_cast<std::uintptr_t>(a_live.key) | a_type];
			p.type = a_type;
			p.fields |= differ;
			p.flagBits |= a_flagBits;
			if (p.what.empty())
				p.what = what;
		};
		for (const auto& n : a_probe.nodes) {
			const auto* mirrored = Node(n.key);
			compare(0, n, mirrored, Fields<NodeRecord>, n.name, mirrored ? (mirrored->flags ^ n.flags) : 0u);
		}
		for (const auto& g : a_probe.geometries) {
			const auto* node = Node(g.key);
			compare(1, g, Geometry(g.key), Fields<GeometryRecord>, node ? node->name : nullptr, 0u);
		}
		for (const auto& p : a_probe.properties)
			compare(2, p, Property(p.key), Fields<PropertyRecord>, nullptr, 0u);
		for (const auto& a : a_probe.alphas)
			compare(3, a, Alpha(a.key), Fields<AlphaRecord>, nullptr, 0u);
	}

	std::string SceneMirror::Report()
	{
		std::string text = fmt::format(
			"[DCLF] scene mirror (6e F3): {} nodes, {} geometries, {} properties, {} alphas; {} captures applied, {} detaches, {} updates ({} to objects it holds "
			"no record of)\n",
			nodes.size(), geometries.size(), properties.size(), alphas.size(), std::exchange(applied, 0), std::exchange(detached, 0), std::exchange(updates, 0),
			std::exchange(updatesUnheld, 0));
		if (lightEvictions)
			text += fmt::format("[DCLF] scene mirror evictions (T6b1b): {} lights and cameras named by an attach out of the world (detached by no hook)\n", std::exchange(lightEvictions, 0));
		if (evictions)
			text += fmt::format("[DCLF] scene mirror evictions (T6b1b): {} records named by an attach out of the world ({} records with them: left the world by no detach a hook saw) <- MIRROR EVICTED; first: {}\n",
				std::exchange(evictions, 0), std::exchange(evictedRecords, 0), std::exchange(firstEviction, {}));
		text += fmt::format(
			"[DCLF] scene mirror order (6e F3c): {} updates replayed after a capture numbered before them, {} skipped as older than their record's capture, {} "
			"records a capture left to a newer one\n",
			std::exchange(replayed, 0), std::exchange(stale, 0), std::exchange(superseded, 0));
		auto& t = tally;
		auto line = [&](const char* a_label, const auto& a_counts) {
				std::string out;
				for (std::size_t type = 0; type < kTypes; ++type) {
					for (std::uint32_t f = 0; f < kMaxFields; ++f) {
						if (!a_counts[type][f])
							continue;
						const char* name = type == 0 ? (f < NodeRecord::kFieldCount ? NodeRecord::kFieldNames[f] : "?") :
						                   type == 1 ? (f < GeometryRecord::kFieldCount ? GeometryRecord::kFieldNames[f] : "?") :
						                   type == 2 ? (f < PropertyRecord::kFieldCount ? PropertyRecord::kFieldNames[f] : "?") :
						                               (f < AlphaRecord::kFieldCount ? AlphaRecord::kFieldNames[f] : "?");
						out += fmt::format("{}{} {} {}", out.empty() ? "" : ", ", kTypeNames[type], name, a_counts[type][f]);
					}
				}
				return fmt::format("{} {}", a_label, out.empty() ? "0" : out);
			};
		text += fmt::format("[DCLF] scene mirror, {}\n", line("updated fields:", updated));
		updated = {};
		if (t.probes) {
			std::uint64_t missed = 0;
			for (const auto& type : t.missed)
				for (const auto count : type)
					missed += count;
			text += fmt::format("[DCLF] mirror parity: {} probes, {} records compared, {} absent from the mirror; {}; {}; {}{}{}{}{}\n", t.probes, t.checked, t.absent,
				line("evented (an event without values):", t.evented), line("late:", t.late), line("missed (no event):", t.missed),
				t.absent || missed ? " <- MIRROR" : " <- OK", t.firstAbsent.empty() ? "" : "; first absent: " + t.firstAbsent,
				t.firstMissed.empty() ? "" : "; first missed: " + t.firstMissed, t.firstEvented.empty() ? "" : "; first evented: " + t.firstEvented);
			std::string bits;
			for (std::uint32_t b = 0; b < 32; ++b)
				if (t.flagBitsMissed[b] || t.flagBitsEvented[b])
					bits += fmt::format("{}bit {} {}/{}", bits.empty() ? "" : ", ", b, t.flagBitsMissed[b], t.flagBitsEvented[b]);
			if (!bits.empty())
				text += fmt::format("[DCLF] mirror parity, node flag bits that differed (missed/evented): {}\n", bits);
			for (std::size_t type = 0; type < kTypes; ++type)
				for (std::uint32_t f = 0; f < kMaxFields; ++f)
					if (!t.firstMissedBy[type][f].empty())
						text += fmt::format("[DCLF] mirror parity, first missed {} field {}: {}\n", kTypeNames[type], f, t.firstMissedBy[type][f]);
			t = {};
		}
		return text;
	}
}
