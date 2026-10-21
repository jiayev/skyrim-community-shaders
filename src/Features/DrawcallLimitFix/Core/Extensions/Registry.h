#pragma once

// The extension registry: what the features registered at install (Api::InstallContext), read-only after. Core reads it where each
// extension point runs (FrameValues, the scene's GPU setup, commits and graph, the frame records, the main build, the payload's
// publication and adoption, the primary cull's stand-in, the report). Switches and toggles register with Core/Settings, which owns them.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	/**
	 * @brief Main thread, at PostPostLoad, before DCLF's threads start (the shell, after DCLF's own engine hooks, before
	 * SceneScheduler::Graph and the shell's hooks): every feature's install, in a_features' order, then every integration's (Integrations/,
	 * written against Api/ like a feature: no early or source hooks), in a_integrations' order, then the resolutions, the frame's schedule
	 * (a_schedule: Schedule/Frame.cpp, which Core never includes) last, against Core's passes and the registered ones. Once; a second call
	 * fails.
	 */
	void InstallFeatures(std::span<const Api::Feature> a_features, std::span<const Api::Feature> a_integrations, const Api::FrameSchedule& a_schedule);
	/**
	 * @brief Main thread, at PostPostLoad, before DCLF's own engine hooks and observers install (the shell, before Diagnostics::Install):
	 * every feature's early hooks (Api::Feature::earlyHooks), in a_features' order. Once, before InstallFeatures.
	 */
	void InstallEarlyHooks(std::span<const Api::Feature> a_features);
	/**
	 * @brief Main thread, at PostPostLoad, where the shadow sources' hooks install (the shell, before the primary cull's): by source, in
	 * Api::ShadowSource order, every feature's source hooks for it (Api::Feature::sourceHooks, in a_features' order; Features/LocalLights'
	 * light selection at the Spot's: Extensions/ShadowViews.cpp). Once, before InstallFeatures.
	 */
	void InstallSourceHooks(std::span<const Api::Feature> a_features);
	/**
	 * @brief The frame's epoch orders, as RenderGraphRuntime::Segment values: the frame's (Api::FrameSchedule::frameOrder), the host's
	 * async epochs (asyncOrder) and the revision-driven ones among them (its DCLF epochs with revision).
	 */
	struct FrameOrders
	{
		std::vector<std::uint32_t> epochOrder, asyncOrder, revisionEpochs;
	};
	/**
	 * @brief Main thread, after InstallFeatures (empty before): the schedule's orders, resolved with it (the schedule's executor's), which
	 * the shell hands to RenderGraphRuntime::ConfigureFrame before the runtime initializes.
	 */
	const FrameOrders& ScheduledFrameOrders();

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
	/** @brief A registered payload product. */
	struct PayloadProductEntry
	{
		std::string name;
		decltype(Api::PayloadProduct::build) build = nullptr;
		decltype(Api::PayloadProduct::carried) carried = nullptr;
		decltype(Api::PayloadProduct::install) install = nullptr;
	};
	/**
	 * @brief The words the scene resources' postInputs post with the revision's inputs, all of them (each its inputWords, in registration
	 * order: SceneResourcesEntry::firstInput; RevisionInputs' SceneResourceInputs): grow it before more are registered.
	 */
	inline constexpr std::uint32_t kSceneResourceInputWords = 12;
	/** @brief Registered scene resources: its posted inputs at [firstInput, firstInput + inputWords) of the kSceneResourceInputWords. */
	struct SceneResourcesEntry
	{
		std::string name;
		decltype(Api::SceneResources::setup) setup = nullptr;
		decltype(Api::SceneResources::registerGraph) registerGraph = nullptr;
		decltype(Api::SceneResources::require) require = nullptr;
		decltype(Api::SceneResources::fit) fit = nullptr;
		decltype(Api::SceneResources::postInputs) postInputs = nullptr;
		std::uint32_t firstInput = 0, inputWords = 0;
		decltype(Api::SceneResources::latched) latched = nullptr;
		decltype(Api::SceneResources::sealed) sealed = nullptr;
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
	/** @brief A registered frame constant row. */
	struct FrameConstantRowEntry
	{
		std::string name;
		std::uint32_t row = ~0u;
		decltype(Api::FrameConstantRow::sample) sample = nullptr;
	};
	/** @brief A registered frame texture. */
	struct FrameTextureEntry
	{
		std::string name;
		std::uint32_t textureRegister = ~0u;
		decltype(Api::FrameTexture::view) view = nullptr;
		bool capturedWhenNull = false;
	};
	/** @brief A feature's registered report lines. */
	struct ReportLinesEntry
	{
		std::string name;
		Api::ReportPoint point = Api::ReportPoint::PrimaryCull;
		decltype(Api::ReportLines::lines) lines = nullptr;
		decltype(Api::ReportLines::text) text = nullptr;
		Api::CoordinatorComponentId component;
		decltype(Api::ReportLines::componentText) componentText = nullptr;
	};
	/** @brief A registered engine channel (its descriptor, the name owned). */
	struct EngineChannelEntry
	{
		std::string name;
		Api::ChannelKind kind = Api::ChannelKind::Keys;
		Api::ChannelApply apply = Api::ChannelApply::AfterSwitches;
		Api::ChannelLoading loading = Api::ChannelLoading::KeepKeys;
		Api::WakeSource wake = Api::WakeSource::None;
		bool namesKeys = false;
		decltype(Api::EngineChannel::drain) drain = nullptr;
		decltype(Api::EngineChannel::push) push = nullptr;
	};

	/** @brief A registered table family. */
	struct TableFamilyEntry
	{
		std::string name;
		decltype(Api::TableFamily::make) make = nullptr;
		decltype(Api::TableFamily::assign) assign = nullptr;
		decltype(Api::TableFamily::assignRow) assignRow = nullptr;
		decltype(Api::TableFamily::same) same = nullptr;
		decltype(Api::TableFamily::clear) clear = nullptr;
		decltype(Api::TableFamily::recycle) recycle = nullptr;
		std::uint32_t rowsEnd = ~0u, recordWord = ~0u;
		std::uint32_t recordShift = 0, recordBias = 0;
		bool recordTag = true, journaled = false;
		decltype(Api::TableFamily::rows) rows = nullptr;
	};
	/** @brief A registered coordinator component. */
	struct CoordinatorComponentEntry
	{
		std::string name;
		decltype(Api::CoordinatorComponent::make) make = nullptr;
		// Its hook sets, as registered.
		Api::CoordinatorComponent::Slots slots;
		Api::CoordinatorComponent::Residency residency;
		Api::CoordinatorComponent::Accumulate accumulate;
		Api::CoordinatorComponent::Walk walk;
		Api::CoordinatorComponent::DenseWalk dense;
		Api::CoordinatorComponent::Retirement retirement;
		Api::CoordinatorComponent::EntryClassHooks entryClass;
		Api::CoordinatorComponent::Events events;
		Api::CoordinatorComponent::Commit commit;
		Api::CoordinatorComponent::Adoption adoption;
	};
	/** @brief A registered shading contribution. */
	struct ShadingContributionEntry
	{
		std::string name;
		std::uint64_t propertyFlags = 0;
		decltype(Api::ShadingContribution::derive) derive = nullptr;
		std::uint32_t objectFlags = 0, blockWords = 0;
		std::span<const Api::ShadingVariable> vsVariables;
	};
	/** @brief A registered frame-globals contribution: its floats at FrameGlobals::features[first, first + floats). */
	struct FrameGlobalsEntry
	{
		std::string name;
		std::uint32_t first = 0, floats = 0;
		decltype(Api::FrameGlobalsContribution::capture) capture = nullptr;
	};

	// Any thread, after InstallFeatures (empty before): the registrations, in order. An index is the extension's id
	// (Api::FrameValuePoolId::index, Api::SceneResourcesId::index, Api::TableFamilyId::index, Api::CoordinatorComponentId::index,
	// Api::EngineChannelId::index; the products' and bindings' slots). The passes are the schedule's executor's
	// (Core/Render/Epochs/ScheduleExecutor.h). The technique contributions are by technique (Extensions/ObjectContributors.h).
	// The material contributors are read resolved (Extensions/Materials.h: MaterialContributors), the Lighting join rules as registered
	// (Extensions/ObjectContributors.h: RegisteredLightingJoinRules), the per-draw buffers by stage (Extensions/FrameBindings.h: PerDrawMask).
	std::span<const FrameValuePoolEntry> RegisteredPools();
	std::span<const PayloadProductEntry> RegisteredProducts();
	std::span<const SceneResourcesEntry> RegisteredSceneResources();
	std::span<const CommitHookEntry> RegisteredCommitHooks();
	std::span<const FrameBindingEntry> RegisteredFrameBindings();
	// The frame constant rows in ascending row order; the frame textures in registration order (Extensions::FrameTextureIndices names each
	// register's).
	std::span<const FrameConstantRowEntry> RegisteredFrameConstantRows();
	std::span<const FrameTextureEntry> RegisteredFrameTextures();
	std::span<const ReportLinesEntry> RegisteredReportLines();
	std::span<const Api::StandInObserver> RegisteredStandInObservers();
	std::span<const EngineChannelEntry> RegisteredEngineChannels();
	/**
	 * @brief The show veto's cells, at a fixed address for the session (the stubs load them): the registered veto's, else open a zero
	 * word and vetoes a function answering false (engineHid null, no hide sites). Whether one is registered: ShowVetoRegistered.
	 */
	const Api::ShowVeto& RegisteredShowVeto();
	bool ShowVetoRegistered();
	/** @brief The hidden sites reserved (InstallContext::ReserveHiddenSite): from HiddenSourceSites(), this many. */
	std::uint32_t ReservedHiddenSites();
	std::span<const TableFamilyEntry> RegisteredTableFamilies();
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents();
	std::span<const ShadingContributionEntry> RegisteredShadingContributions();
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals();
}
