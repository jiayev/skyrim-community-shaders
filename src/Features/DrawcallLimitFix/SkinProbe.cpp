#include "SkinProbe.h"

#include "ConstantMirror.h"
#include "LightingDescriptors.h"
#include "SceneStore.h"
#include "Switches.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Deferred.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		struct Observed
		{
			std::string rtti;
			std::uint32_t partitions = 0;
			std::uint32_t bones = 0;
			std::uint32_t numMatrices = 0;
			std::uint32_t lodBytes = 0;  // the first four partitions' LOD bytes, packed
			std::uint32_t maxBonesPerVertex = 0;
			bool rendererDataIsPartition = false;
			std::uint32_t geometryType = 0;
			std::uint32_t technique = 0;
			bool tracked = false;
			std::uint32_t hint = 0;

			std::string Key() const
			{
				return fmt::format("{} p{} b{} m{} lod{:08X} bpv{} rd{} type{} tech{} {} hint{}", rtti, partitions, bones, numMatrices, lodBytes, maxBonesPerVertex,
					rendererDataIsPartition ? 1 : 0, geometryType, technique, tracked ? "tracked" : "untracked", hint);
			}
		};

		std::uint64_t HashBytes(const void* a_data, std::size_t a_bytes)
		{
			return ankerl::unordered_dense::detail::wyhash::hash(a_data, a_bytes);
		}
	}

	struct SkinProbe::Impl
	{
		ankerl::unordered_dense::map<std::string, std::uint32_t> observed;
		ankerl::unordered_dense::map<std::string, std::uint32_t> names;  // "name (tracked|untracked)"
		struct Pose
		{
			std::uint64_t hash = 0;
			std::uint32_t seen = 0;
			std::uint32_t changed = 0;
		};
		ankerl::unordered_dense::map<const RE::NiSkinInstance*, Pose> poses;
		std::uint32_t draws = 0;
		std::uint32_t trackedDraws = 0;
		std::uint32_t pivotChecked = 0;
		std::uint32_t pivotMismatched = 0;
		std::uint32_t previousPivotMismatched = 0;
		float lastPivot[3]{}, lastPosAdjust[3]{};
		bool loggedSample = false;
	};

	bool SkinProbe::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_SKIN_PROBE");
		return enabled;
	}

	SkinProbe& SkinProbe::Get()
	{
		static SkinProbe probe;
		return probe;
	}

	SkinProbe::SkinProbe() :
		impl(std::make_unique<Impl>()) {}

	SkinProbe::~SkinProbe() = default;

	void SkinProbe::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t)
	{
		if (!a_pass || !a_pass->geometry || !globals::deferred->deferredPass)
			return;
		auto* geometry = a_pass->geometry;
		auto& data = geometry->GetGeometryRuntimeData();
		auto* skin = data.skinInstance.get();
		if (!skin)
			return;
		const auto& store = SceneStore::Get();
		Observed seen{};
		seen.rtti = skin->GetRTTI() && skin->GetRTTI()->GetName() ? skin->GetRTTI()->GetName() : "?";
		auto* skinData = skin->skinData.get();
		auto* partition = skin->skinPartition.get();
		seen.bones = skinData ? skinData->GetBoneCount() : 0;
		seen.numMatrices = skin->numMatrices;
		seen.partitions = partition ? partition->numPartitions : 0;
		if (partition) {
			for (std::uint32_t i = 0; i < std::min<std::uint32_t>(partition->numPartitions, 4); ++i) {
				const auto& p = partition->partitions[i];
				seen.lodBytes |= static_cast<std::uint32_t>(p.pad42 & 0xFF) << (8 * i);
				seen.maxBonesPerVertex = std::max<std::uint32_t>(seen.maxBonesPerVertex, p.bonesPerVertex);
			}
			seen.rendererDataIsPartition = partition->numPartitions > 0 && data.rendererData == partition->partitions[0].buffData;
		}
		seen.geometryType = static_cast<std::uint32_t>(geometry->GetType().get());
		seen.technique = (PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f;
		seen.tracked = store.IsTracked(geometry);
		seen.hint = a_pass->accumulationHint;
		++impl->observed[seen.Key()];
		++impl->draws;
		if (seen.tracked)
			++impl->trackedDraws;
		++impl->names[fmt::format("{} ({})", geometry->name.c_str() ? geometry->name.c_str() : "?", seen.tracked ? "tracked" : "untracked")];

		// Static poses: the palette as it stands at SetupGeometry is last frame's (the setter refreshes it
		// after this hook), which is fine for counting how often it changes between frames.
		if (skin->boneMatrices && skin->numMatrices) {
			auto& pose = impl->poses[skin];
			const std::uint64_t hash = HashBytes(skin->boneMatrices, std::size_t(skin->numMatrices) * 48);
			if (pose.seen && hash != pose.hash)
				++pose.changed;
			pose.hash = hash;
			++pose.seen;
		}

		// BonesPivot (VS_PerFrame c40) and PreviousBonesPivot (c41) against the shadow state's posAdjust
		// and previousPosAdjust: the mirror of the engine's per-frame buffer holds what the draw reads.
		auto* perFrame = *globals::game::perFrame.get();
		if (perFrame) {
			auto& mirror = ConstantMirror::Get();
			mirror.Watch(perFrame);
			const auto contents = mirror.Contents(perFrame);
			if (contents.size() >= 42 * 16) {
				const auto* floats = reinterpret_cast<const float*>(contents.data());
				auto& state = globals::game::shadowState->GetRuntimeData();
				const auto eye = state.posAdjust.getEye();
				const auto previousEye = state.previousPosAdjust.getEye();
				++impl->pivotChecked;
				const bool same = floats[160] == eye.x && floats[161] == eye.y && floats[162] == eye.z;
				const bool samePrevious = floats[164] == previousEye.x && floats[165] == previousEye.y && floats[166] == previousEye.z;
				if (!same)
					++impl->pivotMismatched;
				if (!samePrevious)
					++impl->previousPivotMismatched;
				impl->lastPivot[0] = floats[160];
				impl->lastPivot[1] = floats[161];
				impl->lastPivot[2] = floats[162];
				impl->lastPosAdjust[0] = eye.x;
				impl->lastPosAdjust[1] = eye.y;
				impl->lastPosAdjust[2] = eye.z;
			}
		}
	}

	void SkinProbe::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if ((a_frame % a_interval) != 0)
			return;
		const double frames = static_cast<double>(std::max(1u, a_interval));
		std::uint32_t staticPoses = 0, movingPoses = 0;
		for (const auto& [skin, pose] : impl->poses) {
			if (pose.seen < 2)
				continue;
			(pose.changed == 0 ? staticPoses : movingPoses)++;
		}
		logger::info("[DCLF] skin probe: {:.1f} native skinned draws a frame ({:.1f} tracked); {} skin instances seen, {} never changed their palette, {} did; BonesPivot against posAdjust: {} checked, {} differ (previous {} differ); last pivot ({:.2f} {:.2f} {:.2f}) posAdjust ({:.2f} {:.2f} {:.2f})",
			impl->draws / frames, impl->trackedDraws / frames, impl->poses.size(), staticPoses, movingPoses, impl->pivotChecked, impl->pivotMismatched,
			impl->previousPivotMismatched, impl->lastPivot[0], impl->lastPivot[1], impl->lastPivot[2], impl->lastPosAdjust[0], impl->lastPosAdjust[1], impl->lastPosAdjust[2]);
		std::vector<std::pair<std::string, std::uint32_t>> sorted(impl->observed.begin(), impl->observed.end());
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		for (const auto& [key, count] : sorted)
			logger::info("[DCLF] skin probe: {:.1f}/frame {}", count / frames, key);
		std::vector<std::pair<std::string, std::uint32_t>> names(impl->names.begin(), impl->names.end());
		std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string top;
		for (std::size_t i = 0; i < names.size() && i < 16; ++i)
			top += fmt::format(" '{}'={:.1f}", names[i].first, names[i].second / frames);
		logger::info("[DCLF] skin probe: {} distinct names; top:{}", names.size(), top);
		impl->observed.clear();
		impl->names.clear();
		impl->draws = impl->trackedDraws = impl->pivotChecked = impl->pivotMismatched = impl->previousPivotMismatched = 0;
	}
}
