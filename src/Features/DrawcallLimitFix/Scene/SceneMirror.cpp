#include "SceneMirror.h"

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
		++applied;
		// Properties and alphas first (their values), then the geometries that count their uses.
		for (const auto& p : a_records.properties)
			properties[p.key].record = p;
		for (const auto& a : a_records.alphas)
			alphas[a.key].record = a;
		for (const auto& g : a_records.geometries) {
			// The new record's uses first, then the old one's let go: a property both name keeps its record.
			for (const void* key : { g.property, g.layerProperty })
				if (key)
					++properties[key].uses;
			if (g.alpha)
				++alphas[g.alpha].uses;
			auto [it, inserted] = geometries.try_emplace(g.key, g);
			if (!inserted) {
				Use(it->second.property, it->second.layerProperty, it->second.alpha, -1);
				it->second = g;
			}
		}
		// A property or alpha the capture took that no geometry counts (none can be: each comes with its geometry) is not kept.
		std::erase_if(properties, [](const auto& a_entry) { return a_entry.second.uses == 0; });
		std::erase_if(alphas, [](const auto& a_entry) { return a_entry.second.uses == 0; });
		for (const auto& n : a_records.nodes) {
			nodes[n.key] = n;
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
		++detached;
		// The root leaves its parent's children (the engine's detach, which the hook ran before).
		if (const auto root = nodes.find(a_root); root != nodes.end())
			if (const auto parent = nodes.find(root->second.parent); parent != nodes.end())
				std::erase(parent->second.children, a_root);
		for (const auto* geometry : a_geometries) {
			if (const auto it = geometries.find(geometry); it != geometries.end()) {
				Use(it->second.property, it->second.layerProperty, it->second.alpha, -1);
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

	void SceneMirror::Clear()
	{
		nodes.clear();
		geometries.clear();
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

	void SceneMirror::Check(const Records& a_probe, const KeySet& a_eventKeys)
	{
		auto& t = tally;
		// The last probe's: an event since names the object (late), or none does (missed).
		for (auto& [pendingKey, p] : pending) {
			const void* key = reinterpret_cast<const void*>(pendingKey & ~std::uintptr_t(7));
			const bool late = a_eventKeys.contains(key);
			auto& into = late ? t.late[p.type] : t.missed[p.type];
			for (std::uint32_t f = 0; f < 16; ++f)
				if (p.fields & (1u << f)) {
					++into[f];
					if (!late && t.firstMissedBy[p.type][f].empty())
						t.firstMissedBy[p.type][f] = p.what;
				}
			if (!late)
				for (std::uint32_t b = 0; b < 32; ++b)
					t.flagBitsMissed[b] += (p.flagBits >> b) & 1u;
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
			const std::string what = fmt::format("{} {} '{}': {}", kTypeNames[a_type], a_live.key, a_name ? a_name : "", a_fields(differ));
			if (named.contains(a_live.key)) {
				for (std::uint32_t b = 0; b < 32; ++b)
					t.flagBitsEvented[b] += (a_flagBits >> b) & 1u;
				for (std::uint32_t f = 0; f < 16; ++f)
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
		std::string text = fmt::format("[DCLF] scene mirror (6e F3): {} nodes, {} geometries, {} properties, {} alphas; {} captures applied, {} detaches\n", nodes.size(),
			geometries.size(), properties.size(), alphas.size(), std::exchange(applied, 0), std::exchange(detached, 0));
		auto& t = tally;
		if (t.probes) {
			auto line = [&](const char* a_label, const auto& a_counts) {
				std::string out;
				for (std::size_t type = 0; type < kTypes; ++type) {
					for (std::uint32_t f = 0; f < 16; ++f) {
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
			std::uint64_t missed = 0;
			for (const auto& type : t.missed)
				for (const auto count : type)
					missed += count;
			text += fmt::format("[DCLF] mirror parity: {} probes, {} records compared, {} absent from the mirror; {}; {}; {}{}{}{}{}\n", t.probes, t.checked, t.absent,
				line("evented (a hook without its value):", t.evented), line("late:", t.late), line("missed (no event):", t.missed),
				t.absent || missed ? " <- MIRROR" : " <- OK", t.firstAbsent.empty() ? "" : "; first absent: " + t.firstAbsent,
				t.firstMissed.empty() ? "" : "; first missed: " + t.firstMissed, t.firstEvented.empty() ? "" : "; first evented: " + t.firstEvented);
			std::string bits;
			for (std::uint32_t b = 0; b < 32; ++b)
				if (t.flagBitsMissed[b] || t.flagBitsEvented[b])
					bits += fmt::format("{}bit {} {}/{}", bits.empty() ? "" : ", ", b, t.flagBitsMissed[b], t.flagBitsEvented[b]);
			if (!bits.empty())
				text += fmt::format("[DCLF] mirror parity, node flag bits that differed (missed/evented): {}\n", bits);
			for (std::size_t type = 0; type < kTypes; ++type)
				for (std::uint32_t f = 0; f < 16; ++f)
					if (!t.firstMissedBy[type][f].empty())
						text += fmt::format("[DCLF] mirror parity, first missed {} field {}: {}\n", kTypeNames[type], f, t.firstMissedBy[type][f]);
			t = {};
		}
		return text;
	}
}
