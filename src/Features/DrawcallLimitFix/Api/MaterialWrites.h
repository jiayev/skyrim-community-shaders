#pragma once

// The material-write sink: what a feature's engine hooks tell Core when the engine writes a material or a property's shading in place
// (Features/Effects' effect controllers and material rewrite), so the coordinator's records follow the write (the material captured for
// them, MaterialPort::PushCapture) and the shading resampled. The frame patch of the ports' frame VS variable, which leaves a capture
// whose texture transforms alone moved to the frame (Features/TextureTransforms), is registered through the install context
// (InstallContext::RegisterFrameVSVariablePatch). Implemented by Core (Core/Shading/MaterialWrites.cpp, Core/Extensions/MaterialWrites.h).

namespace RE
{
	class BSShaderMaterial;
	class BSShaderProperty;
}

namespace DCLF::Api
{
	/**
	 * @brief Any thread, lock-free (a hook after the engine's write, where a_material is alive and readable): a_material's fields were
	 * written; it is captured for the coordinator's records. Null: nothing.
	 */
	void NoteMaterialWritten(const RE::BSShaderMaterial* a_material);
	/**
	 * @brief Any thread, lock-free (a hook after the engine's write): a_property's shading moved (a colour or a value its shading row
	 * reads, or its material's alpha); the next frame's shading sample resamples its dependents. Never null.
	 */
	void NotePropertyShading(const RE::BSShaderProperty* a_property);
}
