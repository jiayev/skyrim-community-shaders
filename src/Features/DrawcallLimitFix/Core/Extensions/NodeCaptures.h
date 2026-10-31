#pragma once

// The node capture contributions' Core side (Api/EngineEvents.h, NodeCaptureContribution): each registered class's entry, its RTTI
// resolved at registration (InstallContext::RegisterNodeCapture, during InstallFeatures; read-only after), which the scene capture reads
// as data for every node it records (NodeCaptureOf); and the push a feature's own hooks make for a contributed list's edit.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Extensions
{
	/** @brief A registered node capture contribution: its name, its class's RTTI (resolved) and its capture. */
	struct NodeCaptureEntry
	{
		std::string name;
		const void* rtti = nullptr;
		decltype(Api::NodeCaptureContribution::capture) capture = nullptr;
	};

	/**
	 * @brief Main thread, InstallFeatures (the registry's InstallContext::RegisterNodeCapture for a_feature): a_capture's entry. Fails the
	 * install for no name, RTTI or capture, or a class another contribution has.
	 */
	void AddNodeCapture(std::string_view a_feature, const Api::NodeCaptureContribution& a_capture);
	/** @brief Any thread, after InstallFeatures (none before): the contribution whose class has a_rtti exactly, or null. */
	const NodeCaptureEntry* NodeCaptureOf(const void* a_rtti);

	/**
	 * @brief A store of the feature's own to a_node's a_fields (Api::kNodeField* bits): the mirror's update of them, captured on the storing
	 * thread after the store (inside the engine-read window), in order with what that thread pushed before. Nothing for null.
	 * Core/Engine/Capture/SceneEvents.cpp.
	 */
	void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields);
}
