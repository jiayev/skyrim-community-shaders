// Switches: the registry (name, kind, description per switch), the switches file and environment read once, the startup summary.
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "Utils/SwitchFile.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <deque>
#include <string_view>
#include <vector>

namespace DCLF
{
	namespace
	{
		enum class Kind : std::uint8_t
		{
			Feature,
			Parity,
			Diagnostic,
			Test,
			Other,
		};

		// Which values narrow a feature, as its reader interprets them.
		enum class Reduced : std::uint8_t
		{
			Never,       // not a feature
			UnlessOne,   // on when unset or "1"
			Zero,        // off only with "0"
			Cull,        // "off" and "frustum" narrow it
		};

		struct Entry
		{
			Switch id;
			std::string_view name;
			Kind kind;
			Reduced reduced;
			bool environmentOnly;
			std::string_view description;
		};

		using enum Switch;
		constexpr auto F = Kind::Feature, P = Kind::Parity, D = Kind::Diagnostic, T = Kind::Test, O = Kind::Other;
		// A feature's Api::SwitchKind is one of these (RegisterSwitch casts it).
		static_assert(static_cast<Kind>(Api::SwitchKind::Feature) == F && static_cast<Kind>(Api::SwitchKind::Parity) == P &&
					  static_cast<Kind>(Api::SwitchKind::Diagnostic) == D && static_cast<Kind>(Api::SwitchKind::Test) == T && static_cast<Kind>(Api::SwitchKind::Other) == O);
		constexpr auto N = Reduced::Never;

		// One row per switch, in Switch order (checked below).
		constexpr Entry kRegistry[] = {
			{ Dclf, "CS_DCLF", F, Reduced::Zero, false, "0: DCLF does not install" },
			{ AsyncPriority, "CS_DCLF_ASYNC_PRIORITY", F, N, false, "normal: the coordinator and scene threads keep normal priority instead of above normal; noboost: workers are not raised to a ParallelFor caller's priority while they hold its chunks" },
			{ Workers, "CS_DCLF_WORKERS", F, N, false, "the preparation pool's threads (default hardware threads - 2, at least 1)" },
			{ Fanout, "CS_DCLF_FANOUT", F, N, false,
				"the scene pass's evaluation rounds and joins (T6b3e): 0 interleaved on the scene lane (the reference), 1 every entry evaluated then merged, on the lane; 2 (default) evaluated on the preparation pool from 128 entries, merged in order" },
			{ Precompile, "CS_DCLF_PRECOMPILE", F, Reduced::Zero, false, "0: DCLF's SPIR-V programs compile on first use instead of alongside the engine's shaders" },
			{ Cull, "CS_DCLF_CULL", F, Reduced::Cull, false, "off|frustum|occlusion (default): GPU culling, the live toggle's seed" },
			{ Skinned, "CS_DCLF_SKINNED", F, Reduced::UnlessOne, false, "0: skinned objects stay native (live toggle's seed)" },
			{ SkinPartitions, "CS_DCLF_SKIN_PARTITIONS", F, Reduced::UnlessOne, false, "0: skins of several partitions stay native (live toggle's seed)" },
			{ Actors, "CS_DCLF_ACTORS", F, Reduced::UnlessOne, false, "0: actors stay native (live toggle's seed)" },
			{ Decals, "CS_DCLF_DECALS", F, Reduced::UnlessOne, false, "0: decals stay native (live toggle's seed)" },
			{ Layers, "CS_DCLF_LAYERS", F, Reduced::UnlessOne, false, "0: multi-index shapes' main passes stay native (live toggle's seed)" },
			{ MtLand, "CS_DCLF_MTLAND", F, Reduced::UnlessOne, false, "0: terrain stays native (live toggle's seed)" },
			{ TbTerrain, "CS_DCLF_TB_TERRAIN", F, Reduced::UnlessOne, false,
				"0: terrain under Terrain Blending, and the meshes that feature redraws after it, stay native (T7c1; live toggle's seed)" },
			{ Forward, "CS_DCLF_FORWARD", F, Reduced::UnlessOne, false,
				"1: blended Lighting geometry drawn by DCLF's forward epoch (T7d: a GPU depth sort, back to front, before the engine's blended draws); 0 it stays native (live toggle's seed)" },
			{ Effects, "CS_DCLF_EFFECTS", F, Reduced::UnlessOne, false,
				"1: static blended BSEffectShaderProperty meshes drawn by DCLF's forward epoch, sorted with blended Lighting (T7d's Effect port; needs CS_DCLF_FORWARD); 0 they stay native (live toggle's seed)" },
			{ Refraction, "CS_DCLF_REFRACTION", F, Reduced::UnlessOne, false,
				"1: refraction (kRefraction, kTempRefraction Lighting geometry: heat haze, invisibility) drawn by DCLF into the refraction normals in the forward epoch (T7d; needs CS_DCLF_FORWARD); 0 it stays native (live toggle's seed)" },
			{ Particles, "CS_DCLF_PARTICLES", F, Reduced::UnlessOne, false,
				"1: particle systems with an effect shader (NiParticleSystem: smoke, flames, glows) drawn by DCLF's forward epoch from snapshots taken at the engine's particle update, sorted with the blended objects (T7e; needs CS_DCLF_EFFECTS); 0 they stay native (live toggle's seed)" },
			{ Billboards, "CS_DCLF_BILLBOARDS", F, Reduced::UnlessOne, false,
				"1: effect-shader meshes under an NiBillboardNode (fire cards, spray jets) drawn by DCLF, turned to each view's camera in the vertex stage as NiBillboardNode::OnVisible turns the node (needs CS_DCLF_EFFECTS); 0 they stay native (live toggle's seed)" },
			{ ProjectedUv, "CS_DCLF_PROJECTED_UV", F, Reduced::UnlessOne, false, "0: projected-UV objects stay native (live toggle's seed)" },
			{ SwitchNodes, "CS_DCLF_SWITCH_NODES", F, Reduced::UnlessOne, false, "0: objects under switch nodes stay native (live toggle's seed)" },
			{ LodObjects, "CS_DCLF_LOD_OBJECTS", F, Reduced::UnlessOne, false, "0: object LOD stays native and untracked (live toggle's seed)" },
			{ LodTerrain, "CS_DCLF_LOD_TERRAIN", F, Reduced::UnlessOne, false, "0: terrain LOD stays native (live toggle's seed)" },
			{ LodTrees, "CS_DCLF_LOD_TREES", F, Reduced::UnlessOne, false, "0: tree LOD stays native (live toggle's seed)" },
			{ LodGates, "CS_DCLF_LOD_GATES", F, Reduced::Zero, false,
				"0: the engine's LOD swaps ungated (T6b5: by default an object or terrain LOD swap keeps the outgoing level shown, and the incoming hidden, until DCLF's claims cover the incoming)" },
			{ Shadows, "CS_DCLF_SHADOWS", F, Reduced::UnlessOne, false, "0: DCLF does not draw the shadow views (live toggle's seed)" },
			{ SunSkip, "CS_DCLF_SUN_SKIP", F, Reduced::UnlessOne, false, "0: the engine keeps culling and registering the sun's casters (live toggle's seed)" },
			{ SunExclude, "CS_DCLF_SUN_EXCLUDE", F, Reduced::UnlessOne, false, "0: DCLF's objects stay in the sun's culls (live toggle's seed)" },
			{ PrimaryExclude, "CS_DCLF_PRIMARY_EXCLUDE", F, Reduced::UnlessOne, false, "0: DCLF's objects stay in the main camera's cull (live toggle's seed)" },
			{ Skylight, "CS_DCLF_SKYLIGHT", F, Reduced::UnlessOne, false, "0: Skylighting's occlusion map stays native (live toggle's seed)" },
			{ Precipitation, "CS_DCLF_PRECIPITATION", F, Reduced::UnlessOne, false, "0: the precipitation occlusion mask stays native (live toggle's seed)" },
			{ Reflections, "CS_DCLF_REFLECTIONS", F, Reduced::UnlessOne, false, "0: the water reflection's faces stay native (live toggle's seed)" },
			{ FirstPerson, "CS_DCLF_FIRST_PERSON", F, Reduced::UnlessOne, false,
				"1: the player's first-person arms, weapons and magic hands drawn by DCLF as a view of the one scene (T7e: kSetFirstPerson members, the view bit kViewFirstPerson at the GPU cull, a depth epoch in RenderDepth and a colour epoch at Main::RenderFirstPersonView); 0 they stay native (live toggle's seed)" },
			{ FirstPersonDecals, "CS_DCLF_FIRST_PERSON_DECALS", F, Reduced::Zero, false,
				"default on (T7f item 7): a decal on a first-person entry (an effect's skinned blood decal on the arms, a weapon's BloodLighting) drawn by the first-person forward part as a forward object with its decal group's depth bias, test and blend (needs CS_DCLF_FIRST_PERSON and CS_DCLF_DECALS); 0 it stays the engine's" },
			{ OpaqueEffects, "CS_DCLF_OPAQUE_EFFECTS", F, Reduced::Zero, false,
				"default on (T7f item 7): opaque BSEffectShaderProperty meshes (hint 0: no blending alpha property, no Falloff, depth tested and written; forge coals, slag) drawn by DCLF in the G-buffer with the DEFERRED effect variant, through the opaque decal group's fixed slots and its decal depth pass without the bias (needs CS_DCLF_EFFECTS and CS_DCLF_DECALS); 0 they stay the engine's (effect-opaque)" },
			{ BillboardParticles, "CS_DCLF_BILLBOARD_PARTICLES", F, Reduced::Zero, false,
				"default on (T7f item 7): particle systems under NiBillboardNodes (the magic hands' fire cores and embers) drawn by DCLF's particle path like any other (their world and particles are their last update's; needs CS_DCLF_BILLBOARDS and CS_DCLF_PARTICLES); 0 they stay the engine's (billboard)" },
			{ EmptyFadeNode, "CS_DCLF_EMPTY_FADE_NODE", F, Reduced::Zero, false,
				"default on (T7f item 7): the engine's empty fade node (the BSFadeNode 0x14332a2a0 holds, which every BSShaderProperty starts with and the engine reads as none) is no fade root: such geometry (terrain, object LOD, some clutter) takes its entry's root; 0: it is listed as one fade root of all of them, outside the scene graph" },
			{ OrgEpochs, "CS_ORG_EPOCHS", F, Reduced::Zero, true, "0: the render graph runs without epochs, and DCLF does not install" },
			{ OrgAsyncEpochs, "CS_ORG_ASYNC_EPOCHS", F, Reduced::Zero, true, "0: epochs are recorded on the render thread" },
			{ OrgClosed, "CS_ORG_CLOSED", F, Reduced::Zero, true, "0: epochs do not return their resources to their home states" },
			{ OrgBatchSubmit, "CS_ORG_BATCH_SUBMIT", F, Reduced::Zero, true, "0: an epoch's submissions go to DXVK one at a time" },
			{ OrgEarlyFlush, "CS_ORG_EARLY_FLUSH", F, Reduced::Zero, true, "0: DXVK's pending work is not flushed when an epoch starts" },

			{ PersistentParity, "CS_DCLF_PERSISTENT_PARITY", P, N, false, "1: every kept store (records, bindings, slots, shading, geometry) against a rebuild, every 60 frames" },
			{ WalkParity, "CS_DCLF_WALK_PARITY", P, N, false, "1: the delta walk against a dense walk, every 60 frames" },
			{ ChangeLogParity, "CS_DCLF_CHANGE_LOG_PARITY", P, N, false, "1: the change log against a diff of the tables" },
			{ ResidentParity, "CS_DCLF_RESIDENT_PARITY", P, N, false, "1: resident records against the engine's registrations" },
			{ FadeParity, "CS_DCLF_FADE_PARITY", P, N, false, "1: the fade port against the engine's functions, and FadeStateCS against the port" },
			{ BuildParity, "CS_DCLF_BUILD_PARITY", P, N, false, "1: the build cache against a fresh build" },
			{ SetParity, "CS_DCLF_SET_PARITY", P, N, false, "1: the per-object visibility words read back after the colour epoch against the CPU's decisions" },
			{ MirrorParity, "CS_DCLF_MIRROR_PARITY", P, N, false, "1: the scene mirror against the live objects, a slice of the tracked set a frame (step 6e F3)" },
			{ CaptureParity, "CS_DCLF_CAPTURE_PARITY", P, N, false, "1: the tables against the engine's own lighting draws (run with CS_DCLF_PARITY_BOTH=1, so the engine draws what DCLF draws)" },
			{ ParityBoth, "CS_DCLF_PARITY_BOTH", P, N, false, "1: nothing is withheld from the engine, which draws everything while DCLF draws its set as well (capture parity compares the two; it double-draws)" },
			{ CapturePointParity, "CS_DCLF_CAPTURE_POINT_PARITY", P, N, false, "1: the colour epoch's captured bindings against the frame's first native lighting draw" },
			{ SkylightParity, "CS_DCLF_SKYLIGHT_PARITY", P, N, false, "1: DCLF's Skylighting occlusion map against the engine's, texel by texel" },
			{ AccumulatedParity, "CS_DCLF_ACCUMULATED_PARITY", P, N, false,
				"1: T7b1/T7b2 observers: DCLF's decoded visibility against the engine's kAccumulated on the listed roots and their geometry, frame by frame (<- ACCUMULATED), and FUN_1402cff60's animation gate on the stood-in roots against DCLF's writes (<- ANIM RATE); off: the readback and the writes still run" },

			{ Stats, "CS_DCLF_STATS", D, N, false, "1: the periodic report" },
			{ PassStats, "CS_DCLF_PASS_STATS", D, N, false, "1: per-pass counts in the report" },
			{ Profile, "CS_DCLF_PROFILE", D, N, false, "1: scene-phase timings in the report" },
			{ ShaderDebug, "CS_DCLF_SHADER_DEBUG", D, N, false, "default on: SPIR-V with debug info, and the HLSL sources copied beside the log; 0: without" },
			{ ShaderSourceDir, "CS_DCLF_SHADER_SOURCE_DIR", D, N, false, "where CS_DCLF_SHADER_DEBUG copies the HLSL sources" },
			{ TraceTexturePaths, "CS_DCLF_TRACE_TEXTURE_PATHS", D, N, false, "set: texture identities are logged with their paths" },
			{ TreeLodAudit, "CS_DCLF_TREE_LOD_AUDIT", D, N, false, "1: tree LOD instances the engine leaves shown over a loaded, visible full tree (and hidden ones without one), every 60 frames" },
			{ Census, "CS_DCLF_CENSUS", D, N, false,
				"1: T7a's census, every report: what the engine still draws (the passes registered and not withheld) per view and class (DCLF's verdict for Lighting geometry; shader type and RTTI otherwise)" },
			{ HiddenWatch, "CS_DCLF_HIDDEN_WATCH", D, N, false, "1: hardware watchpoints name the store behind a hidden change no event announced (with walk parity)" },
			{ MirrorWatch, "CS_DCLF_MIRROR_WATCH", D, N, false, "1: hardware watchpoints on a fade node's statics after its placement name the writers the mirror misses" },
			{ TableStart, "CS_DCLF_TABLE_START", D, N, false, "small: the growable GPU tables start with a few rows, so that growth runs early and often" },
			{ SkylightDumpDir, "CS_DCLF_SKYLIGHT_DUMP_DIR", D, N, false, "where CS_DCLF_SKYLIGHT_PARITY writes the maps it compares" },
			{ DepthTrace, "CS_DCLF_DEPTH_TRACE", D, N, false, "1: the D3D11 calls that touch the main depth buffer, for five frames" },
			{ DrawTrace, "CS_DCLF_DRAW_TRACE", D, N, false, "1: the callers of the D3D11 draws" },
			{ TargetProbe, "CS_DCLF_TARGET_PROBE", D, N, false, "x,y: the G-buffer targets at a pixel where the opaque pass ends (open defect)" },
			{ TargetDump, "CS_DCLF_TARGET_DUMP", D, N, false, "f1,f2,...: at those Present counts, every target bound at each dump point of the frame (and kMAIN, its depth) read back asynchronously and written as half-size PNGs with NaN/Inf marked, and their statistics (Diagnostics/TargetDump.h; into SKSE/dclf-dump/<count>)" },
			{ FoliageParity, "CS_DCLF_FOLIAGE_PARITY", P, N, false,
				"1: the alpha-tested colour draws' pixels (object, albedo, diffuse) against the frame before's, tree wind frozen; wind: not frozen" },
			{ ShadowDebugOutput, "CS_DCLF_SHADOW_DEBUG_OUTPUT", D, N, false, "1: Lighting.hlsl writes the sun's shadow terms into the diffuse target (open defect)" },
			{ ReleaseGuard, "CS_DCLF_RELEASE_GUARD", D, N, false,
				"1: the engine objects' last releases on DCLF's threads (the scene lane, the pool) counted and named by class (NiRefObject::DeleteThis patched; T6b3e)" },

			{ TestCommands, "CS_DCLF_TEST_COMMANDS", T, N, false, "<frame>:<console command>;...: console commands at given frames" },
			{ TestMove, "CS_DCLF_TEST_MOVE", T, N, false, "<start>:<end>:<units per frame>;...: carries the player forward" },
			{ TestTurn, "CS_DCLF_TEST_TURN", T, N, false, "<start>:<end>:<degrees per frame>;...: turns the player" },
			{ TestToggle, "CS_DCLF_TEST_TOGGLE", T, N, false, "<off frame>:<on frame>...: switches DCLF off and on" },
			{ TestExit, "CS_DCLF_TEST_EXIT", T, N, false, "<frame>: the game exits at once after that many frames (startup loops)" },

			{ UpscaleSubmit, "CS_UPSCALE_SUBMIT", O, N, false, "direct: upscaling's ring submissions wait for DXVK's command stream" },
			{ StreamlineReflex, "CS_STREAMLINE_REFLEX", O, N, false, "1: Streamline loads Reflex (and with it DLSS-G)" },
		};
		static_assert(std::size(kRegistry) == static_cast<std::size_t>(Switch::Count));
		static_assert([] {
			for (std::size_t i = 0; i < std::size(kRegistry); ++i)
				if (static_cast<std::size_t>(kRegistry[i].id) != i)
					return false;
			return true;
		}());

		struct Values
		{
			std::array<std::string, static_cast<std::size_t>(Switch::Count)> value;
			std::array<bool, static_cast<std::size_t>(Switch::Count)> enabled{};
			std::vector<std::string> unknown;  // CS_DCLF_* names set in the file or the environment that no row names
		};

		const Values& Read()
		{
			static const Values values = [] {
				Values v;
				for (const auto& entry : kRegistry) {
					auto& value = v.value[static_cast<std::size_t>(entry.id)];
					value = Util::SwitchFile::Value(entry.name, entry.environmentOnly);
					v.enabled[static_cast<std::size_t>(entry.id)] = value == "1";
				}
				auto known = [](std::string_view a_name) {
					for (const auto& entry : kRegistry)
						if (entry.name == a_name)
							return true;
					return false;
				};
				for (const auto& name : Util::SwitchFile::FileNames())
					if (!known(name))
						v.unknown.push_back(name);
				if (auto* block = GetEnvironmentStringsW()) {
					for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
						const std::wstring_view pair(p);
						const auto wideName = pair.substr(0, pair.find(L'='));
						if (!wideName.starts_with(L"CS_DCLF"))
							continue;
						std::string name;  // switch names are ASCII
						for (const wchar_t c : wideName)
							name.push_back(static_cast<char>(c));
						if (!known(name) && std::ranges::find(v.unknown, name) == v.unknown.end())
							v.unknown.push_back(name);
					}
					FreeEnvironmentStringsW(block);
				}
				return v;
			}();
			return values;
		}

		// The features' switches (RegisterSwitch), each read as it registered, listed right after the Core row its after names. Written
		// at install alone (the main thread, before DCLF's threads start), read-only after.
		struct Registered
		{
			std::string name, description, after;
			Kind kind = Kind::Feature;
			Reduced reduced = Reduced::Never;
			std::string value;
			bool enabled = false;
		};
		std::deque<Registered>& RegisteredSwitches()
		{
			static std::deque<Registered> registered;
			return registered;
		}

		/** @brief Every switch row in listing order (each Core row, then the features' after it; the rest last): a_row(name, kind, reduced, value). */
		template <class Row>
		void ForEachRow(Row&& a_row)
		{
			const auto& registered = RegisteredSwitches();
			std::vector<std::uint8_t> listed(registered.size(), 0);
			const auto after = [&](std::string_view a_name) {
				for (std::size_t i = 0; i < registered.size(); ++i)
					if (!listed[i] && registered[i].after == a_name) {
						listed[i] = 1;
						a_row(std::string_view(registered[i].name), registered[i].kind, registered[i].reduced, registered[i].value);
					}
			};
			for (const auto& entry : kRegistry) {
				a_row(entry.name, entry.kind, entry.reduced, SwitchValue(entry.id));
				after(entry.name);
			}
			for (std::size_t i = 0; i < registered.size(); ++i)
				if (!listed[i])
					a_row(std::string_view(registered[i].name), registered[i].kind, registered[i].reduced, registered[i].value);
		}

		bool IsReduced(Reduced a_rule, const std::string& a_value)
		{
			switch (a_rule) {
			case Reduced::UnlessOne:
				return !a_value.empty() && a_value != "1";
			case Reduced::Zero:
				return a_value == "0";
			case Reduced::Cull:
				return a_value == "off" || a_value == "frustum";
			default:
				return false;
			}
		}
	}

	std::filesystem::path SwitchesDirectory() { return Util::SwitchFile::Directory(); }

	const std::string& SwitchValue(Switch a_switch)
	{
		return Read().value[static_cast<std::size_t>(a_switch)];
	}

	bool SwitchEnabled(Switch a_switch)
	{
		return Read().enabled[static_cast<std::size_t>(a_switch)];
	}

	Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch, std::string_view a_feature)
	{
		auto& registered = RegisteredSwitches();
		const auto known = [&](std::string_view a_name) {
			for (const auto& entry : kRegistry)
				if (entry.name == a_name)
					return true;
			return false;
		};
		if (a_switch.name.empty() || known(a_switch.name))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered the switch '{}', which Core has (or no name)", a_feature, a_switch.name));
		for (const auto& entry : registered)
			if (entry.name == a_switch.name)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered the switch {} twice", a_feature, a_switch.name));
		if (!a_switch.after.empty() && !known(a_switch.after))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s switch {} is listed after {}, which Core does not have", a_feature, a_switch.name, a_switch.after));
		auto& entry = registered.emplace_back();
		entry.name = a_switch.name;
		entry.description = a_switch.description;
		entry.after = a_switch.after;
		entry.kind = static_cast<Kind>(a_switch.kind);
		entry.reduced = a_switch.reduced == Api::SwitchReduced::UnlessOne ? Reduced::UnlessOne : a_switch.reduced == Api::SwitchReduced::Zero ? Reduced::Zero : Reduced::Never;
		entry.value = Util::SwitchFile::Value(entry.name, false);
		entry.enabled = entry.value == "1";
		return { static_cast<std::uint32_t>(registered.size() - 1) };
	}

	const std::string& SwitchValue(Api::SwitchId a_switch)
	{
		static const std::string kUnset;
		const auto& registered = RegisteredSwitches();
		return a_switch.index < registered.size() ? registered[a_switch.index].value : kUnset;
	}

	bool SwitchEnabled(Api::SwitchId a_switch)
	{
		const auto& registered = RegisteredSwitches();
		return a_switch.index < registered.size() && registered[a_switch.index].enabled;
	}

	std::string ReducedFeatures()
	{
		std::string text;
		ForEachRow([&](std::string_view a_name, Kind, Reduced a_reduced, const std::string& a_value) {
			if (!IsReduced(a_reduced, a_value))
				return;
			if (!text.empty())
				text += ", ";
			text += fmt::format("{}={}", a_name, a_value);
		});
		return text;
	}

	std::string SwitchSummary()
	{
		// Grouped by kind, so a run's features, checks and probes read apart at a glance.
		constexpr std::pair<Kind, std::string_view> kGroups[] = {
			{ Kind::Feature, "features" }, { Kind::Parity, "parity" }, { Kind::Diagnostic, "diagnostics" }, { Kind::Test, "test" }, { Kind::Other, "other" }
		};
		std::string text;
		for (const auto& [kind, label] : kGroups) {
			std::string group;
			const Kind wanted = kind;
			ForEachRow([&](std::string_view a_name, Kind a_kind, Reduced, const std::string& a_value) {
				if (a_kind == wanted && !a_value.empty())
					group += fmt::format("{}{}={}", group.empty() ? "" : ", ", a_name, a_value);
			});
			if (!group.empty())
				text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", label, group);
		}
		// Read before the features registered theirs: a name one of them registered is known.
		std::string unknown;
		for (const auto& name : Read().unknown)
			if (std::ranges::none_of(RegisteredSwitches(), [&](const Registered& a_entry) { return a_entry.name == name; }))
				unknown += fmt::format("{}{}", unknown.empty() ? "" : ", ", name);
		if (!unknown.empty())
			text += fmt::format("{}UNKNOWN (read by nothing): {}", text.empty() ? "" : "; ", unknown);
		return text.empty() ? std::string("none set") : text;
	}
}
