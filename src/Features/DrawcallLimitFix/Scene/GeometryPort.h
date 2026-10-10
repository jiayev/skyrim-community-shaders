#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "ConstantEvaluator.h"

namespace DCLF::GeometryPort
{
	/**
	 * @brief The per-frame inputs of a pipeline's PerGeometry block (T6b2): what BSLightingShader::SetupGeometry (AE 0x1414dd040)
	 * writes that is neither the object's nor fixed by the pass descriptor, as the engine would compute it now. Sampled by the
	 * render thread at the frame's start (cheap global reads), after which nothing of the engine is read: PipelineGeometryConstants
	 * is a pure function of a descriptor and this.
	 *
	 * Only the main view's case: Community Shaders' world-space patch (Hooks.cpp, AE SetupGeometry+0x71 "and r15d, 0") is assumed,
	 * so the model-space branches (D3DXVec3TransformNormal of the sun, the transformed ambient) never run.
	 */
	struct PipelineFrame
	{
		// ShadowSceneNode[0]->sunLight with its NiLight: the template pass's sceneLights[0] (SceneStore::TemplatePassOf). Without it the
		// template is not evaluated either, and nothing below is meaningful.
		bool sun = false;
		// PS DirLightDirection (3): D3DXVec3Normalize(-NiDirectionalLight+0x140), through the engine's own import (0x1417502e0).
		float dirLightDirection[3]{};
		// PS DirLightColor (4): NiLight diffuse (+0x11c) x (NiLight fade +0x134 x ImageSpaceManager (*0x14332a6a0) +0xe0).
		float dirLightColor[3]{};
		// PS DirectionalAmbient (5): BSShaderManager::State+0xc8 (0x142033128, 12 floats: a 3x3 then a column), in the block's order:
		// the rows of the 3x3 with the column's entries as their w.
		float directionalAmbient[12]{};
		// VS EyePosition (2), xyz: the current accumulator's (0x14332a3a8, FUN_141480b20) eye (+0x16c) less posAdjust (shadow state
		// +0x35c, 0x14202aecc). eye false: no accumulator (the engine would fault; written by no pass then).
		bool eye = false;
		float eyePosition[3]{};
		// PS SSRParams (16), xyz: the Setting values at 0x142034e80 and 0x142034e98 (x, x + y; ConfigureR writes them), and the
		// auxiliary-target switch 0x14338ca08 (1 only inside FUN_1414b3390 / FUN_1414b3f20, the post-resolve-depth passes; 0 between).
		float ssrParams[3]{};
		// VS WorldMapOverlayParameters (8) and PS WorldMapOverlayParametersPS (17) for WorldMap passes (descriptor bit 18): 0x14332a4f0
		// and 0x14332a500 (FUN_140988940 / FUN_140989000, the world map's).
		float worldMapVS[4]{};
		float worldMapPS[4]{};
		// PS AmbientSpecularTintAndFresnelPower (6) for passes whose raw technique has AmbientSpecular (0x20000; every True PBR pass
		// gains it in True PBR's hook): not vanilla SetupGeometry's but SSE Engine Fixes' BSLightingAmbientSpecular fix, which patches
		// SetupGeometry before its Specular test (around 0x1414ddb5c) to copy BSShaderManager::State+0xfc (0x14203315c: the tint, then
		// the Fresnel power; written with the directional ambient by FUN_141480340) over PerGeometry PS 6. ambientSpecularFix: the
		// patch is there (SetupGeometry's bytes around the site differ from the engine's).
		bool ambientSpecularFix = false;
		float ambientSpecular[4]{};

		/**
		 * @brief Whether two frames give every pipeline the same block in what a DCLF_BINDLESS draw reads from it (SSRParams.xyz and
		 * the world map rows; the sun, the ambient and the eye are not read there: kPSBindlessGeometryUnread, DCLFFrameLighting).
		 * When true, no pipeline's posted constants change (only the frame lighting does).
		 */
		bool SamePipelineInputs(const PipelineFrame& a_other) const;
	};

	/** @brief This frame's (render thread, the frame's start, inside the engine-read window: SampleExtrasFrame's point). */
	PipelineFrame SamplePipelineFrame();

	/**
	 * @brief What the template evaluation (ConstantEvaluator::EvaluateGeometry on SceneStore::TemplatePassOf's pass) contributes to a
	 * pipeline's PerGeometry block, in the same ConstantBlock layout (LightingVSLayout / LightingPSLayout), kUnwrittenBits where
	 * SetupGeometry writes nothing:
	 * - per pipeline: PS NumLightNumShadowLight (0) xy, PS PointLightPosition / PointLightColor (1, 2) zeroed (the layout's scratch);
	 * - per frame (a_frame): VS EyePosition (2) where the pass writes it, VS / PS world map rows (8, 17) for WorldMap, PS
	 *   DirLightDirection, DirLightColor, DirectionalAmbient (3-5), PS SSRParams.xyz (16), and PS AmbientSpecularTintAndFresnelPower
	 *   (6) for AmbientSpecular passes where Engine Fixes' patch writes it (PipelineFrame::ambientSpecularFix).
	 * The per-object variables (kObjectVS / kObjectPS below, ObjectGeometryConstants') are left unwritten rather than taken from a
	 * template object. a_renderFlags: what the per-object outputs read (0x2, 0x8, 0x10), so nothing here; kept for the call sites.
	 * False when a_frame has no sun (the template pass would not exist).
	 */
	bool PipelineGeometryConstants(std::uint32_t a_passDescriptor, std::uint32_t a_renderFlags, const PipelineFrame& a_frame, GeometryConstants& a_out);

	/**
	 * @brief The frame lighting rows (FrameLighting, LightingConstants.h: DirLightDirection, DirLightColor, DirectionalAmbient,
	 * AmbientSpecularTintAndFresnelPower) the frame gives, into a_out where a_written (a bit per float) does not have them yet; the bits
	 * filled are added. MergeFrameLighting's contract, from the frame instead of an evaluation: rows 0 and 1 xyz, rows 2-4, and row 5
	 * (PS 6) where Engine Fixes' patch writes it: the same for every AmbientSpecular pass, so the frame's whichever pipelines exist.
	 */
	void MergeFrameLighting(const PipelineFrame& a_frame, std::array<float, 24>& a_out, std::uint32_t& a_written);

	// The PerGeometry variables the port leaves to the objects (ObjectGeometryConstants, FrameValues' rows, the extras): World,
	// PreviousWorld, LandBlendParams, TreeParams, WindTimers, TextureProj; MaterialData, EmitColor, ProjectedUVParams 1-3. SSRParams'
	// w alone is the object's (the port writes xyz).
	inline constexpr std::uint64_t kObjectVS = (1ull << 0) | (1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 5) | (1ull << 6);
	inline constexpr std::uint64_t kObjectPS = (1ull << 7) | (1ull << 8) | (1ull << 12) | (1ull << 13) | (1ull << 14);

	/**
	 * @brief The parity: where a port block and an evaluation (EvaluateGeometry on the synthetic template pass, numLights 1) differ in
	 * the port's domain (the PerGeometry variables but kObjectVS / kObjectPS and SSRParams.w), bit for bit, unwritten included; empty
	 * when they agree. a_ignoreLightCount: PS NumLightNumShadowLight.x left out (an evaluation on a registered pass, whose x is its own
	 * point lights, numLights - 1).
	 */
	std::string Differences(const GeometryConstants& a_port, const GeometryConstants& a_evaluated, bool a_ignoreLightCount = false);
}
