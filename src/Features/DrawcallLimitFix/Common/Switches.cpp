#include "Switches.h"

#include <ShlObj.h>
#include <Windows.h>

#include <array>
#include <fstream>
#include <string_view>

#include <ankerl/unordered_dense.h>

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
			UnlessStatic,  // on when unset or "static"
			Zero,        // off only with "0"
			Async,       // "off", "0" and "probe" (inline builds, compared) all narrow it
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
		constexpr auto N = Reduced::Never;

		// One row per switch, in Switch order (checked below).
		constexpr Entry kRegistry[] = {
			{ Dclf, "CS_DCLF", F, Reduced::Zero, false, "0: DCLF does not install" },
			{ Async, "CS_DCLF_ASYNC", F, Reduced::Async, false, "off|0: builds inline on the render thread; probe: the worker's builds, compared against inline ones" },
			{ AsyncWaitMs, "CS_DCLF_ASYNC_WAIT_MS", F, N, false, "how long the render thread waits for a worker build before building inline (default 3)" },
			{ AsyncPriority, "CS_DCLF_ASYNC_PRIORITY", F, N, false, "normal: the worker thread keeps normal priority instead of above normal" },
			{ Precompile, "CS_DCLF_PRECOMPILE", F, Reduced::Zero, false, "0: DCLF's SPIR-V programs compile on first use instead of alongside the engine's shaders" },
			{ Cull, "CS_DCLF_CULL", F, Reduced::Cull, false, "off|frustum|occlusion (default): GPU culling, the live toggle's seed" },
			{ Ownership, "CS_DCLF_OWNERSHIP", F, Reduced::UnlessStatic, false, "static (default)|off: withhold DCLF's passes from the engine, the live toggle's seed" },
			{ ShadowOwnership, "CS_DCLF_SHADOW_OWNERSHIP", F, Reduced::UnlessStatic, false, "static (default)|off: withhold DCLF's shadow casters, the live toggle's seed" },
			{ Skinned, "CS_DCLF_SKINNED", F, Reduced::UnlessOne, false, "0: skinned objects stay native (live toggle's seed)" },
			{ SkinPartitions, "CS_DCLF_SKIN_PARTITIONS", F, Reduced::UnlessOne, false, "0: skins of several partitions stay native (live toggle's seed)" },
			{ Actors, "CS_DCLF_ACTORS", F, Reduced::UnlessOne, false, "0: actors stay native (live toggle's seed)" },
			{ Trees, "CS_DCLF_TREES", F, Reduced::UnlessOne, false, "0: trees stay native (live toggle's seed)" },
			{ Decals, "CS_DCLF_DECALS", F, Reduced::UnlessOne, false, "0: decals stay native (live toggle's seed)" },
			{ Fading, "CS_DCLF_FADING", F, Reduced::UnlessOne, false, "0: fading objects stay native (live toggle's seed)" },
			{ LodCrossfade, "CS_DCLF_LOD_CROSSFADE", F, Reduced::UnlessOne, false, "0: objects in a LOD cross-fade stay native (live toggle's seed)" },
			{ MtLand, "CS_DCLF_MTLAND", F, Reduced::UnlessOne, false, "0: terrain stays native (live toggle's seed)" },
			{ ProjectedUv, "CS_DCLF_PROJECTED_UV", F, Reduced::UnlessOne, false, "0: projected-UV objects stay native (live toggle's seed)" },
			{ SwitchNodes, "CS_DCLF_SWITCH_NODES", F, Reduced::UnlessOne, false, "0: objects under switch nodes stay native (live toggle's seed)" },
			{ Shadows, "CS_DCLF_SHADOWS", F, Reduced::UnlessOne, false, "0: DCLF does not draw the shadow views (live toggle's seed)" },
			{ SunSkip, "CS_DCLF_SUN_SKIP", F, Reduced::UnlessOne, false, "0: the engine keeps culling and registering the sun's casters (live toggle's seed)" },
			{ SunExclude, "CS_DCLF_SUN_EXCLUDE", F, Reduced::UnlessOne, false, "0: DCLF's objects stay in the sun's culls; probe: the exclusion runs dry (live toggle's seed)" },
			{ PrimaryExclude, "CS_DCLF_PRIMARY_EXCLUDE", F, Reduced::UnlessOne, false, "0: DCLF's objects stay in the main camera's cull; probe: a census that removes nothing (live toggle's seed)" },
			{ ListFilter, "CS_DCLF_LIST_FILTER", F, Reduced::Zero, false, "0: the scene lists keep the roots DCLF draws whole (the engine's cull and the sun's full-frustum cull walk them)" },
			{ TreeList, "CS_DCLF_TREE_LIST", F, Reduced::Zero, false, "0: the trees DCLF draws stay on the tree manager's animation list (its update walks them every frame)" },
			{ LightExclude, "CS_DCLF_LIGHT_EXCLUDE", F, Reduced::Async, false, "0: point lights' shadow culls walk DCLF's entries too; probe: nothing skipped, counted" },
			{ MoveEvents, "CS_DCLF_MOVE_EVENTS", F, Reduced::Zero, false, "0: the light path places every mover every frame instead of those the engine's move events name" },
			{ HiddenEvents, "CS_DCLF_HIDDEN_EVENTS", F, Reduced::Zero, false, "0: an actor's frame verdict is taken again every frame instead of on its chain's hidden-bit events" },
			{ Skylight, "CS_DCLF_SKYLIGHT", F, Reduced::UnlessOne, false, "0: Skylighting's occlusion map stays native (live toggle's seed)" },
			{ Precipitation, "CS_DCLF_PRECIPITATION", F, Reduced::UnlessOne, false, "0: the precipitation occlusion mask stays native (live toggle's seed)" },
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
			{ ResidentDrawParity, "CS_DCLF_RESIDENT_DRAW_PARITY", P, N, false, "1: the resident region's draws against a whole-scene build" },
			{ BuildParity, "CS_DCLF_BUILD_PARITY", P, N, false, "1: the build cache against a fresh build" },
			{ SetParity, "CS_DCLF_SET_PARITY", P, N, false, "1: the per-object visibility words read back after the colour epoch against the CPU's decisions" },
			{ BindlessParity, "CS_DCLF_BINDLESS_PARITY", P, N, false, "1: the object records against the values of the constant groups they replace" },
			{ CaptureParity, "CS_DCLF_CAPTURE_PARITY", P, N, false, "1: the tables against the engine's own lighting draws (run with CS_DCLF_OWNERSHIP=off)" },
			{ CapturePointParity, "CS_DCLF_CAPTURE_POINT_PARITY", P, N, false, "1: the colour epoch's captured bindings against the frame's first native lighting draw" },
			{ SkylightParity, "CS_DCLF_SKYLIGHT_PARITY", P, N, false, "1: DCLF's Skylighting occlusion map against the engine's, texel by texel" },
			{ ClassifyCache, "CS_DCLF_CLASSIFY_CACHE", P, N, false, "probe: every cached classification verdict against a recomputed one" },
			{ DerivedCache, "CS_DCLF_DERIVED_CACHE", P, N, false, "probe: every cached derivation against a recomputed one" },
			{ MaterialCache, "CS_DCLF_MATERIAL_CACHE", P, N, false, "off: stops the standing check of a few drawn material records against a live evaluation" },

			{ Stats, "CS_DCLF_STATS", D, N, false, "1: the periodic report" },
			{ PassStats, "CS_DCLF_PASS_STATS", D, N, false, "1: per-pass counts in the report" },
			{ Profile, "CS_DCLF_PROFILE", D, N, false, "1: scene-phase timings in the report" },
			{ ShaderDebug, "CS_DCLF_SHADER_DEBUG", D, N, false, "1: SPIR-V with debug info, and the HLSL sources copied beside the log" },
			{ ShaderSourceDir, "CS_DCLF_SHADER_SOURCE_DIR", D, N, false, "where CS_DCLF_SHADER_DEBUG copies the HLSL sources" },
			{ Eval, "CS_DCLF_EVAL", D, N, false, "audit: device state before and after each constant evaluation, compared" },
			{ TraceTexturePaths, "CS_DCLF_TRACE_TEXTURE_PATHS", D, N, false, "set: texture identities are logged with their paths" },
			{ CoverageProbe, "CS_DCLF_COVERAGE_PROBE", D, N, false, "1: which shaders the objects DCLF leaves native use" },
			{ DeriveProbe, "CS_DCLF_DERIVE_PROBE", D, N, false, "1: derivation statistics in the report" },
			{ SlotProbe, "CS_DCLF_SLOT_PROBE", D, N, false, "1: slot lifetime statistics in the report" },
			{ DecalOrderProbe, "CS_DCLF_DECAL_ORDER_PROBE", D, N, false, "1: the main pass's decal registration order against the scene lists' order" },
			{ HiddenWatch, "CS_DCLF_HIDDEN_WATCH", D, N, false, "1: hardware watchpoints name the store behind a hidden change no event announced (with walk parity)" },
			{ InputWatch, "CS_DCLF_INPUT_WATCH", D, N, false, "1: which of a per-frame entry's classify and shading inputs changed when the light path re-read them" },
			{ DecalOrder, "CS_DCLF_DECAL_ORDER", F, N, false, "engine: member decals draw in the order of the frame's scene lists, as the engine's (which reorders them whenever a root before them is shown or hidden); stable (default): the scene's order, rewritten only when the member decals change" },
			{ TableStart, "CS_DCLF_TABLE_START", D, N, false, "small: the growable GPU tables start with a few rows, so that growth runs early and often" },
			{ SkylightDumpDir, "CS_DCLF_SKYLIGHT_DUMP_DIR", D, N, false, "where CS_DCLF_SKYLIGHT_PARITY writes the maps it compares" },
			{ DepthTrace, "CS_DCLF_DEPTH_TRACE", D, N, false, "1: the D3D11 calls that touch the main depth buffer, for five frames" },
			{ DrawTrace, "CS_DCLF_DRAW_TRACE", D, N, false, "1: the callers of the D3D11 draws" },
			{ TargetProbe, "CS_DCLF_TARGET_PROBE", D, N, false, "x,y: the G-buffer targets at a pixel where the opaque pass ends (open defect)" },
			{ ShadowMaskProbe, "CS_DCLF_SHADOWMASK_PROBE", D, N, false, "x,y: the shadow mask at a pixel (open defect)" },
			{ ShadowMapProbe, "CS_DCLF_SHADOWMAP_PROBE", D, N, false, "1: the shadow maps' contents (open defect)" },
			{ GBufferProbe, "CS_DCLF_GBUFFER_PROBE", D, N, false, "x,y: the G-buffer at a pixel after DCLF's colour epoch (open defect)" },
			{ ShadowDebugOutput, "CS_DCLF_SHADOW_DEBUG_OUTPUT", D, N, false, "1: Lighting.hlsl writes the sun's shadow terms into the diffuse target (open defect)" },

			{ TestCommands, "CS_DCLF_TEST_COMMANDS", T, N, false, "<frame>:<console command>;...: console commands at given frames" },
			{ TestMove, "CS_DCLF_TEST_MOVE", T, N, false, "<start>:<end>:<units per frame>;...: carries the player forward" },
			{ TestTurn, "CS_DCLF_TEST_TURN", T, N, false, "<start>:<end>:<degrees per frame>;...: turns the player" },
			{ TestToggle, "CS_DCLF_TEST_TOGGLE", T, N, false, "<off frame>:<on frame>...: switches DCLF off and on" },

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

		constexpr const char* kFileName = "CommunityShaders-DCLF.ini";

		// NAME=value per line; blank lines and lines starting with ';' or '#' are ignored.
		ankerl::unordered_dense::map<std::string, std::string> LoadFile()
		{
			ankerl::unordered_dense::map<std::string, std::string> values;
			const auto directory = SwitchesDirectory();
			if (directory.empty())
				return values;
			std::ifstream file(directory / kFileName);
			if (!file)
				return values;
			std::string line;
			while (std::getline(file, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				const auto first = line.find_first_not_of(" \t");
				if (first == std::string::npos || line[first] == ';' || line[first] == '#')
					continue;
				const auto equals = line.find('=', first);
				if (equals == std::string::npos)
					continue;
				auto name = line.substr(first, equals - first);
				while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
					name.pop_back();
				values[name] = line.substr(equals + 1);
			}
			return values;
		}

		std::string Environment(std::string_view a_name)
		{
			char buffer[1024] = {};
			const std::string name(a_name);
			const auto length = GetEnvironmentVariableA(name.c_str(), buffer, sizeof(buffer));
			return length && length < sizeof(buffer) ? std::string(buffer, length) : std::string();
		}

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
				const auto file = LoadFile();
				for (const auto& entry : kRegistry) {
					auto& value = v.value[static_cast<std::size_t>(entry.id)];
					value = Environment(entry.name);
					if (value.empty() && !entry.environmentOnly)
						if (const auto it = file.find(std::string(entry.name)); it != file.end())
							value = it->second;
					v.enabled[static_cast<std::size_t>(entry.id)] = value == "1";
				}
				auto known = [](std::string_view a_name) {
					for (const auto& entry : kRegistry)
						if (entry.name == a_name)
							return true;
					return false;
				};
				for (const auto& [name, value] : file)
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

		bool IsReduced(Reduced a_rule, const std::string& a_value)
		{
			switch (a_rule) {
			case Reduced::UnlessOne:
				return !a_value.empty() && a_value != "1";
			case Reduced::UnlessStatic:
				return !a_value.empty() && a_value != "static";
			case Reduced::Zero:
				return a_value == "0";
			case Reduced::Async:
				return a_value == "off" || a_value == "0" || a_value == "probe";
			case Reduced::Cull:
				return a_value == "off" || a_value == "frustum";
			default:
				return false;
			}
		}
	}

	std::filesystem::path SwitchesDirectory()
	{
		PWSTR documents = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
			return {};
		std::filesystem::path path(documents);
		CoTaskMemFree(documents);
		return path / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
	}

	const std::string& SwitchValue(Switch a_switch)
	{
		return Read().value[static_cast<std::size_t>(a_switch)];
	}

	bool SwitchEnabled(Switch a_switch)
	{
		return Read().enabled[static_cast<std::size_t>(a_switch)];
	}

	std::string ReducedFeatures()
	{
		std::string text;
		for (const auto& entry : kRegistry) {
			const auto& value = SwitchValue(entry.id);
			if (!IsReduced(entry.reduced, value))
				continue;
			if (!text.empty())
				text += ", ";
			text += fmt::format("{}={}", entry.name, value);
		}
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
			for (const auto& entry : kRegistry)
				if (entry.kind == kind && !SwitchValue(entry.id).empty())
					group += fmt::format("{}{}={}", group.empty() ? "" : ", ", entry.name, SwitchValue(entry.id));
			if (!group.empty())
				text += fmt::format("{}{}: {}", text.empty() ? "" : "; ", label, group);
		}
		std::string unknown;
		for (const auto& name : Read().unknown)
			unknown += fmt::format("{}{}", unknown.empty() ? "" : ", ", name);
		if (!unknown.empty())
			text += fmt::format("{}UNKNOWN (read by nothing): {}", text.empty() ? "" : "; ", unknown);
		return text.empty() ? std::string("none set") : text;
	}
}
