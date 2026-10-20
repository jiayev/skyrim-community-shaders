#pragma once

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace DCLF
{
	/**
	 * @brief Every `CS_*` switch DCLF's Core (and the few other features that share its switches file) reads. The
	 * registry in Switches.cpp holds each one's name, kind and description, in this order. DCLF's features register their own
	 * (RegisterSwitch), read by their Api::SwitchId.
	 */
	enum class Switch : std::uint8_t
	{
		// Features: unset is DCLF's full featureset; the startup log names a run that narrows one.
		Dclf,
		AsyncPriority,
		AsyncCompute,
		ForwardPlain,
		MainPlain,
		TerrainPlain,
		Workers,
		Fanout,
		PayloadAppend,
		Precompile,
		Cull,
		ProjectedUv,
		Shadows,
		PrimaryExclude,
		OrgEpochs,
		OrgAsyncEpochs,
		OrgClosed,
		OrgBatchSubmit,
		OrgEarlyFlush,

		// Parity checks: a live path against a from-scratch reference, logging each mismatch.
		PersistentParity,
		WalkParity,
		ChangeLogParity,
		ResidentParity,
		BuildParity,
		SetParity,
		MirrorParity,
		CaptureParity,
		ParityBoth,
		CapturePointParity,
		AccumulatedParity,

		// Diagnostics: stats, shader debugging, and the probes of open investigations (dclf-validation.md, "Probes and diagnostics").
		Stats,
		PassStats,
		Profile,
		ShaderDebug,
		ShaderSourceDir,
		TraceTexturePaths,
		Census,
		MirrorWatch,
		TableStart,
		DepthTrace,
		DrawTrace,
		TargetProbe,
		TargetDump,
		FoliageParity,
		ShadowDebugOutput,
		ReleaseGuard,
		ScheduleDump,
		FrameTrace,

		// Test harness: scripted input for unattended runs.
		TestCommands,
		TestMove,
		TestTurn,
		TestToggle,
		TestExit,

		// Other features' switches, read through the same file.
		UpscaleSubmit,
		StreamlineReflex,

		Count
	};

	/**
	 * @brief A switch's value, from the environment or from the switches file; empty when set in neither.
	 *
	 * The environment is checked first, then `<Documents>\My Games\Skyrim Special Edition\SKSE\
	 * CommunityShaders-DCLF.ini`, a flat `NAME=value` file in the directory CS already writes its log to.
	 * The file exists because the game does not reliably inherit the environment of whatever started it:
	 * Mod Organizer 2 hands the game its own environment block, so switches exported by a launcher script
	 * never arrive, and a run silently behaves as if every switch were off. The render graph's `CS_ORG_*`
	 * switches are the exception: RenderGraphRuntime reads them from the environment alone, so they are
	 * read from there here too.
	 *
	 * Every switch is read once, on first use of any, so none can change under a running frame.
	 */
	const std::string& SwitchValue(Switch a_switch);

	/** @brief Whether a switch is set to "1". */
	bool SwitchEnabled(Switch a_switch);

	/** @brief Whether a default-on switch is on: anything but "0" (unset included). */
	inline bool SwitchOnUnlessZero(Switch a_switch) { return SwitchValue(a_switch) != "0"; }

	/**
	 * @brief Main thread, at install (Extensions::InstallFeatures, for a_feature): a feature's switch, read now, as Core's were. Fails
	 * the install for a name Core or another feature has, or both an after and a before. Its after or before names a Core switch or a
	 * feature's (registered before it or after), which CheckSwitchAnchors checks once every feature registered.
	 */
	Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch, std::string_view a_feature);
	/**
	 * @brief Main thread, at the end of the features' install (Extensions::InstallFeatures): fails the install for a feature's switch
	 * whose after or before names no switch, Core's or a feature's, or whose anchors loop back to it (it would be listed nowhere).
	 */
	void CheckSwitchAnchors();
	/** @brief A feature's switch's value (empty: unset, or no such switch); any thread after its registration. */
	const std::string& SwitchValue(Api::SwitchId a_switch);
	/** @brief Whether a feature's switch is set to "1". */
	bool SwitchEnabled(Api::SwitchId a_switch);
	/**
	 * @brief A feature's switch's value by its name (empty: unset, or no such switch); any thread after its registration. For a reader
	 * outside the feature that has no id (Core's held fade node reads Features/Fades' CS_DCLF_EMPTY_FADE_NODE).
	 */
	const std::string& RegisteredSwitchValue(std::string_view a_name);

	/** @brief The directory the switches file lives in (the SKSE log directory), outside MO2's virtual file system. */
	std::filesystem::path SwitchesDirectory();

	/**
	 * @brief Every switch that is set, as `NAME=value` pairs, for the startup log; a `CS_DCLF_*` name in the
	 * file or the environment that the registry does not know is listed as unknown, so a misspelt switch
	 * cannot silently do nothing.
	 */
	std::string SwitchSummary();

	/**
	 * @brief The feature switches set to something other than DCLF's full featureset, as `NAME=value` pairs, or
	 * empty when the run exercises all of it. Every feature defaults to on; the startup log names a reduced
	 * run so that a measurement or a validation cannot silently leave a feature out.
	 */
	std::string ReducedFeatures();
}
