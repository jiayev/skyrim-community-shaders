#include "Toggles.h"

namespace DCLF
{
	namespace
	{
		using S = ToggleSet;
		constexpr std::string_view kOn = "1";

		constexpr ToggleInfo kToggles[] = {
			{ &S::skinned, Switch::Skinned, kOn, "skinned", "Object classes", "Skinned (CS_DCLF_SKINNED)", nullptr, true, {} },
			{ &S::trees, Switch::Trees, kOn, "trees", {}, "Trees (CS_DCLF_TREES)", nullptr, true, {} },
			{ &S::decals, Switch::Decals, kOn, "decals", {}, "Decals (CS_DCLF_DECALS)", nullptr, true, {} },
			{ &S::layers, Switch::Layers, kOn, "multi-index layers", {}, "Multi-index shapes: cave walls with an ice or snow layer (CS_DCLF_LAYERS)", nullptr, true, { &S::decals } },
			{ &S::projectedUv, Switch::ProjectedUv, kOn, "projected", {}, "Projected UV (CS_DCLF_PROJECTED_UV)", nullptr, true, {} },
			{ &S::mtLand, Switch::MtLand, kOn, "terrain", {}, "Terrain (CS_DCLF_MTLAND)", nullptr, true, {} },
			{ &S::tbTerrain, Switch::TbTerrain, kOn, "terrain under TB", {}, "Terrain under Terrain Blending (CS_DCLF_TB_TERRAIN)",
				"With Terrain Blending on, DCLF draws the terrain as that feature does: its near blocks' depth into its terrain depth and, offset, into the main depth, and its colour blended after the opaque and decal draws, then the meshes it redraws after the terrain.",
				true, { &S::mtLand } },
			{ &S::forward, Switch::Forward, kOn, "forward", {}, "Blended objects: the forward epoch (CS_DCLF_FORWARD)",
				"T7d: DCLF draws blended Lighting geometry itself, sorted back to front on the GPU, just before the engine's own blended draws (effects, particles). On by default (validated w174/w175).",
				true, {} },
			{ &S::effects, Switch::Effects, kOn, "effects", {}, "Effect meshes: clouds, glows, mist (CS_DCLF_EFFECTS)",
				"T7d: DCLF draws blended effect-shader meshes in the forward epoch, sorted with the blended Lighting objects, and (with Decals) effect decals in its decal groups, skinned ones (with Skinned) through the skin palette. On by default (validated w174/w175; decals and skinned effects not yet).",
				true, { &S::forward } },
			{ &S::refraction, Switch::Refraction, kOn, "refraction", {}, "Refraction: heat haze, invisibility (CS_DCLF_REFRACTION)",
				"T7d: DCLF draws refraction geometry's normals into the refraction normals the engine's refraction effect warps the frame by, in the forward epoch. On by default (validated w174/w175).",
				true, { &S::forward } },
			{ &S::particles, Switch::Particles, kOn, "particles", {}, "Particle systems: smoke, flames, glows (CS_DCLF_PARTICLES)",
				"T7e: DCLF draws effect-shader particle systems in the forward epoch, sorted with the other blended objects, their quads built from snapshots taken at the engine's particle update. Off by default until validated.",
				true, { &S::effects }, true },
			{ &S::switchNodes, Switch::SwitchNodes, kOn, "switch nodes", {}, "Under switch nodes: trees, harvestables (CS_DCLF_SWITCH_NODES)", nullptr, true, {} },
			{ &S::lodObjects, Switch::LodObjects, kOn, "object LOD", {}, "Object LOD: distant objects (CS_DCLF_LOD_OBJECTS)", nullptr, true, {} },
			{ &S::lodTerrain, Switch::LodTerrain, kOn, "terrain LOD", {}, "Terrain LOD: distant land (CS_DCLF_LOD_TERRAIN)", nullptr, true, {} },
			{ &S::lodTrees, Switch::LodTrees, kOn, "tree LOD", {}, "Tree LOD: distant trees (CS_DCLF_LOD_TREES)", nullptr, false, {} },
			{ &S::skinPartitions, Switch::SkinPartitions, kOn, "skin partitions", {}, "Skins of several partitions: LOD trees, actor bodies (CS_DCLF_SKIN_PARTITIONS)", nullptr, true, { &S::skinned } },
			{ &S::actors, Switch::Actors, kOn, "actors", {}, "Actors (CS_DCLF_ACTORS)", nullptr, true, {} },
			{ &S::fading, Switch::Fading, kOn, "fading", {}, "Fading objects: the screen-door fade (CS_DCLF_FADING)", nullptr, true, {} },
			{ &S::lodCrossfade, Switch::LodCrossfade, kOn, "LOD cross-fade", {}, "LOD cross-fades: keep the object, leave the copy native (CS_DCLF_LOD_CROSSFADE)", nullptr, true, {} },
			{ &S::shadows, Switch::Shadows, kOn, "shadows", "Shadow views", "Draw the shadow views (CS_DCLF_SHADOWS)", nullptr, false, {} },
			{ &S::skipSunAccumulation, Switch::SunSkip, kOn, "skip sun accumulation", {}, "Skip the engine's sun shadow culling and registration (CS_DCLF_SUN_SKIP)",
				"The engine stops building sun shadow passes for the casters DCLF draws; it still sets their shadow bits for the main pass.", false, { &S::shadows } },
			{ &S::excludeSunEntries, Switch::SunExclude, kOn, "exclude sun entries", {}, "Take DCLF's objects out of the engine's sun culls (CS_DCLF_SUN_EXCLUDE)",
				"The sun's cascade culls skip every reference whose shadows DCLF draws entirely; DCLF sets those objects' sun shadow bits for the main pass.", false, { &S::skipSunAccumulation } },
			// `probe` is PrimaryCull's census, which removes nothing (and so leaves the cut off).
			{ &S::excludePrimaryEntries, Switch::PrimaryExclude, kOn, "exclude primary entries", {}, "Take DCLF's objects out of the engine's main camera cull (CS_DCLF_PRIMARY_EXCLUDE)",
				"The main camera's cull and registration skip every reference DCLF draws entirely; DCLF builds their main passes itself and runs their fade updates.", false,
				{ &S::excludeSunEntries } },
			{ &S::skyOcclusion, Switch::Skylight, kOn, "sky occlusion", {}, "Draw Skylighting's occlusion map (CS_DCLF_SKYLIGHT)",
				"With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
			{ &S::precipitationOcclusion, Switch::Precipitation, kOn, "precipitation occlusion", {}, "Draw the precipitation occlusion mask (CS_DCLF_PRECIPITATION)",
				"With Skylighting loaded, DCLF draws the precipitation occlusion mask (where rain and snow stop) from its own tables on the GPU, and the engine no longer culls or registers the scene for it.", true,
				{ &S::shadows } },
			{ &S::reflections, Switch::Reflections, kOn, "reflections", "Reflections", "Draw the water reflection's distant land, objects and trees (CS_DCLF_REFLECTIONS)",
				"DCLF draws the LOD the water's cube map reflects from its own tables on the GPU; the engine draws the sky, and any LOD DCLF cannot draw yet.", false, {} },
			{ &S::firstPerson, Switch::FirstPerson, kOn, "first person", "First person", "Draw the first-person arms and weapons (CS_DCLF_FIRST_PERSON)",
				"T7e: DCLF draws the first-person skeleton's geometry (arms, weapons, shield, magic hands) as a view of its scene, with the first-person camera, in the engine's depth and first-person passes. Off by default until validated.",
				true, { &S::skinned, &S::actors }, true },
		};

		// The packed word: cullMode in the low two bits, then each flag in kToggles order.
		constexpr std::uint32_t kFirstFlagBit = 2;
		static_assert(kFirstFlagBit + std::size(kToggles) <= 32);

		bool NeedsMet(const ToggleInfo& a_info, const ToggleSet& a_set)
		{
			for (const auto need : a_info.needs)
				if (need && !(a_set.*need))
					return false;
			return true;
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			const auto& cull = SwitchValue(Switch::Cull);
			set.cullMode = (cull.empty() || cull == "occlusion") ? 2 : cull == "frustum" ? 1 : 0;
			for (const auto& toggle : kToggles) {
				const auto& value = SwitchValue(toggle.seed);
				set.*toggle.member = value.empty() ? !toggle.offByDefault : value == toggle.onValue;
			}
			return set;
		}

		/** @brief The combinations that cannot hold: each flag off while one it needs is off (kToggles order resolves chains). */
		ToggleSet Normalised(ToggleSet a_set)
		{
			for (const auto& toggle : kToggles)
				a_set.*toggle.member = a_set.*toggle.member && NeedsMet(toggle, a_set);
			return a_set;
		}

		bool EntersClassification(const ToggleSet& a, const ToggleSet& b)
		{
			for (const auto& toggle : kToggles)
				if (toggle.entersClassification && a.*toggle.member != b.*toggle.member)
					return true;
			return false;
		}
	}

	std::span<const ToggleInfo> ToggleTable()
	{
		return kToggles;
	}

	bool Toggles::Editable(const ToggleInfo& a_info) const
	{
		return NeedsMet(a_info, Normalised(requested));
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

	std::uint32_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		return bits;
	}

	ToggleSet Toggles::Unpack(std::uint32_t a_bits)
	{
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(a_bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((a_bits >> (kFirstFlagBit + i)) & 1u) != 0;
		return set;
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
