#pragma once

// The extension registry: what the features registered at install (Api::InstallContext), read-only after. Core reads it where each
// extension point runs (FrameValues, the scene's GPU setup and graph, the main build, the payload's publication and adoption).

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>
#include <string>

namespace DCLF::Extensions
{
	/**
	 * @brief Main thread, at PostPostLoad, before DCLF's threads start (the shell, before SceneScheduler::Graph and the hooks): every
	 * feature's install, in a_features' order. Once; a second call fails.
	 */
	void InstallFeatures(std::span<const Api::Feature> a_features);

	/** @brief A registered frame-value pool (its descriptor, the name owned). */
	struct FrameValuePoolEntry
	{
		std::string name;
		std::uint32_t stride = 0;
		decltype(Api::FrameValuePool::kick) kick = nullptr;
		decltype(Api::FrameValuePool::sample) sample = nullptr;
		decltype(Api::FrameValuePool::send) send = nullptr;
	};
	/** @brief A registered GPU stage. */
	struct GpuStageEntry
	{
		std::string passName;
		Api::GpuStageSlot slot = Api::GpuStageSlot::DepthSegmentBeforeBuild;
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

	// Any thread, after InstallFeatures (empty before): the registrations, in order. An index is the extension's id
	// (Api::FrameValuePoolId::index; the stage states' and products' slots).
	std::span<const FrameValuePoolEntry> RegisteredPools();
	std::span<const GpuStageEntry> RegisteredStages();
	std::span<const PayloadProductEntry> RegisteredProducts();
}
