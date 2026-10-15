// The Effect port's program family (Api::ProgramFamilyContribution): its keys' programs are Effect.hlsl's, of the engine's BSEffectShader.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		/**
		 * @brief The engine's BSEffectShader (the instance its GetRenderPasses makes passes of), null before it exists or when what is there
		 * is not an Effect shader. Any thread (a pointer read): the family's program source, read each time the lane's inputs are posted.
		 */
		const void* EffectShader()
		{
			static const REL::Relocation<RE::BSShader**> instance{ REL::Offset(Engine::Address::kEffectShader) };
			auto* shader = *instance.get();
			return shader && shader->shaderType.get() == RE::BSShader::Type::Effect ? shader : nullptr;
		}
	}

	void RegisterPrograms(Api::InstallContext& a_context)
	{
		// Effect.hlsl's vertex and pixel stages, built forward or as a decal; its rows' technique and geometry blocks zero (its variants read
		// the frame's fog and camera planes, the object's World, PropertyColor and alpha reference); a first-person view draws it forward
		// only; a billboard leaf's mode in its spare key bits; External Emittance's suppress bit, the billboard bit and mode, and Blood's
		// reference (SetupGeometry's AlphaTestRef.x, tested or not) in its records. After the port (its shader type's).
		a_context.RegisterProgramFamily({
			.family = Api::ProgramFamily::Effect,
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Effect),
			.name = "Effect",
			.keyName = "effect",
			.sourcePath = "Data/Shaders/Effect.hlsl",
			.nativeNote = "effect meshes stay native",
			.stages = Api::ProgramStages::VertexPixel,
			.build = Api::ProgramBuild::ForwardOrDecal,
			.rowBlocks = false,
			.rootViewsForwardOnly = true,
			.turnedKeyBits = true,
			.records = { .suppressEmittance = kRecordEffectSuppressEmittance,
				.turned = kRecordEffectBillboard,
				.turnedModeShift = kRecordEffectBillboardModeShift,
				.alphaRefPassBits = EffectFlag::Blood },
			.programSource = &EffectShader,
		});
	}
}
