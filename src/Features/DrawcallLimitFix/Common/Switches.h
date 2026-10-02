#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace DCLF
{
	/**
	 * @brief Every `CS_*` switch DCLF (and the few other features that share its switches file) reads. The
	 * registry in Switches.cpp holds each one's name, kind and description, in this order.
	 */
	enum class Switch : std::uint8_t
	{
		// Features: unset is DCLF's full featureset; the startup log names a run that narrows one.
		Dclf,
		Async,
		AsyncWaitMs,
		AsyncPriority,
		Precompile,
		Cull,
		Ownership,
		ShadowOwnership,
		Skinned,
		SkinPartitions,
		Actors,
		Trees,
		Decals,
		Fading,
		LodCrossfade,
		MtLand,
		ProjectedUv,
		SwitchNodes,
		Shadows,
		SunSkip,
		SunExclude,
		PrimaryExclude,
		ListFilter,
		TreeList,
		LightExclude,
		LightList,
		MoveEvents,
		HiddenEvents,
		Skylight,
		Precipitation,
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
		FadeParity,
		ResidentDrawParity,
		BuildParity,
		SetParity,
		BindlessParity,
		CaptureParity,
		CapturePointParity,
		SkylightParity,
		ClassifyCache,
		DerivedCache,
		MaterialCache,

		// Diagnostics: stats, shader debugging, and the probes of open investigations (dclf-open-defects.md).
		Stats,
		PassStats,
		Profile,
		ShaderDebug,
		ShaderSourceDir,
		Eval,
		TraceTexturePaths,
		CoverageProbe,
		DeriveProbe,
		SlotProbe,
		DecalOrderProbe,
		HiddenWatch,
		InputWatch,
		DecalOrder,
		TableStart,
		SkylightDumpDir,
		DepthTrace,
		DrawTrace,
		TargetProbe,
		ShadowMaskProbe,
		ShadowMapProbe,
		GBufferProbe,
		ShadowDebugOutput,

		// Test harness: scripted input for unattended runs.
		TestCommands,
		TestMove,
		TestTurn,
		TestToggle,

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
