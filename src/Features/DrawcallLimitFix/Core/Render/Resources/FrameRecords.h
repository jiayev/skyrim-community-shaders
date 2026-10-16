#pragma once

// The frame record and the frame-wide rows of a frame-constants buffer (Resources/FrameSlots.h's layout), written from this frame's
// sources: the one writer of every commit that draws with frame constants - the main commits (Payload/Uploads.cpp), the shadow commit's
// record (Shadow/ShadowEpochs.cpp) and a view's commit with frame constants of its own (CommitView: the reflection's faces). Render
// thread. Self-contained.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Resources/Buffers/Buffer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DCLF::Draws
{
	struct PayloadVersions;
	struct SceneBuffers;

	/** @brief FrameValues' buffers a frame record names (FrameValues::PlacementsIndex, PalettesIndex, ShadingIndex as the commit took them). */
	struct FrameValueIndices
	{
		std::uint32_t placements = 0, palettes = 0, shading = 0;
	};

	/**
	 * @brief A frame record's buffers: the installed payload's objects and extras (a_parts), FrameValues' placements, palettes and shading,
	 * and the features' frame bindings (Extensions::FrameBindingValuesOf of a_adopted at a_frame: Features/Fades' root states and rows
	 * among them). Every other register of a_record stands.
	 */
	void WriteRecordBuffers(DrawBindings& a_record, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted,
		const SceneBuffers& a_scene, std::uint32_t a_frame);

	/**
	 * @brief A frame record: the frame slots of a_frameConstants for the registers a_vsMask and a_psMask supply, PS b3 the shared light
	 * block and PS b13 the lighting slot, a_frameTextures at their registers (kInvalidIndex: a_nullIndex), then WriteRecordBuffers.
	 */
	DrawBindings FrameRecordOf(std::uint64_t a_frameConstants, std::uint32_t a_vsMask, std::uint32_t a_psMask, std::span<const std::uint32_t, kTextureRegisters> a_frameTextures,
		std::uint32_t a_nullIndex, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted, const SceneBuffers& a_scene,
		std::uint32_t a_frame);

	/** @brief The zeroed light block every bindless draw's PS b3 reads, staged into a_target once (a_zeroed). */
	void ZeroSharedLightOnce(CommitUploads& a_uploads, const std::shared_ptr<org::Buffer>& a_target, bool& a_zeroed);

	/**
	 * @brief The frame-wide rows into a_target: the frame lighting (PS b13), the LOD fades' frame inputs after it, the frame fog (VS b13),
	 * the extras' vertex part after the fog and pixel part at PS b13 c16, and the frame constant rows (PS b13 c17 and up), in ascending row
	 * order. This frame's: sampled now.
	 */
	void LatchFrameRows(LatchedUploads& a_latched, const std::shared_ptr<org::Buffer>& a_target);
	/** @brief Each write LatchFrameRows makes, in its order: a_add(a_context, bytes, offset) (a shape's latched copies). */
	void FrameRowWrites(void (*a_add)(void* a_context, std::size_t a_bytes, std::uint64_t a_offset), void* a_context);

	/** @brief Community Shaders' SharedData (PS b5) and FeatureData (PS b6) as it last uploaded them (empty: none). */
	std::span<const std::byte> SharedDataBlock();
	std::span<const std::byte> FeatureDataBlock();

	/**
	 * @brief A frame texture register's binding, kept while the register names the view it was resolved from (the volatile t16+ inputs
	 * keep a live import only that long). Returns its descriptor index (a_view null: a_nullIndex, the binding dropped); the binding's
	 * owner goes to a_owners, for the execution that reads it.
	 */
	std::uint32_t HoldFrameTexture(ID3D11ShaderResourceView*& a_heldView, GpuTextures::Binding& a_held, ID3D11ShaderResourceView* a_view, std::uint32_t a_register,
		std::uint32_t a_nullIndex, std::vector<std::shared_ptr<const void>>& a_owners);
}
