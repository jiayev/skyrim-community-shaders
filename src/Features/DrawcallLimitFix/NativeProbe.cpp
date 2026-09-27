#include "NativeProbe.h"

#include "LightingDescriptors.h"
#include "SceneStore.h"
#include "Switches.h"

#include <algorithm>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Deferred.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		const char* NameOf(const RE::NiObjectNET* a_object)
		{
			return a_object && a_object->name.c_str() ? a_object->name.c_str() : "?";
		}

		const char* RttiOf(const RE::NiObject* a_object)
		{
			const auto* rtti = a_object ? a_object->GetRTTI() : nullptr;
			return rtti && rtti->GetName() ? rtti->GetName() : "?";
		}
	}

	struct NativeProbe::Impl
	{
		ankerl::unordered_dense::map<std::string, std::uint32_t> keys;
		ankerl::unordered_dense::map<std::string, std::uint32_t> names;   // "form | name | path"
		ankerl::unordered_dense::map<std::string, std::uint32_t> totals;  // by form type
		std::uint32_t draws = 0;
		std::uint32_t samples = 0;
	};

	bool NativeProbe::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_NATIVE_PROBE");
		return enabled;
	}

	NativeProbe& NativeProbe::Get()
	{
		static NativeProbe probe;
		return probe;
	}

	NativeProbe::NativeProbe() :
		impl(std::make_unique<Impl>()) {}

	NativeProbe::~NativeProbe() = default;

	void NativeProbe::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t)
	{
		if (!a_pass || !a_pass->geometry || !globals::deferred->deferredPass)
			return;
		auto* geometry = a_pass->geometry;
		const auto& store = SceneStore::Get();

		// The owning reference, and the notable node classes between it and the leaf.
		std::string form = "no-ref";
		std::string path;
		const RE::NiAVObject* top = geometry;
		for (const RE::NiAVObject* object = geometry->parent; object; object = object->parent) {
			top = object;
			const char* rtti = RttiOf(object);
			if (std::string_view(rtti) != "NiNode" && path.size() < 96)
				path += fmt::format("{}{}", path.empty() ? "" : "<", rtti);
			if (auto* ref = object->GetUserData()) {
				auto* base = ref->GetBaseObject();
				form = fmt::format("{}{}", base ? RE::FormTypeToString(base->GetFormType()) : "?", ref->IsActor() ? "(actor)" : "");
				break;
			}
		}
		// Where the reference hangs: the root-most two ancestors.
		std::string root;
		{
			const RE::NiAVObject* chain[64];
			std::uint32_t depth = 0;
			for (const RE::NiAVObject* object = geometry; object && depth < 64; object = object->parent)
				chain[depth++] = object;
			for (std::uint32_t i = depth; i-- > 0 && depth - i <= 3;)
				root += fmt::format("{}{}:{}", root.empty() ? "" : "/", RttiOf(chain[i]), NameOf(chain[i]));
		}

		const bool tracked = store.IsTracked(geometry);
		bool accumulate = false;
		const Ineligible reason = tracked ? store.ReasonThisFrame(geometry, &accumulate) : Ineligible::None;
		const char* verdict = !tracked ? "untracked" : kIneligibleNames[static_cast<std::size_t>(reason)].data();

		auto& data = geometry->GetGeometryRuntimeData();
		std::string skin = "-";
		if (auto* instance = data.skinInstance.get()) {
			auto* partition = instance->skinPartition.get();
			auto* skinData = instance->skinData.get();
			std::string lods;
			if (partition)
				for (std::uint32_t i = 0; i < std::min<std::uint32_t>(partition->numPartitions, 8); ++i)
					lods += fmt::format("{}", partition->partitions[i].pad42 & 0xFF);
			skin = fmt::format("{} p{} lod[{}] b{} mode{}{}", RttiOf(instance), partition ? partition->numPartitions : 0, lods, skinData ? skinData->GetBoneCount() : 0,
				a_pass->LODMode.index, a_pass->LODMode.singleLevel ? "s" : "");
		}
		const std::uint32_t technique = (PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f;

		const std::string key = fmt::format("{} | {} | tech{} type{} hint{} | {}", form, verdict, technique,
			static_cast<std::uint32_t>(geometry->GetType().get()), a_pass->accumulationHint, skin);
		// The first time a key is seen in an interval: the whole ancestor chain, and for a skin whether its
		// partitions share their buffers.
		if (!impl->keys.contains(key) && impl->samples < 60) {
			++impl->samples;
			std::string chain;
			std::uint32_t depth = 0;
			for (const RE::NiAVObject* object = geometry; object && depth < 12; object = object->parent, ++depth)
				chain += fmt::format("{}{}:'{}'{}{}", chain.empty() ? "" : " < ", RttiOf(object), NameOf(object), object->GetUserData() ? "(ref)" : "",
					object->GetFlags().any(RE::NiAVObject::Flag::kHidden) ? "(HIDDEN)" : "");
			std::string buffers;
			// Each switch node on the path: its index, where the path's child sits, and the revisions.
			{
				const RE::NiAVObject* child = geometry;
				for (auto* node = geometry->parent; node; child = node, node = node->parent) {
					auto* switchNode = node->AsSwitchNode();
					if (!switchNode)
						continue;
					const auto& children = switchNode->GetChildren();
					std::int32_t at = -1;
					for (std::uint32_t i = 0; i < children.capacity(); ++i)
						if (children[static_cast<std::uint16_t>(i)].get() == child)
							at = static_cast<std::int32_t>(i);
					SceneStore::SwitchState state;
					if (!SceneStore::ReadSwitch(*switchNode, state))
						continue;
					buffers += fmt::format(" [switch '{}': index {} child-at {} size {} cap {} flags {:x} rev {} childRev {} revs cap {} selects {}]", NameOf(switchNode),
						state.index, at, children.size(), children.capacity(), state.flags, state.revID,
						state.childRevID && state.index >= 0 && static_cast<std::uint32_t>(state.index) < state.childRevCapacity ? state.childRevID[state.index] : ~0u,
						state.childRevCapacity, SceneStore::SwitchSelects(*switchNode, child));
				}
			}
			if (auto* instance = data.skinInstance.get(); instance && instance->skinPartition) {
				auto* partition = instance->skinPartition.get();
				for (std::uint32_t i = 0; i < std::min<std::uint32_t>(partition->numPartitions, 4); ++i) {
					const auto& p = partition->partitions[i];
					buffers += fmt::format(" [{}: tri{} bones{} vb{} ib{} rd{}]", i, p.triangles, p.numBones,
						p.buffData ? fmt::format("{}", fmt::ptr(p.buffData->vertexBuffer)) : "-", p.buffData ? fmt::format("{}", fmt::ptr(p.buffData->indexBuffer)) : "-",
						p.buffData == data.rendererData ? "=geom" : "");
				}
				if (auto* dismember = skyrim_cast<RE::BSDismemberSkinInstance*>(instance)) {
					buffers += " dismember:";
					for (std::int32_t i = 0; i < dismember->GetRuntimeData().numPartitions && i < 8; ++i)
						buffers += fmt::format(" {}/{}", dismember->GetRuntimeData().partitions[i].slot, dismember->GetRuntimeData().partitions[i].editorVisible ? 1 : 0);
				}
			}
			logger::info("[DCLF] native probe sample: {} || {}{}", key, chain, buffers);
		}
		++impl->keys[key];
		++impl->totals[form];
		++impl->names[fmt::format("{} | {} | {} | {}", form, NameOf(geometry), path, root)];
		++impl->draws;
		(void)top;
	}

	void NativeProbe::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if ((a_frame % a_interval) != 0)
			return;
		const double frames = static_cast<double>(std::max(1u, a_interval));
		auto sorted = [](const auto& a_map) {
			std::vector<std::pair<std::string, std::uint32_t>> out(a_map.begin(), a_map.end());
			std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			return out;
		};
		std::string totals;
		for (const auto& [form, count] : sorted(impl->totals))
			totals += fmt::format(" {}={:.1f}", form, count / frames);
		logger::info("[DCLF] native probe: {:.1f} native lighting draws a frame; by form:{}", impl->draws / frames, totals);
		std::uint32_t lines = 0;
		for (const auto& [key, count] : sorted(impl->keys)) {
			if (++lines > 40)
				break;
			logger::info("[DCLF] native probe: {:.1f}/frame {}", count / frames, key);
		}
		lines = 0;
		for (const auto& [name, count] : sorted(impl->names)) {
			if (++lines > 40)
				break;
			logger::info("[DCLF] native probe name: {:.1f}/frame {}", count / frames, name);
		}
		impl->keys.clear();
		impl->names.clear();
		impl->totals.clear();
		impl->draws = 0;
		impl->samples = 0;
	}
}
