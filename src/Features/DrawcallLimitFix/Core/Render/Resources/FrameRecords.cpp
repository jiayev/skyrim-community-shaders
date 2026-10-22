// FrameRecords.h's writers: a frame record, the frame-wide rows of a frame-constants buffer and the frame buffers' copies, from this
// frame's sources (render thread).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

namespace DCLF::Draws
{
	void WriteRecordBuffers(DrawBindings& a_record, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted,
		const SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		a_record.textures[kObjectBufferRegister] = a_parts[PayloadPart::Objects].srvIndex;
		a_record.textures[kExtrasBufferRegister] = a_parts[PayloadPart::Extras].srvIndex;
		a_record.textures[kPlacementBufferRegister] = a_values.placements;
		a_record.textures[kPaletteBufferRegister] = a_values.palettes;
		a_record.textures[kShadingBufferRegister] = a_values.shading;
		Extensions::FrameBindingValues bindings;
		Extensions::FrameBindingValuesOf(a_adopted, a_scene, a_frame, bindings, true);
		// The root states and rows every draw's alpha reads are among them (t120/t119: Features/Fades').
		Extensions::WriteFrameBindings(bindings, a_record.textures);
	}

	DrawBindings FrameRecordOf(std::uint64_t a_frameConstants, std::uint32_t a_vsMask, std::uint32_t a_psMask, std::span<const std::uint32_t, kTextureRegisters> a_frameTextures,
		std::uint32_t a_nullIndex, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted, const SceneBuffers& a_scene,
		std::uint32_t a_frame)
	{
		DrawBindings record{};
		for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
			record.vertexConstants[r] = ((a_vsMask >> r) & 1) ? a_frameConstants + FrameSlotOffset(false, r) : 0;
			record.pixelConstants[r] = ((a_psMask >> r) & 1) ? a_frameConstants + FrameSlotOffset(true, r) : 0;
		}
		record.pixelConstants[3] = a_frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
		record.pixelConstants[kFrameLightingRegister] = a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
		for (std::uint32_t t = 0; t < kTextureRegisters; ++t)
			record.textures[t] = a_frameTextures[t] == kInvalidIndex ? a_nullIndex : a_frameTextures[t];
		WriteRecordBuffers(record, a_parts, a_values, a_adopted, a_scene, a_frame);
		return record;
	}

	void ZeroSharedLightOnce(CommitUploads& a_uploads, const std::shared_ptr<org::Buffer>& a_target, bool& a_zeroed)
	{
		if (a_zeroed)
			return;
		static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
		a_uploads(a_target, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		a_zeroed = true;
	}

	void LatchFrameRows(LatchedUploads& a_latched, const std::shared_ptr<org::Buffer>& a_target)
	{
		// The frame lighting, every commit: latching it only on change would change the frame's shape.
		const auto& frameCapture = Scene::FrameView::Get().GetFrameCapture();
		a_latched(a_target, frameCapture.lighting.data(), sizeof(frameCapture.lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		// The LOD fades' frame inputs after the lighting (PS b13, c6): the draws fade specular and envmap by distance.
		const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
		a_latched(a_target, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		// The frame's fog into VS b13 (DCLFFrameFog); the technique rows keep theirs.
		a_latched(a_target, frameCapture.fog.data(), sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		// The extras' frame inputs: vertex part after the fog (c3-c7), pixel part at PS b13 c16.
		const ExtrasFrame extrasFrame = SampleExtrasFrame();
		a_latched(a_target, &extrasFrame, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		a_latched(a_target, extrasFrame.projectedGlobals, sizeof(extrasFrame.projectedGlobals), kExtrasPixelFrameOffset);
		// The frame constant rows (PS b13 c17 and up, Api::FrameConstantRow: the Effect port's camera planes, light colour and blood),
		// in ascending row order, as FrameRowWrites lays them out.
		for (const auto& row : Extensions::RegisteredFrameConstantRows()) {
			float value[4]{};
			row.sample(value);
			a_latched(a_target, value, sizeof(value), FrameConstantRowOffset(row.row));
		}
	}

	void FrameRowWrites(void (*a_add)(void* a_context, std::size_t a_bytes, std::uint64_t a_offset), void* a_context)
	{
		a_add(a_context, sizeof(Scene::FrameView::FrameCapture::lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		a_add(a_context, sizeof(LodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		a_add(a_context, sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		a_add(a_context, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		a_add(a_context, sizeof(ExtrasFrame::projectedGlobals), kExtrasPixelFrameOffset);
		for (const auto& row : Extensions::RegisteredFrameConstantRows())
			a_add(a_context, sizeof(float[4]), FrameConstantRowOffset(row.row));
	}

	bool LatchFrameBuffer(LatchedUploads& a_latched, const FrameBuffer& a_frameBuffer, std::span<const std::byte> a_contents)
	{
		const std::size_t offset = std::size_t(a_frameBuffer.firstElement) * a_frameBuffer.stride;
		const std::size_t bytes = std::size_t(a_frameBuffer.elements) * a_frameBuffer.stride;
		if (a_contents.size() < offset + bytes) {
			static std::vector<std::byte> zeros;
			if (zeros.size() < bytes)
				zeros.resize(bytes);
			a_latched(a_frameBuffer.copy, zeros.data(), bytes, 0);
			return false;
		}
		a_latched(a_frameBuffer.copy, a_contents.data() + offset, bytes, 0);
		return true;
	}

	winrt::com_ptr<ID3D11Buffer> BoundPixelBuffer(std::uint32_t a_register)
	{
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		globals::d3d::context->PSGetShaderResources(a_register, 1, view.put());
		if (!view)
			return nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		view->GetDesc(&desc);
		if (desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		return resource.try_as<ID3D11Buffer>();
	}

	std::span<const std::byte> SharedDataBlock()
	{
		// Community Shaders' own, written through its ConstantBuffer helper (the constant mirror never sees it): the struct it uploaded.
		const auto* state = globals::state;
		return state ? std::as_bytes(std::span(&state->lastSharedData, 1)) : std::span<const std::byte>();
	}

	std::span<const std::byte> FeatureDataBlock()
	{
		const auto* state = globals::state;
		return state ? std::as_bytes(std::span(state->lastFeatureData)) : std::span<const std::byte>();
	}

	std::uint32_t HoldFrameTexture(ID3D11ShaderResourceView*& a_heldView, GpuTextures::Binding& a_held, ID3D11ShaderResourceView* a_view, std::uint32_t a_register,
		std::uint32_t a_nullIndex, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		if (!a_view) {
			if (a_heldView) {
				a_heldView = nullptr;
				a_held = {};
			}
			return a_nullIndex;  // the shared lookup root already owns the null descriptor
		}
		if (a_heldView != a_view || !a_held.owner || a_held.index == GpuTextures::kInvalid) {
			a_heldView = a_view;
			a_held = GpuTextures::Get().ResolveBinding(a_view, 64 + a_register);
		}
		if (a_held.owner)
			a_owners.push_back(a_held.owner);
		return a_held.index;
	}
}
