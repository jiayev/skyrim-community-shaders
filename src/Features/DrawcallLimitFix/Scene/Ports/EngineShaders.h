#pragma once

#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"

namespace DCLF
{
	/**
	 * @brief The engine's shader instances DCLF's programs and material ports are of.
	 *
	 * The Lighting shader is BSLightingShader's instance (0x14338ca00, written by its constructor), as captured at a frame's start
	 * (Coordinator::CaptureLightingShader): set once, on the render thread with the coordinator idle and before the scene work's kick,
	 * so the scene lane reads only what was set before it; null before the engine made it. The Effect shader is read where it is
	 * asked for.
	 */
	class EngineShaders
	{
	public:
		static RE::BSShader* Lighting() { return lighting; }
		static void SetLighting(RE::BSShader* a_shader) { lighting = a_shader; }

		/**
		 * @brief The engine's BSEffectShader (the instance its GetRenderPasses makes passes of), null before it exists or
		 * when what is there is not an Effect shader. Any thread (a pointer read).
		 */
		static RE::BSShader* Effect()
		{
			static const REL::Relocation<RE::BSShader**> instance{ REL::Offset(Engine::Address::kEffectShader) };
			auto* shader = *instance.get();
			return shader && shader->shaderType.get() == RE::BSShader::Type::Effect ? shader : nullptr;
		}

	private:
		static inline RE::BSShader* lighting = nullptr;
	};
}
