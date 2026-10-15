#pragma once

// The extension registry: what the features registered at install (Api::InstallContext), read-only after. Core reads it where each
// extension point runs (FrameValues, the scene's GPU setup, commits and graph, the frame records, the main build, the payload's
// publication and adoption, the primary cull's stand-in, the report). Switches and toggles register with Core/Settings, which owns them.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>
#include <string>

namespace DCLF::Extensions
{
	/**
	 * @brief Main thread, at PostPostLoad, before DCLF's threads start (the shell, after DCLF's own engine hooks, before
	 * SceneScheduler::Graph and the shell's hooks): every feature's install, in a_features' order. Once; a second call fails.
	 */
	void InstallFeatures(std::span<const Api::Feature> a_features);
	/**
	 * @brief Main thread, at PostPostLoad, before DCLF's own engine hooks and observers install (the shell, before Diagnostics::Install):
	 * every feature's early hooks (Api::Feature::earlyHooks), in a_features' order. Once, before InstallFeatures.
	 */
	void InstallEarlyHooks(std::span<const Api::Feature> a_features);
	/**
	 * @brief Main thread, at PostPostLoad, where the shadow cameras' hooks install (the shell, after SunAccumulation's, before the primary
	 * cull's): by source, in Api::ShadowSource order, every feature's camera hooks for it (Api::Feature::cameraHooks, in a_features'
	 * order), with Core's light selection right after the Sun's (Extensions/ShadowViews.cpp). Once, before InstallFeatures.
	 */
	void InstallCameraHooks(std::span<const Api::Feature> a_features);

	/** @brief A registered frame-value pool (its descriptor, the name owned). */
	struct FrameValuePoolEntry
	{
		std::string name;
		std::uint32_t stride = 0;
		decltype(Api::FrameValuePool::frameStart) frameStart = nullptr;
		decltype(Api::FrameValuePool::kick) kick = nullptr;
		decltype(Api::FrameValuePool::sample) sample = nullptr;
		decltype(Api::FrameValuePool::send) send = nullptr;
	};
	/** @brief A registered GPU stage. */
	struct GpuStageEntry
	{
		std::string passName;
		Api::GpuStageSlot slot = Api::GpuStageSlot::DepthSegmentBeforeBuild;
		Api::GpuQueue queue = Api::GpuQueue::Graphics;
		decltype(Api::GpuStage::setup) setup = nullptr;
		decltype(Api::GpuStage::makePass) makePass = nullptr;
		decltype(Api::GpuStage::latchWord) latchWord = nullptr;
	};
	/** @brief A registered payload product. */
	struct PayloadProductEntry
	{
		std::string name;
		decltype(Api::PayloadProduct::build) build = nullptr;
		decltype(Api::PayloadProduct::carried) carried = nullptr;
		decltype(Api::PayloadProduct::install) install = nullptr;
	};
	/** @brief Registered scene resources. */
	struct SceneResourcesEntry
	{
		std::string name;
		decltype(Api::SceneResources::setup) setup = nullptr;
		decltype(Api::SceneResources::registerGraph) registerGraph = nullptr;
		decltype(Api::SceneResources::require) require = nullptr;
		decltype(Api::SceneResources::fit) fit = nullptr;
	};
	/** @brief A registered commit hook. */
	struct CommitHookEntry
	{
		std::string name;
		decltype(Api::CommitHook::commit) commit = nullptr;
	};
	/** @brief A registered frame binding. */
	struct FrameBindingEntry
	{
		std::string name;
		std::uint32_t textureRegister = ~0u;
		decltype(Api::FrameBinding::select) select = nullptr;
	};
	/** @brief A feature's registered report lines. */
	struct ReportLinesEntry
	{
		std::string name;
		Api::ReportPoint point = Api::ReportPoint::PrimaryCull;
		decltype(Api::ReportLines::lines) lines = nullptr;
		decltype(Api::ReportLines::text) text = nullptr;
	};

	/** @brief A registered table family. */
	struct TableFamilyEntry
	{
		std::string name;
		decltype(Api::TableFamily::make) make = nullptr;
		decltype(Api::TableFamily::assign) assign = nullptr;
		decltype(Api::TableFamily::same) same = nullptr;
		decltype(Api::TableFamily::clear) clear = nullptr;
		decltype(Api::TableFamily::recycle) recycle = nullptr;
		std::uint32_t rowsEnd = ~0u, recordWord = ~0u;
	};
	/** @brief A registered coordinator component. */
	struct CoordinatorComponentEntry
	{
		std::string name;
		decltype(Api::CoordinatorComponent::make) make = nullptr;
		// Its hook sets, as registered.
		Api::CoordinatorComponent::Residency residency;
		Api::CoordinatorComponent::Accumulate accumulate;
		Api::CoordinatorComponent::Walk walk;
		Api::CoordinatorComponent::DenseWalk dense;
		Api::CoordinatorComponent::Retirement retirement;
		Api::CoordinatorComponent::EntryClassHooks entryClass;
	};
	/** @brief A registered shading contribution. */
	struct ShadingContributionEntry
	{
		std::string name;
		std::uint64_t propertyFlags = 0;
		decltype(Api::ShadingContribution::derive) derive = nullptr;
	};
	/** @brief A registered frame-globals contribution: its floats at FrameGlobals::features[first, first + floats). */
	struct FrameGlobalsEntry
	{
		std::string name;
		std::uint32_t first = 0, floats = 0;
		decltype(Api::FrameGlobalsContribution::capture) capture = nullptr;
	};

	// Any thread, after InstallFeatures (empty before): the registrations, in order. An index is the extension's id
	// (Api::FrameValuePoolId::index, Api::SceneResourcesId::index, Api::TableFamilyId::index, Api::CoordinatorComponentId::index; the
	// stage states', products' and bindings' slots). The technique contributions are by technique (Extensions/ObjectContributors.h).
	std::span<const FrameValuePoolEntry> RegisteredPools();
	std::span<const GpuStageEntry> RegisteredStages();
	std::span<const PayloadProductEntry> RegisteredProducts();
	std::span<const SceneResourcesEntry> RegisteredSceneResources();
	std::span<const CommitHookEntry> RegisteredCommitHooks();
	std::span<const FrameBindingEntry> RegisteredFrameBindings();
	std::span<const ReportLinesEntry> RegisteredReportLines();
	std::span<const Api::StandInObserver> RegisteredStandInObservers();
	std::span<const TableFamilyEntry> RegisteredTableFamilies();
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents();
	std::span<const ShadingContributionEntry> RegisteredShadingContributions();
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals();
}
