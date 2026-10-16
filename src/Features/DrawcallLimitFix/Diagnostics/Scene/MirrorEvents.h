#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <span>

namespace DCLF::Diagnostics
{
	// The scene mirror's writes (SceneMirror, the coordinator's), for the mirror parity's observers (Events.h's other hook points name
	// Coordinator; these do not, so SceneMirror.cpp includes this alone).

	/** @brief The coordinator: a capture's records applied to the mirror. */
	struct MirrorCaptureApplied
	{
		const SceneCapture::Records& records;
	};

	/** @brief The coordinator: a detached subtree's records taken out of the mirror. */
	struct MirrorDetached
	{
		std::span<RE::BSGeometry* const> geometries;
		std::span<const void* const> nodes;
	};

	/** @brief The coordinator: every record of the mirror dropped. */
	struct MirrorCleared
	{
	};

	/** @brief A reference's 3D placed in its cell (the engine's thread): its fade node's statics just set. */
	struct FadeStaticsPlaced
	{
		const RE::NiAVObject* fade;
	};
}
