// The late poses' search, plans and rows (see the header).

#include "Features/DrawcallLimitFix/Core/LatePoses/LatePoseOverrides.h"

#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LatePoses.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <atomic>

namespace DCLF::LatePoses
{
	namespace
	{
		// A node is hidden when its world scale is below this share of its parent's. Improved Camera's is 0.001: it sets the local scale
		// (kept until the next frame's start, or put back at once with the world left shrunk). Anything this small draws nothing.
		constexpr float kHiddenRatio = 0.01f;
		// A plan's factor at or above this is no shrink: the node is as small at the frame's start (shrunk for good), so the rows the
		// producer samples are already shrunk.
		constexpr float kNoShrink = 0.5f;
		// Nodes a frame may hide (a plan's scale index is 16 bits, 0 meaning none).
		constexpr std::size_t kMaxHidden = 64;

		// One search's result, holding the engine objects it names (released on the render thread when replaced).
		struct Search
		{
			std::vector<RE::NiPointer<RE::NiAVObject>> nodes;
			std::vector<float> worldScales;  // each node's world scale as found
			struct Skin
			{
				RE::NiPointer<RE::BSGeometry> geometry;
				std::vector<std::uint16_t> scaleOf;
			};
			std::vector<Skin> skins;
			std::vector<std::pair<RE::NiPointer<RE::BSGeometry>, std::uint16_t>> rigid;
			bool rooted = false;  // a root was named

			bool Empty() const { return nodes.empty(); }
		};

		// Render thread.
		struct State
		{
			std::uint64_t frame = 0;    // PrepareFrame's count
			Search last;                // the newest search (ConfirmFrame's)
			std::uint64_t lastFrame = 0;  // the frame it was taken in
			Search inUse;               // the one the frame's plan came from: its objects outlive the frame's producer
			bool planned = false;       // this frame has a plan
			bool searchedEmpty = false;  // this frame's depth commit named a root and found no hidden node
		};
		State& S()
		{
			static State state;
			return state;
		}

		// Any thread (the producer's adds, the report's take).
		struct Counters
		{
			std::atomic<std::uint64_t> rooted{ 0 }, hidden{ 0 }, planned{ 0 }, confirmed{ 0 }, refused{ 0 }, unplanned{ 0 }, afterDepth{ 0 };
			std::atomic<std::uint64_t> skins{ 0 }, rigid{ 0 }, missing{ 0 }, maxNodes{ 0 };
		};
		Counters counters;

		void NoteMax(std::atomic<std::uint64_t>& a_max, std::uint64_t a_value)
		{
			for (auto max = a_max.load(std::memory_order_relaxed); a_value > max && !a_max.compare_exchange_weak(max, a_value, std::memory_order_relaxed);) {}
		}

		// Whether a node is hidden: its world scale far below its parent's.
		bool Hidden(const RE::NiAVObject& a_node)
		{
			const auto* parent = a_node.parent;
			return parent && parent->world.scale > 1e-4f && a_node.world.scale < kHiddenRatio * parent->world.scale;
		}

		// Every hidden node under a_root (the outermost one of a hidden chain), the skins with bones under them, and the geometry
		// without a skin under them.
		void SearchRoot(RE::NiAVObject* a_root, Search& a_out)
		{
			// First the nodes: each node's hidden ancestor (its index + 1), the geometry met on the way.
			ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint16_t> hiddenOf;
			std::vector<std::pair<RE::BSGeometry*, std::uint16_t>> geometry;
			std::vector<std::pair<RE::NiAVObject*, std::uint16_t>> stack{ { a_root, std::uint16_t{ 0 } } };
			while (!stack.empty()) {
				auto [object, hidden] = stack.back();
				stack.pop_back();
				if (!object)
					continue;
				if (auto* shape = object->AsGeometry()) {
					geometry.emplace_back(shape, hidden);
					continue;
				}
				auto* node = object->AsNode();
				if (!node)
					continue;
				if (!hidden && object != a_root && a_out.nodes.size() < kMaxHidden)
					if (Hidden(*object)) {
						a_out.nodes.emplace_back(object);
						a_out.worldScales.push_back(object->world.scale);
						hidden = static_cast<std::uint16_t>(a_out.nodes.size());
					}
				if (hidden)
					hiddenOf.emplace(object, hidden);
				for (auto& child : node->GetChildren())
					stack.emplace_back(child.get(), hidden);
			}
			if (hiddenOf.empty())
				return;
			// Then the geometry: a skin by its bones (any skin under the root may have bones in a hidden subtree), the rest whole.
			for (auto [shape, hidden] : geometry) {
				auto* skin = shape->GetGeometryRuntimeData().skinInstance.get();
				const auto* data = skin ? skin->skinData.get() : nullptr;
				if (!skin || !data || !skin->bones) {
					if (hidden && !skin)
						a_out.rigid.emplace_back(RE::NiPointer<RE::BSGeometry>(shape), hidden);
					continue;
				}
				const std::uint32_t bones = data->GetBoneCount();
				std::vector<std::uint16_t> scaleOf(bones, 0);
				bool any = false;
				for (std::uint32_t b = 0; b < bones; ++b)
					if (const auto it = hiddenOf.find(skin->bones[b]); it != hiddenOf.end()) {
						scaleOf[b] = it->second;
						any = true;
					}
				if (any)
					a_out.skins.push_back({ RE::NiPointer<RE::BSGeometry>(shape), std::move(scaleOf) });
			}
		}

		Search SearchRoots()
		{
			Search search;
			for (const auto& pose : Extensions::LatePoses())
				if (const void* root = pose.root ? pose.root() : nullptr) {
					search.rooted = true;
					SearchRoot(const_cast<RE::NiAVObject*>(static_cast<const RE::NiAVObject*>(root)), search);
				}
			return search;
		}

		// x' = center + factor * (x - center) on a 3x4 transform of three float4 rows (translation in w).
		void ScaleRows(float* a_rows, const FramePlan::Scale& a_scale)
		{
			for (int row = 0; row < 3; ++row) {
				float* r = a_rows + row * 4;
				r[0] *= a_scale.factor;
				r[1] *= a_scale.factor;
				r[2] *= a_scale.factor;
				r[3] = a_scale.center[row] + a_scale.factor * (r[3] - a_scale.center[row]);
			}
		}
	}

	std::shared_ptr<const FramePlan> PrepareFrame()
	{
		auto& s = S();
		const std::uint64_t previous = s.frame++;
		s.planned = false;
		s.searchedEmpty = false;
		// The last plan's objects: its producer is done (the frame before presented after it).
		s.inUse = {};
		if (s.last.Empty() || s.lastFrame != previous) {
			s.last = {};
			return nullptr;
		}
		s.inUse = std::move(s.last);
		s.last = {};
		// The scales about each hidden node's world position now (the frame's start, the full pose the producer samples), by the share
		// of its world scale now that it was hidden to. One as small now shrinks nothing: its scale index stays 0 below.
		auto plan = std::make_shared<FramePlan>();
		std::vector<std::uint16_t> index(s.inUse.nodes.size() + 1, 0);
		for (std::size_t i = 0; i < s.inUse.nodes.size(); ++i) {
			const auto& node = *s.inUse.nodes[i];
			const float factor = node.world.scale > 1e-6f ? s.inUse.worldScales[i] / node.world.scale : 1.0f;
			if (!(factor < kNoShrink))
				continue;
			const auto& at = node.world.translate;
			plan->scales.push_back({ { at.x, at.y, at.z }, factor });
			index[i + 1] = static_cast<std::uint16_t>(plan->scales.size());
		}
		for (const auto& skin : s.inUse.skins) {
			FramePlan::Skin planned{ skin.geometry.get(), static_cast<std::uint32_t>(skin.scaleOf.size() * SkinPalettes::kRowsPerBone), skin.scaleOf };
			bool any = false;
			for (auto& scale : planned.scaleOf)
				any |= (scale = index[scale]) != 0;
			if (any)
				plan->skins.push_back(std::move(planned));
		}
		for (const auto& [shape, scale] : s.inUse.rigid)
			if (index[scale])
				plan->rigid.push_back({ shape.get(), index[scale] });
		if (plan->skins.empty() && plan->rigid.empty())
			return nullptr;
		s.planned = true;
		++counters.planned;
		return plan;
	}

	bool ConfirmFrame()
	{
		auto& s = S();
		auto search = SearchRoots();
		if (search.rooted)
			++counters.rooted;
		if (!search.Empty()) {
			++counters.hidden;
			NoteMax(counters.maxNodes, search.nodes.size());
		}
		s.searchedEmpty = search.rooted && search.Empty();
		// The plan holds when each node it shrinks is shrunk now: else (the plugin stopped hiding one) the frame draws its first rows.
		bool holds = s.planned;
		for (std::size_t i = 0; holds && i < s.inUse.nodes.size(); ++i)
			holds = std::ranges::find(search.nodes, s.inUse.nodes[i]) != search.nodes.end();
		if (s.planned)
			++(holds ? counters.confirmed : counters.refused);
		else if (!search.Empty())
			++counters.unplanned;
		s.last = std::move(search);
		s.lastFrame = s.frame;
		return holds;
	}

	void CheckAtPresent()
	{
		auto& s = S();
		if (!std::exchange(s.searchedEmpty, false))
			return;
		if (!SearchRoots().Empty())
			++counters.afterDepth;
	}

	void BuildRows(const FramePlan& a_plan, const std::function<bool(const void*, Location&)>& a_find, const std::vector<BindlessPlacement>& a_rows,
		const std::vector<FrameValues::PaletteRow>& a_palettes, Rows& a_out)
	{
		a_out.Clear();
		const auto scaleOf = [&](std::uint16_t a_index) -> const FramePlan::Scale* {
			return a_index && a_index <= a_plan.scales.size() ? &a_plan.scales[a_index - 1] : nullptr;
		};
		for (const auto& skin : a_plan.skins) {
			Location at;
			if (!a_find(skin.geometry, at) || at.boneRows != skin.boneRows) {
				++counters.missing;
				continue;
			}
			const auto block = PaletteRowsOf(at.boneOffset, at.boneRows);
			if (std::size_t(block.current) + 2ull * at.boneRows > a_palettes.size()) {
				++counters.missing;
				continue;
			}
			// The whole block (current, then previous), its hidden bones shrunk in both.
			const std::size_t first = a_out.palettes.size();
			a_out.palettes.insert(a_out.palettes.end(), a_palettes.begin() + block.current, a_palettes.begin() + block.current + 2ull * at.boneRows);
			for (std::size_t b = 0; b < skin.scaleOf.size(); ++b)
				if (const auto* scale = scaleOf(skin.scaleOf[b])) {
					ScaleRows(a_out.palettes[first + b * SkinPalettes::kRowsPerBone].v, *scale);
					ScaleRows(a_out.palettes[first + at.boneRows + b * SkinPalettes::kRowsPerBone].v, *scale);
				}
			a_out.paletteRuns.emplace_back(block.current, 2 * at.boneRows);
			++counters.skins;
		}
		for (const auto& rigid : a_plan.rigid) {
			Location at;
			const auto* scale = scaleOf(rigid.scale);
			if (!scale || !a_find(rigid.geometry, at)) {
				++counters.missing;
				continue;
			}
			for (const auto slot : { at.slot, at.layerSlot }) {
				if (slot == kNoObjectSlot || slot >= a_rows.size())
					continue;
				auto row = a_rows[slot];
				ScaleRows(row.world, *scale);
				ScaleRows(row.previousWorld, *scale);
				a_out.placements.push_back(row);
				a_out.placementSlots.push_back(slot);
			}
			++counters.rigid;
		}
	}

	std::string TakeReport()
	{
		auto& c = counters;
		const auto rooted = c.rooted.exchange(0);
		const auto hidden = c.hidden.exchange(0), planned = c.planned.exchange(0), confirmed = c.confirmed.exchange(0), refused = c.refused.exchange(0);
		const auto unplanned = c.unplanned.exchange(0), afterDepth = c.afterDepth.exchange(0);
		const auto skins = c.skins.exchange(0), rigid = c.rigid.exchange(0), missing = c.missing.exchange(0), maxNodes = c.maxNodes.exchange(0);
		if (!rooted)
			return {};
		return fmt::format("late poses: {} frames with a root, {} with hidden nodes (at most {}), {} found only after the depth commit; {} plans, "
						   "{} used, {} refused (a node shown again), {} frames hidden without a plan (a hide's first frame); {} skins and {} rigid "
						   "geometries shrunk, {} not among the producer's movers (so far the engine's own draws, which its hide covers)",
			rooted, hidden, maxNodes, afterDepth, planned, confirmed, refused, unplanned, skins, rigid, missing);
	}
}
