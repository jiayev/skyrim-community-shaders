#pragma once

// The frame binding contract (Api::InstallContext::RegisterFrameBinding): a buffer every DCLF draw reads at a declared texture register,
// given by the frame record (DrawBindings) because it changes by frame: the version of a feature's buffer the frame's draws read.

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/FrameBindings.h), which the features inside DCLF include for it.
	class FrameBindingSelect;

	/**
	 * @brief A frame binding.
	 *
	 * The register is the feature's compile-time constant, with its HLSL declaration; Features/Manifest.cpp checks that no two
	 * registers overlap, and the install that none is Core's or another binding's.
	 *
	 * - select: the render thread (the main and shadow commits' frame records) or the snapshot builder (the addresses a build embeds),
	 *   for a frame: the versioned buffer whose named version's descriptor the frame's draws read at the register (null: the register
	 *   keeps what the record gave it). It reads only handles made at the scene's setup, and the frame: the binding may alternate by
	 *   frame (the named-address parity takes either of two consecutive frames' as right).
	 */
	struct FrameBinding
	{
		std::string_view name;  // the named-address parity's name for it
		std::uint32_t textureRegister = ~0u;
		void (*select)(FrameBindingSelect& a_select) = nullptr;
	};
}
