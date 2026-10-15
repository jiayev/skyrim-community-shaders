// The Effect port's program family (Api::ProgramFamilyContribution), Core's built-in until Features/Effects registers it
// (Extensions::CoreProgramFamilies): its keys' programs are Effect.hlsl's, of the engine's BSEffectShader.

#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"

#include "Features/DrawcallLimitFix/Scene/Ports/EffectConstants.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

namespace DCLF::Extensions
{
	std::span<const Api::ProgramFamilyContribution> CoreProgramFamilies()
	{
		// Effect.hlsl's vertex and pixel stages, built forward or as a decal; its rows' technique and geometry blocks zero (its variants read
		// the frame's fog and camera planes, the object's World, PropertyColor and alpha reference); a first-person view draws it forward
		// only; a billboard leaf's mode in its spare key bits; External Emittance's suppress bit, the billboard bit and mode, and Blood's
		// reference (SetupGeometry's AlphaTestRef.x, tested or not) in its records.
		static const Api::ProgramFamilyContribution families[] = { {
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
			.programSource = []() -> const void* { return EngineShaders::Effect(); },
		} };
		return families;
	}
}
