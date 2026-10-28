#include "Rendering/EnvironmentProbe/Scene.h"

#include "Globals.h"
#include "Rendering/EnvironmentProbe/Capture.h"
#include "State.h"
#include <atomic>

namespace EnvironmentProbe::Scene
{
	namespace
	{
		struct MaterialPasses
		{
			RE::BSLightingShaderProperty* property;
			uint32_t flags;
			size_t first;
			size_t count;
		};
		struct Frame
		{
			struct Frontier
			{
				size_t first;
				size_t count;
			};
			uint32_t number = UINT32_MAX;
			RE::NiAVObject* scene = nullptr;
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
			std::unordered_map<RE::NiAVObject*, Frontier> frontiers;
			std::unordered_set<RE::NiAVObject*> branches;
			std::unordered_map<RE::BSGeometry*, MaterialPasses> materials;
			std::vector<RE::BSRenderPass> passes;
			void Advance(uint32_t value)
			{
				if (number == value)
					return;
				number = value;
				scene = nullptr;
				roots.clear();
				frontiers.clear();
				branches.clear();
				materials.clear();
				passes.clear();
			}
		};
		Frame frame;
		std::mutex mutex;
		std::atomic<uint32_t> requestedFrame = UINT32_MAX;
		bool Requested()
		{
			const auto requested = requestedFrame.load(std::memory_order_relaxed);
			return requested != UINT32_MAX && globals::state->frameCountAtomic.load(std::memory_order_relaxed) - requested <= 1;
		}
		struct Visit
		{
			bool children = false;
			bool grouped = false;
		};
		struct Traversal
		{
			RE::BSCullingProcess* process;
			Visit* parent = nullptr;
			std::vector<RE::NiPointer<RE::NiAVObject>> roots;
		};
		thread_local Traversal* traversal = nullptr;

		struct ProcessObject
		{
			static void Record(RE::BSCullingProcess* process, RE::NiAVObject* object, int32_t group)
			{
				auto& current = *traversal;
				auto* parent = current.parent;
				const bool grouped = process->isGroupingAlphas || group >= 0;
				if (parent) {
					parent->children = true;
					parent->grouped |= grouped;
				}
				Visit visit;
				current.parent = &visit;
				const auto first = current.roots.size();
				func(process, object, group);
				current.parent = parent;
				// A pruned node represents its whole unvisited subtree. Retain alpha-group
				// roots as units so replaying the frontier preserves native grouping.
				if (!visit.children || (visit.grouped && !grouped)) {
					current.roots.resize(first);
					current.roots.emplace_back(object);
				}
			}
			static void thunk(RE::BSCullingProcess* process, RE::NiAVObject* object, int32_t group)
			{
				if (traversal) {
					if (traversal->process == process)
						Record(process, object, group);
					else
						func(process, object, group);
					return;
				}
				if (IsRendering() || !globals::state || !globals::game::smState || !Requested() ||
					process->camera != RE::Main::WorldRootCamera() || process->isGroupingAlphas || group >= 0) {
					func(process, object, group);
					return;
				}
				auto* target = globals::game::smState->shadowSceneNode[0];
				auto* ancestor = object;
				while (ancestor && ancestor != target)
					ancestor = ancestor->parent;
				if (!ancestor) {
					func(process, object, group);
					return;
				}
				// World culling dispatches several subtree/geometry jobs. A Process2-only
				// scope misses the remaining objects in those jobs and their worker threads.
				thread_local Traversal current;
				current.process = process;
				current.roots.clear();
				traversal = &current;
				Record(process, object, group);
				traversal = nullptr;
				std::scoped_lock lock(mutex);
				frame.Advance(globals::state->frameCountAtomic.load(std::memory_order_relaxed));
				frame.scene = target;
				frame.frontiers[object] = { frame.roots.size(), current.roots.size() };
				for (auto& root : current.roots)
					frame.roots.push_back(std::move(root));
				for (auto* node = object->parent; node; node = node->parent) {
					if (!frame.branches.insert(node).second || node == target)
						break;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void BeginFrame()
	{
		std::scoped_lock lock(mutex);
		frame.Advance(globals::state->frameCountAtomic.load(std::memory_order_relaxed));
	}

	void RequestCapture()
	{
		requestedFrame.store(globals::state->frameCountAtomic.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	RE::BSTArray<RE::NiPointer<RE::NiAVObject>> Roots(RE::NiAVObject* scene)
	{
		std::scoped_lock lock(mutex);
		if (frame.number != globals::state->frameCountAtomic.load(std::memory_order_relaxed) || frame.scene != scene)
			return {};
		// This frontier covers visited leaves AND pruned subtrees; no main-view
		// visibility result is reused. Native LOD/switch policy remains world-view based.
		RE::BSTArray<RE::NiPointer<RE::NiAVObject>> roots;
		static REL::Relocation<const RE::NiRTTI*> nodeRTTI{ RE::NiNode::Ni_RTTI };
		const auto collect = [&](auto&& self, RE::NiAVObject* object) -> void {
			if (object->GetAppCulled())
				return;
			if (const auto found = frame.frontiers.find(object); found != frame.frontiers.end()) {
				const auto& frontier = found->second;
				for (size_t i = 0; i < frontier.count; ++i)
					roots.push_back(frame.roots[frontier.first + i]);
			} else if (auto* node = object->AsNode(); node && frame.branches.contains(object) &&
													  (object == scene || node->GetRTTI() == nodeRTTI.get())) {
				// Only unpack plain containers. Switch/LOD/ordered nodes must execute
				// their native visibility/grouping logic if their subtree was not recorded.
				for (const auto& child : node->GetChildren())
					if (child)
						self(self, child.get());
			} else {
				roots.push_back(RE::NiPointer<RE::NiAVObject>(object));
			}
		};
		collect(collect, scene);
		return roots;
	}

	bool CopyPasses(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t flags, std::deque<RE::BSRenderPass>& output)
	{
		std::scoped_lock lock(mutex);
		if (frame.number != globals::state->frameCountAtomic.load(std::memory_order_relaxed))
			return false;
		const auto found = frame.materials.find(geometry);
		if (found == frame.materials.end() || found->second.property != property || found->second.flags != flags)
			return false;
		const auto& material = found->second;
		for (size_t i = 0; i < material.count; ++i)
			output.push_back(frame.passes[material.first + i]);
		return true;
	}

	void RememberPasses(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t flags, RE::BSShaderAccumulator* accumulator, const RE::BSShaderProperty::RenderPassArray* passes)
	{
		if (REL::Module::IsVR() || !passes || !accumulator || IsRendering() || !Requested() || accumulator->GetRuntimeData()->renderMode != RE::BSShaderAccumulator::RENDER_MODE::kNormal)
			return;
		std::scoped_lock lock(mutex);
		if (accumulator->camera != RE::Main::WorldRootCamera())
			return;
		frame.Advance(globals::state->frameCountAtomic.load(std::memory_order_relaxed));
		auto& value = frame.materials[geometry];
		value.property = property;
		value.flags = flags;
		value.first = frame.passes.size();
		for (auto* pass = passes->head; pass; pass = pass->next)
			if (pass->shader->shaderType.get() == RE::BSShader::Type::Lighting)
				frame.passes.push_back(*pass);
		value.count = frame.passes.size() - value.first;
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR())
			return;
		// Derived cullers can inherit these entry points through their own vtables.
		// Intercept the functions so those world traversals are recorded as well.
		REL::Relocation<std::uintptr_t*> vtable{ RE::VTABLE_BSCullingProcess[0] };
		stl::detour_thunk<ProcessObject>(vtable.get()[0x16]);
	}
}
