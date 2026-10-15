#pragma once

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

namespace DCLF
{
	/**
	 * @brief The engine's Lighting shader instance DCLF's programs are of (a program family's source is the family's own:
	 * Api::ProgramFamilyContribution::programSource).
	 *
	 * The Lighting shader is BSLightingShader's instance (0x14338ca00, written by its constructor), as captured at a frame's start
	 * (FrameView::CaptureLightingShader): set once, on the render thread before the frame's inputs are posted, so the scene lane reads
	 * only what was set before it; null before the engine made it.
	 */
	class EngineShaders
	{
	public:
		static RE::BSShader* Lighting() { return lighting; }
		static void SetLighting(RE::BSShader* a_shader) { lighting = a_shader; }

	private:
		static inline RE::BSShader* lighting = nullptr;
	};
}
