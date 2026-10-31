// Tree wind's seeds: a frame-value pool of the listed trees' nodes as the frame's start had them (TreeStatic, two rows a tree slot:
// MergeTreeSeed), read on the render thread at the frame's start, written and sent by FrameValues' producer, read by TreeWindCS
// through the depth commit's frame row.

#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		float TreeNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}

		// FUN_14147d640(node): the node's object (+0x118, via vfunc 0x30). FUN_140437e50 takes a tree into the near list when
		// object+0x128 -> +0x18 -> int at +0x10 == 1.
		REL::Relocation<std::uintptr_t (*)(const void*)> treeObjectOf{ REL::Offset(Address::kTreeFirstMultiBound) };

		bool NearListSelectable(const void* a_node)
		{
			const std::uintptr_t object = treeObjectOf(a_node);
			if (!object)
				return false;
			const auto first = *reinterpret_cast<const std::uintptr_t*>(object + 0x128);
			if (!first)
				return false;
			const auto second = *reinterpret_cast<const std::uintptr_t*>(first + 0x18);
			return second && *reinterpret_cast<const std::int32_t*>(second + 0x10) == 1;
		}

		/** @brief A BSTreeNode's static row into a_out (the render thread's tree seeds). */
		void TreeStaticOfNode(const void* a_node, TreeStatic& a_out)
		{
			a_out = {};
			const void* node = a_node;
			const auto* bytes = static_cast<const std::byte*>(node);
			std::memcpy(a_out.position, bytes + 0xA0, sizeof(a_out.position));
			a_out.leafFrequency = TreeNodeFloat(node, 0x160);
			a_out.timer = TreeNodeFloat(node, 0x164);
			a_out.previousTimer = TreeNodeFloat(node, 0x168);
			a_out.amplitude = TreeNodeFloat(node, 0x15c);
			// The manager advances a node only in its near loop (FUN_1404381e0's loop 2 over FUN_140437e50's near list): one the list
			// selects whose model (+0xF8, its +0x40) has bones (+0xB8), scaling the gust by the model's +0xB0. Other nodes keep their
			// clock and amplitude; their +0xB0 is never read (can be garbage like 2.3e36). Not replicated: the near list's count cap.
			const auto* holder = *reinterpret_cast<const std::byte* const*>(bytes + 0xF8);
			const auto* model = holder ? *reinterpret_cast<const std::byte* const*>(holder + 0x40) : nullptr;
			const bool bones = model && *reinterpret_cast<const void* const*>(model + 0xB8);
			a_out.animated = bones && NearListSelectable(node) ? 1u : 0u;
			a_out.modelAmplitude = a_out.animated ? *reinterpret_cast<const float*>(model + 0xB0) : 0.0f;
		}

		Api::FrameValuePoolId pool;  // install

		// What a kick hands its producer: the seeds read since the last kick (row, value; in request order), and the rows they name.
		struct SeedInput
		{
			std::vector<std::pair<std::uint32_t, TreeStatic>> values;
			std::uint32_t rows = 0;
		};

		// Render thread: the seeds read at the frames' starts that no kick handed over yet, the most rows named (2 * slot + seedOdd + 1:
		// the rows never shrink), and the input a kick hands when nothing is new (kept while the rows stand: no allocation a frame).
		std::vector<std::pair<std::uint32_t, TreeStatic>> unsent;
		std::uint32_t seedRows = 0;
		std::shared_ptr<const SeedInput> idle;

		// The producers', one at a time in frame order: the seed rows, by 2 * tree slot + seedOdd.
		KeptWholeArray<TreeStatic> seeds;

		// Render thread, every frame's start: the seeds the installed publications asked for (the component's listings: FrameView).
		void FrameStart(const Api::FrameValuePoolFrameStart& a_start)
		{
			FrameGlobals::Scope scope(a_start.globals);
			// Read here, not by the producer: TreeStaticOfNode's near-list test reads the tree's multibound shape's cull result (FUN_14147d640
			// is a pure read, the first child's AsMultiBoundNode; then the shape's +0x10), which this frame's culls write.
			for (const auto& seed : Scene::FrameView::Get().TakeComponentSeeds(ComponentId())) {
				if (!seed.node)
					continue;
				TreeStatic row;
				TreeStaticOfNode(seed.node.get(), row);
				row.generation = seed.generation;
				row.seedOdd = seed.row & 1u;
				unsent.emplace_back(seed.row, row);
				seedRows = std::max(seedRows, seed.row + 1);
			}
		}

		// Render thread, at Kick: every seed read since the last kick, for the frame's producer.
		Api::FrameValueInput Kick(const Api::FrameValuePoolKick&)
		{
			if (!unsent.empty())
				return { std::make_shared<const SeedInput>(SeedInput{ std::exchange(unsent, {}), seedRows }), seedRows };
			if (!idle || idle->rows != seedRows)
				idle = std::make_shared<const SeedInput>(SeedInput{ {}, seedRows });
			return { idle, seedRows };
		}

		void Sample(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.BeginBuild(seeds);
			auto& trees = seeds.Mutable();
			const auto* input = a_producer.Input<SeedInput>();
			const std::uint32_t rows = input ? input->rows : 0u;
			if (trees.size() < rows) {
				const std::size_t first = trees.size();
				trees.resize(rows, TreeStatic{});
				seeds.MarkRange(first, rows - first);
			}
			std::uint64_t seeded = 0;
			if (input)
				for (const auto& [at, row] : input->values) {
					if (at >= trees.size())
						continue;
					trees[at] = row;
					seeds.Mark(at);
					++seeded;
				}
			a_producer.NoteSeeded(seeded);
		}

		void Send(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(seeds);
		}
	}

	void RegisterSeeds(Api::InstallContext& a_context)
	{
		pool = a_context.RegisterFrameValuePool({ .name = "tree-seeds", .stride = sizeof(TreeStatic), .frameStart = &FrameStart, .kick = &Kick, .sample = &Sample,
			.send = &Send });
	}

	std::uint32_t SeedsIndex()
	{
		return Extensions::FrameValuePoolIndex(pool);
	}

	const std::vector<TreeStatic>* SeedsIfDone(bool a_wait)
	{
		return FrameValues::Get().ProducerDone(a_wait) ? &seeds.Get() : nullptr;
	}
}
