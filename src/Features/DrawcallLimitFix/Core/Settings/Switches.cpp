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
			{ AsyncCompute, "CS_DCLF_ASYNC_COMPUTE", F, Reduced::Zero, false,
				"0: the passes that prefer the compute queue (build-draws' queries, fades, portal batches, tree wind) run on the graphics queue" },
			{ ForwardPlain, "CS_DCLF_FORWARD_PLAIN", F, Reduced::Zero, false,
				"0: the forward epoch draws its range device-generated, a call per state class, instead of a plain draw per pipeline" },
			{ MainPlain, "CS_DCLF_MAIN_PLAIN", F, Reduced::Zero, false,
				"0: the colour pass draws its main range device-generated, a call per state class, instead of a plain draw per pipeline" },
			{ TerrainPlain, "CS_DCLF_TERRAIN_PLAIN", F, Reduced::Zero, false,
				"0: the colour segment's second pass draws the terrain groups' ranges device-generated, a call per state class, instead of a plain draw per pipeline" },
			{ Workers, "CS_DCLF_WORKERS", F, N, false, "the preparation pool's threads (default hardware threads - 2, at least 1)" },
			{ Fanout, "CS_DCLF_FANOUT", F, N, false,
				"the scene pass's evaluation rounds and joins (T6b3e): 0 interleaved on the scene lane (the reference), 1 every entry evaluated then merged, on the lane; 2 (default) evaluated on the preparation pool from 128 entries, merged in order" },
			{ PayloadAppend, "CS_DCLF_PAYLOAD_APPEND", F, N, false,
				"1: a payload part whose changes since the last publication's backing all lie past what any published payload names of it appends into that backing instead of catching up a pooled one (default off)" },
			{ Precompile, "CS_DCLF_PRECOMPILE", F, Reduced::Zero, false, "0: DCLF's SPIR-V programs compile on first use instead of alongside the engine's shaders" },
			{ Cull, "CS_DCLF_CULL", F, Reduced::Cull, false, "off|frustum|occlusion (default): GPU culling, the live toggle's seed" },
			// (Features/Skins' CS_DCLF_SKINNED, then its CS_DCLF_SKIN_PARTITIONS, are listed here, after CS_DCLF_CULL; Features/Actors',
			// Features/TreeWind's and Features/Decals' switches after CS_DCLF_SKIN_PARTITIONS; then Features/Landscape's CS_DCLF_MTLAND and
			// Integrations/TerrainBlending's CS_DCLF_TB_TERRAIN, before Features/Forward's CS_DCLF_FORWARD; then Features/Effects'
			// CS_DCLF_EFFECTS after it, and Features/Forward's CS_DCLF_REFRACTION, before CS_DCLF_PROJECTED_UV.)
			{ ProjectedUv, "CS_DCLF_PROJECTED_UV", F, Reduced::UnlessOne, false, "0: projected-UV objects stay native (live toggle's seed)" },
			// (Features/Lod's CS_DCLF_LOD_OBJECTS and CS_DCLF_LOD_TERRAIN, then Features/TreeLod's CS_DCLF_LOD_TREES, then Features/Lod's
			// CS_DCLF_LOD_GATES, are listed here, before CS_DCLF_SHADOWS.)
			{ Shadows, "CS_DCLF_SHADOWS", F, Reduced::UnlessOne, false, "0: DCLF does not draw the shadow views (live toggle's seed)" },
			// (Features/Shadows/Sun's CS_DCLF_SUN_SKIP, then its CS_DCLF_SUN_EXCLUDE, are listed here, after CS_DCLF_SHADOWS.)
			{ PrimaryExclude, "CS_DCLF_PRIMARY_EXCLUDE", F, Reduced::UnlessOne, false, "0: DCLF's objects stay in the main camera's cull (live toggle's seed)" },
			// (Features/TreeWind's CS_DCLF_TREE_LIST_ALL, then Features/Effects' CS_DCLF_OPAQUE_EFFECTS, are listed here, after
			// CS_DCLF_PRIMARY_EXCLUDE: Features/OcclusionMaps' CS_DCLF_SKYLIGHT and CS_DCLF_PRECIPITATION, Features/Reflection's and
			// Features/FirstPerson's switches before CS_DCLF_OPAQUE_EFFECTS, Features/Billboards' CS_DCLF_BILLBOARD_PARTICLES after it.)
			// (Features/Fades' CS_DCLF_EMPTY_FADE_NODE is listed here, before CS_ORG_EPOCHS.)
			{ OrgEpochs, "CS_ORG_EPOCHS", F, Reduced::Zero, true, "0: the render graph runs without epochs, and DCLF does not install" },
			{ OrgAsyncEpochs, "CS_ORG_ASYNC_EPOCHS", F, Reduced::Zero, true, "0: epochs are recorded on the render thread" },
			{ OrgClosed, "CS_ORG_CLOSED", F, Reduced::Zero, true, "0: epochs do not return their resources to their home states" },
			{ OrgBatchSubmit, "CS_ORG_BATCH_SUBMIT", F, Reduced::Zero, true, "0: an epoch's submissions go to DXVK one at a time" },
			{ OrgEarlyFlush, "CS_ORG_EARLY_FLUSH", F, Reduced::Zero, true, "0: DXVK's pending work is not flushed when an epoch starts" },

			{ PersistentParity, "CS_DCLF_PERSISTENT_PARITY", P, N, false, "1: every kept store (records, bindings, slots, shading, geometry) against a rebuild, every 60 frames" },
			{ WalkParity, "CS_DCLF_WALK_PARITY", P, N, false, "1: the delta walk against a dense walk, every 60 frames" },
			{ ChangeLogParity, "CS_DCLF_CHANGE_LOG_PARITY", P, N, false, "1: the change log against a diff of the tables" },
			{ ResidentParity, "CS_DCLF_RESIDENT_PARITY", P, N, false, "1: resident records against the engine's registrations" },
			// (Features/Fades' CS_DCLF_FADE_PARITY is listed here, after CS_DCLF_RESIDENT_PARITY.)
			{ BuildParity, "CS_DCLF_BUILD_PARITY", P, N, false, "1: the build cache against a fresh build" },
			{ SetParity, "CS_DCLF_SET_PARITY", P, N, false, "1: the per-object visibility words read back after the colour epoch against the CPU's decisions" },
			{ MirrorParity, "CS_DCLF_MIRROR_PARITY", P, N, false, "1: the scene mirror against the live objects, a slice of the tracked set a frame (step 6e F3)" },
			{ CaptureParity, "CS_DCLF_CAPTURE_PARITY", P, N, false, "1: the tables against the engine's own lighting draws (run with CS_DCLF_PARITY_BOTH=1, so the engine draws what DCLF draws)" },
			{ ParityBoth, "CS_DCLF_PARITY_BOTH", P, N, false, "1: nothing is withheld from the engine, which draws everything while DCLF draws its set as well (capture parity compares the two; it double-draws)" },
			{ CapturePointParity, "CS_DCLF_CAPTURE_POINT_PARITY", P, N, false, "1: the colour epoch's captured bindings against the frame's first native lighting draw" },
			// (Features/OcclusionMaps' CS_DCLF_SKYLIGHT_PARITY is listed here, after CS_DCLF_CAPTURE_POINT_PARITY.)
			{ AccumulatedParity, "CS_DCLF_ACCUMULATED_PARITY", P, N, false,
				"1: T7b1/T7b2 observers: DCLF's decoded visibility against the engine's kAccumulated on the listed roots and their geometry, frame by frame (<- ACCUMULATED), and FUN_1402cff60's animation gate on the stood-in roots against DCLF's writes (<- ANIM RATE); off: the readback and the writes still run" },

			{ Stats, "CS_DCLF_STATS", D, N, false, "1: the periodic report" },
			{ PassStats, "CS_DCLF_PASS_STATS", D, N, false, "1: per-pass counts in the report" },
			{ Profile, "CS_DCLF_PROFILE", D, N, false, "1: scene-phase timings in the report" },
			{ ShaderDebug, "CS_DCLF_SHADER_DEBUG", D, N, false, "default on: SPIR-V with debug info, and the HLSL sources copied beside the log; 0: without" },
			{ ShaderSourceDir, "CS_DCLF_SHADER_SOURCE_DIR", D, N, false, "where CS_DCLF_SHADER_DEBUG copies the HLSL sources" },
			{ TraceTexturePaths, "CS_DCLF_TRACE_TEXTURE_PATHS", D, N, false, "set: texture identities are logged with their paths" },
			// (Features/TreeLod's CS_DCLF_TREE_LOD_AUDIT is listed here, after CS_DCLF_TRACE_TEXTURE_PATHS.)
			{ Census, "CS_DCLF_CENSUS", D, N, false,
				"1: T7a's census, every report: what the engine still draws (the passes registered and not withheld) per view and class (DCLF's verdict for Lighting geometry; shader type and RTTI otherwise)" },
			{ MirrorWatch, "CS_DCLF_MIRROR_WATCH", D, N, false, "1: hardware watchpoints on a fade node's statics after its placement name the writers the mirror misses" },
			{ TableStart, "CS_DCLF_TABLE_START", D, N, false, "small: the growable GPU tables start with a few rows, so that growth runs early and often" },
			// (Features/OcclusionMaps' CS_DCLF_SKYLIGHT_DUMP_DIR is listed here, after CS_DCLF_TABLE_START.)
			{ DepthTrace, "CS_DCLF_DEPTH_TRACE", D, N, false, "1: the D3D11 calls that touch the main depth buffer, for five frames" },
			{ DrawTrace, "CS_DCLF_DRAW_TRACE", D, N, false, "1: the callers of the D3D11 draws" },
			{ TargetProbe, "CS_DCLF_TARGET_PROBE", D, N, false, "x,y: the G-buffer targets at a pixel where the opaque pass ends (open defect)" },
			{ TargetDump, "CS_DCLF_TARGET_DUMP", D, N, false, "f1,f2,...: at those Present counts, every target bound at each dump point of the frame (and kMAIN, its depth) read back asynchronously and written as half-size PNGs with NaN/Inf marked, and their statistics (Diagnostics/TargetDump.h; into SKSE/dclf-dump/<count>)" },
			{ FoliageParity, "CS_DCLF_FOLIAGE_PARITY", P, N, false,
				"1: the alpha-tested colour draws' pixels (object, albedo, diffuse) against the frame before's, tree wind frozen; wind: not frozen" },
			{ ShadowDebugOutput, "CS_DCLF_SHADOW_DEBUG_OUTPUT", D, N, false, "1: Lighting.hlsl writes the sun's shadow terms into the diffuse target (open defect)" },
			{ ReleaseGuard, "CS_DCLF_RELEASE_GUARD", D, N, false,
				"1: the engine objects' last releases on DCLF's threads (the scene lane, the pool) counted and named by class (NiRefObject::DeleteThis patched; T6b3e)" },
			{ ScheduleDump, "CS_DCLF_SCHEDULE_DUMP", D, N, false, "1: each epoch's passes as gathered, logged whenever a list changes (tools/dclf-schedule-diff.py)" },
			{ FrameTrace, "CS_DCLF_FRAME_TRACE", D, N, false,
				"1: every frame's adoption, epoch coverage, main epoch outcomes, colour culling counts and retirement point, and F10 as a marker, for finding corrupt frames (tools/dclf-corrupt-frames.py)" },

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

		// The features' switches (RegisterSwitch), each read as it registered, listed right after the row its after names or right
		// before the one its before names (Core's, or a feature's). Written at install alone (the main thread, before DCLF's threads
		// start), read-only after.
		struct Registered
		{
			std::string name, description, after, before;
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

		/**
		 * @brief The features' rows whose a_anchor names a_name, in registration order, each with the rows anchored at it around it
		 * (those before it first, those after it next): a_row(name, kind, reduced, value). a_listed marks the rows listed.
		 */
		template <class Row>
		void ListAnchoredAt(Row& a_row, std::vector<std::uint8_t>& a_listed, std::string Registered::*a_anchor, std::string_view a_name)
		{
			const auto& registered = RegisteredSwitches();
			for (std::size_t i = 0; i < registered.size(); ++i)
				if (!a_listed[i] && registered[i].*a_anchor == a_name) {
					a_listed[i] = 1;
					const auto& entry = registered[i];
					ListAnchoredAt(a_row, a_listed, &Registered::before, entry.name);
					a_row(std::string_view(entry.name), entry.kind, entry.reduced, entry.value);
					ListAnchoredAt(a_row, a_listed, &Registered::after, entry.name);
				}
		}

		/**
		 * @brief Every switch row in listing order (each Core row, the features' before it first and after it next, each with the rows
		 * anchored at it around it; the rest last): a_row(name, kind, reduced, value).
		 */
		template <class Row>
		void ForEachRow(Row&& a_row)
		{
			const auto& registered = RegisteredSwitches();
			std::vector<std::uint8_t> listed(registered.size(), 0);
			for (const auto& entry : kRegistry) {
				ListAnchoredAt(a_row, listed, &Registered::before, entry.name);
				a_row(entry.name, entry.kind, entry.reduced, SwitchValue(entry.id));
				ListAnchoredAt(a_row, listed, &Registered::after, entry.name);
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
		// Its anchor may be a feature's switch registered after it: CheckSwitchAnchors checks it once every feature registered.
		if (!a_switch.before.empty() && !a_switch.after.empty())
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s switch {} is listed before {} and after {}", a_feature, a_switch.name, a_switch.before,
				a_switch.after));
		auto& entry = registered.emplace_back();
		entry.name = a_switch.name;
		entry.description = a_switch.description;
		entry.after = a_switch.after;
		entry.before = a_switch.before;
		entry.kind = static_cast<Kind>(a_switch.kind);
		entry.reduced = a_switch.reduced == Api::SwitchReduced::UnlessOne ? Reduced::UnlessOne : a_switch.reduced == Api::SwitchReduced::Zero ? Reduced::Zero : Reduced::Never;
		entry.value = Util::SwitchFile::Value(entry.name, false);
		entry.enabled = entry.value == "1";
		return { static_cast<std::uint32_t>(registered.size() - 1) };
	}

	void CheckSwitchAnchors()
	{
		// A row no Core row lists has an anchor that names no switch, or anchors that loop back to it.
		const auto& registered = RegisteredSwitches();
		std::vector<std::uint8_t> listed(registered.size(), 0);
		auto none = [](std::string_view, Kind, Reduced, const std::string&) {};
		for (const auto& entry : kRegistry) {
			ListAnchoredAt(none, listed, &Registered::before, entry.name);
			ListAnchoredAt(none, listed, &Registered::after, entry.name);
		}
		for (std::size_t i = 0; i < registered.size(); ++i) {
			const auto& entry = registered[i];
			if (!listed[i] && (!entry.after.empty() || !entry.before.empty()))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the switch {} is listed {} {}, which no switch is (or its anchors loop)", entry.name,
					entry.after.empty() ? "before" : "after", entry.after.empty() ? entry.before : entry.after));
		}
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

	const std::string& RegisteredSwitchValue(std::string_view a_name)
	{
		static const std::string kUnset;
		for (const auto& entry : RegisteredSwitches())
			if (entry.name == a_name)
				return entry.value;
		return kUnset;
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
