// The node capture contributions' entries (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"

#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// Written on the main thread during InstallFeatures, read-only after: the captures that read it start with the world's load.
		std::vector<NodeCaptureEntry> nodeCaptures;
	}

	void AddNodeCapture(std::string_view a_feature, const Api::NodeCaptureContribution& a_capture)
	{
		const void* rtti = a_capture.rtti ? a_capture.rtti() : nullptr;
		if (a_capture.name.empty() || !rtti || !a_capture.capture)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} registered an incomplete node capture contribution", a_feature));
		for (const auto& entry : nodeCaptures)
			if (entry.rtti == rtti)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s node capture {} names the class node capture {} has", a_feature, a_capture.name, entry.name));
		nodeCaptures.push_back({ std::string(a_capture.name), rtti, a_capture.capture });
	}

	const NodeCaptureEntry* NodeCaptureOf(const void* a_rtti)
	{
		for (const auto& entry : nodeCaptures)
			if (a_rtti && entry.rtti == a_rtti)
				return &entry;
		return nullptr;
	}
}
