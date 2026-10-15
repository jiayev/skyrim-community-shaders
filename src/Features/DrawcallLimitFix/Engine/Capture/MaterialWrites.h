#pragma once

// The material write hooks: the shader-property controllers' updates and the materials' in-place rewrites, hooked so that every write
// captures the material for the coordinator's records (MaterialPort::PushCapture) and names the shading it moved. Any thread. The features'
// own write hooks (Features/Effects' effect controllers and material rewrite) push through the sink, Api/MaterialWrites.h.

#include <vector>

#include <ankerl/unordered_dense.h>

namespace DCLF::MaterialWrites
{
	/** @brief Installs the write hooks (Lighting's controllers, the engine's Lighting material vtables). */
	void Install();

	/** @brief A material's fields were written (any thread, lock-free). */
	void NoteWritten(const RE::BSShaderMaterial* a_material);

	/** @brief NoteWritten when the scope ends: for an in-place rewrite with early returns (CS's PBR materials). */
	struct NoteWrittenOnExit
	{
		const RE::BSShaderMaterial* material;
		~NoteWrittenOnExit() { NoteWritten(material); }
	};

	/**
	 * @brief Render thread, CS_DCLF_CAPTURE_PARITY's diagnostics alone (the capture check's drain, Diagnostics/Scene): every material written
	 * since the last call. The writes are queued only under that switch (the records follow the writes' captures). Always complete.
	 */
	bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out);
	void DrainShadingChanges(std::vector<const void*>& a_out);
	/** @brief The same events, for the kept shadow build (its one consumer): materials whose texture transform a controller moved. */
	void DrainShadowTransformChanges(std::vector<const RE::BSShaderMaterial*>& a_out);
}
