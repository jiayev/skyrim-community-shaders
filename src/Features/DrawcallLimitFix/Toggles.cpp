#include "Toggles.h"

#include "Switches.h"

namespace DCLF
{
	namespace
	{
		// Bit positions of the packed active word.
		enum : std::uint32_t
		{
			kHybrid = 0,
			kOwnership,
			kCullMode,  // two bits
			kCullTracked = 4,
			kSkinned,
			kTrees,
			kDecals,
			kProjectedUv,
			kMtLand,
			kShadows,
			kShadowOwnership,
			kDebugView,
			kHybridNoSkip,
			kOnlyEligible,
			kNoZPrepass,
			kSwitchNodes,
			kSkinPartitions,
			kActors,
			kFading,
			kLodCrossfade,
			kSkipSunAccumulation,
			kExcludeSunEntries,
			kSkyOcclusion,
			kExcludePrimaryEntries,
		};

		// Defaults: the configuration every gate run of this work used. An unset switch takes the default;
		// an explicit value overrides it ("0" / "off" turns an on-by-default feature off).
		bool OnUnlessOff(const char* a_name)
		{
			const auto value = SwitchValue(a_name);
			return value.empty() || value == "1";
		}

		bool StaticUnlessOff(const char* a_name)
		{
			const auto value = SwitchValue(a_name);
			return value.empty() || value == "static";
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			set.hybrid = OnUnlessOff("CS_DCLF_HYBRID");
			set.ownership = StaticUnlessOff("CS_DCLF_OWNERSHIP");
			const auto cull = SwitchValue("CS_DCLF_CULL");
			set.cullMode = (cull.empty() || cull == "occlusion") ? 2 : cull == "frustum" ? 1 : 0;
			set.cullTracked = SwitchValue("CS_DCLF_CULL_INPUT") == "tracked";
			set.skinned = OnUnlessOff("CS_DCLF_SKINNED");
			set.trees = OnUnlessOff("CS_DCLF_TREES");
			set.decals = OnUnlessOff("CS_DCLF_DECALS");
			set.projectedUv = OnUnlessOff("CS_DCLF_PROJECTED_UV");
			set.mtLand = OnUnlessOff("CS_DCLF_MTLAND");
			set.switchNodes = OnUnlessOff("CS_DCLF_SWITCH_NODES");
			set.skinPartitions = OnUnlessOff("CS_DCLF_SKIN_PARTITIONS");
			set.actors = OnUnlessOff("CS_DCLF_ACTORS");
			set.fading = OnUnlessOff("CS_DCLF_FADING");
			set.lodCrossfade = OnUnlessOff("CS_DCLF_LOD_CROSSFADE");
			set.shadows = OnUnlessOff("CS_DCLF_SHADOWS");
			set.shadowOwnership = StaticUnlessOff("CS_DCLF_SHADOW_OWNERSHIP");
			set.skipSunAccumulation = OnUnlessOff("CS_DCLF_SUN_SKIP");
			// `probe` runs the exclusion dry (SunAccumulation), which needs the toggle on.
			set.excludeSunEntries = OnUnlessOff("CS_DCLF_SUN_EXCLUDE") || SwitchValue("CS_DCLF_SUN_EXCLUDE") == "probe";
			set.skyOcclusion = OnUnlessOff("CS_DCLF_SKYLIGHT");
			// `probe` is PrimaryCull's census, which removes nothing (and so leaves the cut off).
			set.excludePrimaryEntries = OnUnlessOff("CS_DCLF_PRIMARY_EXCLUDE");
			set.debugView = SwitchEnabled("CS_DCLF_DEBUG_VIEW");
			set.hybridNoSkip = SwitchEnabled("CS_DCLF_HYBRID_NOSKIP");
			set.onlyEligible = SwitchEnabled("CS_DCLF_ONLY_ELIGIBLE");
			set.noZPrepass = SwitchEnabled("CS_DCLF_NO_ZPREPASS");
			return set;
		}

		/** @brief The combinations that cannot hold: ownership without the path that draws into the frame. */
		ToggleSet Normalised(ToggleSet a_set)
		{
			a_set.ownership = a_set.ownership && a_set.hybrid;
			a_set.shadowOwnership = a_set.shadowOwnership && a_set.shadows;
			a_set.skipSunAccumulation = a_set.skipSunAccumulation && a_set.shadowOwnership;
			a_set.excludeSunEntries = a_set.excludeSunEntries && a_set.skipSunAccumulation;
			a_set.skyOcclusion = a_set.skyOcclusion && a_set.shadows;
			a_set.excludePrimaryEntries = a_set.excludePrimaryEntries && a_set.excludeSunEntries && a_set.ownership && !a_set.cullTracked;
			a_set.skinPartitions = a_set.skinPartitions && a_set.skinned;
			return a_set;
		}

		bool EntersClassification(const ToggleSet& a, const ToggleSet& b)
		{
			return a.hybrid != b.hybrid || a.cullTracked != b.cullTracked || a.skinned != b.skinned || a.trees != b.trees ||
			       a.decals != b.decals || a.projectedUv != b.projectedUv || a.mtLand != b.mtLand || a.switchNodes != b.switchNodes ||
			       a.skinPartitions != b.skinPartitions || a.actors != b.actors || a.fading != b.fading ||
			       a.lodCrossfade != b.lodCrossfade || a.skyOcclusion != b.skyOcclusion;
		}
	}

	Toggles& Toggles::Get()
	{
		static Toggles toggles;
		return toggles;
	}

	Toggles::Toggles() :
		requested(FromSwitches())
	{
		active.store(Pack(Normalised(requested)), std::memory_order_relaxed);
	}

	std::uint32_t Toggles::Pack(const ToggleSet& s)
	{
		std::uint32_t bits = 0;
		auto put = [&](std::uint32_t a_bit, bool a_on) { bits |= (a_on ? 1u : 0u) << a_bit; };
		put(kHybrid, s.hybrid);
		put(kOwnership, s.ownership);
		bits |= (s.cullMode & 3u) << kCullMode;
		put(kCullTracked, s.cullTracked);
		put(kSkinned, s.skinned);
		put(kTrees, s.trees);
		put(kDecals, s.decals);
		put(kProjectedUv, s.projectedUv);
		put(kMtLand, s.mtLand);
		put(kShadows, s.shadows);
		put(kShadowOwnership, s.shadowOwnership);
		put(kDebugView, s.debugView);
		put(kHybridNoSkip, s.hybridNoSkip);
		put(kOnlyEligible, s.onlyEligible);
		put(kNoZPrepass, s.noZPrepass);
		put(kSwitchNodes, s.switchNodes);
		put(kSkinPartitions, s.skinPartitions);
		put(kActors, s.actors);
		put(kFading, s.fading);
		put(kLodCrossfade, s.lodCrossfade);
		put(kSkipSunAccumulation, s.skipSunAccumulation);
		put(kExcludeSunEntries, s.excludeSunEntries);
		put(kSkyOcclusion, s.skyOcclusion);
		put(kExcludePrimaryEntries, s.excludePrimaryEntries);
		return bits;
	}

	ToggleSet Toggles::Unpack(std::uint32_t a_bits)
	{
		ToggleSet s;
		auto get = [&](std::uint32_t a_bit) { return ((a_bits >> a_bit) & 1u) != 0; };
		s.hybrid = get(kHybrid);
		s.ownership = get(kOwnership);
		s.cullMode = static_cast<std::uint8_t>((a_bits >> kCullMode) & 3u);
		s.cullTracked = get(kCullTracked);
		s.skinned = get(kSkinned);
		s.trees = get(kTrees);
		s.decals = get(kDecals);
		s.projectedUv = get(kProjectedUv);
		s.mtLand = get(kMtLand);
		s.shadows = get(kShadows);
		s.shadowOwnership = get(kShadowOwnership);
		s.debugView = get(kDebugView);
		s.hybridNoSkip = get(kHybridNoSkip);
		s.onlyEligible = get(kOnlyEligible);
		s.noZPrepass = get(kNoZPrepass);
		s.switchNodes = get(kSwitchNodes);
		s.skinPartitions = get(kSkinPartitions);
		s.actors = get(kActors);
		s.fading = get(kFading);
		s.lodCrossfade = get(kLodCrossfade);
		s.skipSunAccumulation = get(kSkipSunAccumulation);
		s.excludeSunEntries = get(kExcludeSunEntries);
		s.skyOcclusion = get(kSkyOcclusion);
		s.excludePrimaryEntries = get(kExcludePrimaryEntries);
		return s;
	}

	bool Toggles::BeginFrame()
	{
		const auto next = Normalised(requested);
		const auto current = Active();
		if (next == current)
			return false;
		active.store(Pack(next), std::memory_order_relaxed);
		generation.fetch_add(1, std::memory_order_relaxed);
		return EntersClassification(current, next);
	}
}
