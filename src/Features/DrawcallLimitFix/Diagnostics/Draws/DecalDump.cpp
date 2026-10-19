// TEMPORARY (decal loss): CS_DCLF_DECAL_DUMP=<frame>. Once per session, at the first colour main epoch at or after that frame
// (counted as CS_DCLF_TARGET_DUMP counts them: Presents without a loading screen), every per-object input the pixel stage of each
// DCLF decal (kObjectDecal) reads, one "[DCLF] DECAL-DUMP " line per item, by object index, so two sessions' logs can be diffed.
// Render thread (MainEpochDone, after the colour epoch's execution, inside the engine-read window). Reads the committed colour payload's
// rows (the bytes uploaded: MainBody::materialRows, pipelineRows; header addresses are row offsets there), the installed tables and
// lookups (FrameView's), the frame values' shading rows (only once the frame's producer is done: never waited for), the bindless
// texture imports' views (only where the lookup entry's owners pin them) and the engine's geometry and material for names.
// Pointer values go on separate "ptr" lines; every other line should compare equal between sessions where the inputs are equal (bindless
// heap indices excepted: they are allocation order).

#include "DrawsState.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "RE/B/BSEffectShaderMaterial.h"
#include "RE/B/BSLightingShaderMaterialBase.h"
#include "RE/B/BSTextureSet.h"
#include "RE/N/NiSourceTexture.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>

namespace DCLF::Diagnostics
{
	namespace
	{
		constexpr const char* kDumpPrefix = "[DCLF] DECAL-DUMP ";
		// Colour epochs a dump waits for consistent inputs (the payload's tables and lookups the installed ones, the shading rows done).
		constexpr std::uint32_t kDumpMaxTries = 60;

		struct DumpState
		{
			std::uint32_t wanted = 0;  // 0: off
			std::uint32_t presents = 0;
			std::uint32_t tries = 0;
			bool done = false;
		};
		DumpState& DumpStateOf()
		{
			static DumpState state = [] {
				DumpState s;
				if (const char* value = std::getenv("CS_DCLF_DECAL_DUMP"))
					s.wanted = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
				return s;
			}();
			return state;
		}

		void DumpLine(const std::string& a_text) { logger::info("{}{}", kDumpPrefix, a_text); }

		/** @brief a_bytes as hex words, trailing zero words dropped: "n=<words kept>/<words> w0 w1 ...". */
		std::string DumpHex(const std::byte* a_bytes, std::size_t a_size)
		{
			const std::size_t words = a_size / 4;
			std::size_t kept = words;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_bytes + i * 4, 4);
				return w;
			};
			while (kept && word(kept - 1) == 0)
				--kept;
			std::string out = fmt::format("n={}/{}", kept, words);
			for (std::size_t i = 0; i < kept; ++i)
				out += fmt::format(" {:08x}", word(i));
			return out;
		}

		/** @brief a_bytes as floats, trailing zero words dropped. */
		std::string DumpFloats(const std::byte* a_bytes, std::size_t a_size)
		{
			const std::size_t words = a_size / 4;
			std::size_t kept = words;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_bytes + i * 4, 4);
				return w;
			};
			while (kept && word(kept - 1) == 0)
				--kept;
			std::string out = fmt::format("n={}/{}", kept, words);
			for (std::size_t i = 0; i < kept; ++i)
				out += fmt::format(" {:.6g}", std::bit_cast<float>(word(i)));
			return out;
		}

		std::uint64_t DumpFnv(const std::byte* a_bytes, std::size_t a_size)
		{
			std::uint64_t hash = 14695981039346656037ull;
			for (std::size_t i = 0; i < a_size; ++i)
				hash = (hash ^ std::to_integer<std::uint8_t>(a_bytes[i])) * 1099511628211ull;
			return hash;
		}

		const char* DumpNameOf(const RE::NiAVObject* a_object)
		{
			if (!a_object)
				return "<none>";
			const char* name = a_object->name.c_str();
			return name && *name ? name : "<unnamed>";
		}

		ID3D11ShaderResourceView* DumpViewOf(const RE::NiSourceTexture* a_texture)
		{
			const auto* texture = a_texture ? a_texture->rendererTexture : nullptr;
			return texture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->resourceView) : nullptr;
		}

		const char* DumpTextureName(const RE::NiSourceTexture* a_texture)
		{
			if (!a_texture)
				return "<none>";
			const char* name = a_texture->name.c_str();
			return name && *name ? name : "<unnamed>";
		}

		/** @brief The engine textures an SRV may be: the material's own (by role), the engine's defaults. Render thread. */
		struct DumpKnownViews
		{
			std::vector<std::pair<ID3D11ShaderResourceView*, std::string>> views;

			void Add(ID3D11ShaderResourceView* a_view, std::string a_label)
			{
				if (a_view)
					views.emplace_back(a_view, std::move(a_label));
			}
			std::string Label(ID3D11ShaderResourceView* a_view) const
			{
				if (!a_view)
					return "null-view";
				std::string out;
				for (const auto& [view, label] : views)
					if (view == a_view)
						out += (out.empty() ? "" : "|") + label;
				return out.empty() ? "unknown" : out;
			}
		};

		void DumpEngineDefaults(DumpKnownViews& a_known)
		{
			if (const auto* graphicsState = globals::game::graphicsState) {
				const auto& state = graphicsState->GetRuntimeData();
				a_known.Add(DumpViewOf(state.defaultTextureBlack.get()), "default:black");
				a_known.Add(DumpViewOf(state.defaultTextureWhite.get()), "default:white");
				a_known.Add(DumpViewOf(state.defaultTextureGrey.get()), "default:grey");
				a_known.Add(DumpViewOf(state.defaultTextureNormalMap.get()), "default:normal");
				a_known.Add(DumpViewOf(state.defaultTextureDitherNoiseMap.get()), "default:dither");
			}
		}

		/** @brief An SRV's desc and its resource's, where the view is pinned (alive). */
		std::string DumpDescribeView(ID3D11ShaderResourceView* a_view)
		{
			if (!a_view)
				return "view=none";
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			std::string out = fmt::format("srvFmt={} srvDim={}", static_cast<int>(desc.Format), static_cast<int>(desc.ViewDimension));
			if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
				out += fmt::format(" srvMips={}+{}", desc.Texture2D.MostDetailedMip, desc.Texture2D.MipLevels);
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			if (auto texture = resource.try_as<ID3D11Texture2D>()) {
				D3D11_TEXTURE2D_DESC t{};
				texture->GetDesc(&t);
				out += fmt::format(" tex={}x{} mips={} array={} fmt={} usage={} bind={:#x} misc={:#x}", t.Width, t.Height, t.MipLevels, t.ArraySize, static_cast<int>(t.Format),
					static_cast<int>(t.Usage), t.BindFlags, t.MiscFlags);
			} else
				out += " tex=not-2d";
			return out;
		}
	}

	// TEMPORARY (decal loss): the dump itself (see the file comment).
	void DumpDecals(const MainEpochDone& a_epoch)
	{
		auto& s = DumpStateOf();
		if (s.done || !s.wanted || a_epoch.depthOnly || s.presents < s.wanted)
			return;
		const auto& payload = a_epoch.payload;
		const auto& body = *payload.body;
		auto& view = Scene::FrameView::Get();
		const auto& tables = view.GetTables();
		const auto& lookups = view.GetLookups();
		const auto* shadingRows = FrameValues::Get().ShadingIfDone(false);
		// Consistent inputs: the payload built against the installed tables and lookups, the shading rows done. Else the next colour epoch.
		const bool consistent = payload.inputs.tablesGeneration == view.GetTablesGeneration() && payload.inputs.lookupGeneration == lookups.generation;
		if ((!consistent || !shadingRows) && ++s.tries < kDumpMaxTries)
			return;
		s.done = true;

		DumpLine(fmt::format("BEGIN presents={} wanted={} engineFrame={} tries={} consistent={} payloadTablesGen={} tablesGen={} payloadLookupGen={} lookupGen={} "
						 "lookupSharedVersion={} nullTexture={} textureGen={} shading={} objects={} materials={} pipelines={} materialRows={} pipelineRows={} "
						 "decalCount={},{},{} decalsDrawn={}",
			s.presents, s.wanted, view.GetFrame(), s.tries, consistent, payload.inputs.tablesGeneration, view.GetTablesGeneration(), payload.inputs.lookupGeneration,
			lookups.generation, lookups.sharedVersion, lookups.nullTexture, GpuTextures::Get().Generation(), shadingRows ? "done" : "unavailable", tables.objects.size(),
			tables.materials.size(), tables.pipelines.size(), body.materialRows.Count(), body.pipelineRows.Count(), body.decalCount[0], body.decalCount[1], body.decalCount[2],
			body.decalsDrawn));
		{
			std::string samplers;
			for (const auto sampler : lookups.samplers)
				samplers += fmt::format(" {}", sampler == Lookups::kNone ? -1 : static_cast<std::int64_t>(sampler));
			DumpLine(fmt::format("shared samplers(addr*5+filter):{} projected={},{},{},{}", samplers, static_cast<std::int32_t>(lookups.projectedTextures[0]),
				static_cast<std::int32_t>(lookups.projectedTextures[1]), static_cast<std::int32_t>(lookups.projectedTextures[2]),
				static_cast<std::int32_t>(lookups.projectedTextures[3])));
		}

		// The colour shape's class map (set index -> class << 16 | index in class), as BuildParity reads it.
		const auto& active = DrawsView::Get().revisions.active;
		const auto shapeEpoch = Draws::EpochOfShape(Draws::ShapeKind::Main, static_cast<std::uint32_t>(kColourShape));
		const auto shape = active ? active->Get<PassFrame>(DrawsView::Impl::SceneRevisions::kShapeSlot + shapeEpoch) : nullptr;
		const std::vector<std::uint32_t>* classOf = shape ? &shape->indirect.classOf : nullptr;

		// The payload's inputs by object (the resident region's, then the loop's).
		std::unordered_map<std::uint32_t, Draws::DrawInput> inputs;
		auto collect = [&](const Draws::DrawInput& a_input) {
			if (a_input.objectIndex < tables.objects.size() && (tables.objects[a_input.objectIndex].flags & kObjectDecal))
				inputs.emplace(a_input.objectIndex, a_input);
		};
		for (std::size_t i = 0; i < payload.resident.Count(); ++i)
			collect(*payload.resident.At(i));
		for (const auto& input : body.inputList)
			collect(input);

		DumpKnownViews defaults;
		DumpEngineDefaults(defaults);
		const auto rootPhases = Extensions::Views().rootPhases;
		std::uint32_t decals = 0;

		for (std::uint32_t o = 0; o < tables.objects.size(); ++o) {
			const auto& object = tables.objects[o];
			if (!(object.flags & kObjectDecal) || (object.flags & kObjectFree))
				continue;
			++decals;
			const std::string tag = fmt::format("o={} ", o);
			const std::uint32_t group = ObjectDecalGroup(object.flags);
			const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
			const bool noBindings = (object.flags & kObjectNoBindings) != 0;
			const bool inSet = (object.flags & kObjectMember) || (o < tables.setPhases.size() && (tables.setPhases[o] & rootPhases));
			const std::uint32_t ordinal = o < tables.rangeOrdinal.size() ? tables.rangeOrdinal[o] : ~0u;
			const RE::BSGeometry* geometry = o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr;

			// The object and its names.
			{
				const RE::NiAVObject* parent = geometry ? geometry->parent : nullptr;
				const RE::NiAVObject* grand = parent ? parent->parent : nullptr;
				const RE::NiAVObject* top = parent;
				for (std::uint32_t depth = 0; top && top->parent && depth < 64; ++depth)
					top = top->parent;
				const std::uint8_t state = o < body.objectState.size() ? body.objectState[o] : 0xEE;
				DumpLine(tag + fmt::format("object name=\"{}\" parent=\"{}\" grand=\"{}\" top=\"{}\" flags={:08x} group={} ordinal={} inSet={} member={} noBindings={} "
									   "setPhases={:#x} resident={} alphaThreshold={} objectState={:#x} geometryIndex={}",
										DumpNameOf(geometry), DumpNameOf(parent), DumpNameOf(grand), DumpNameOf(top), object.flags, group, static_cast<std::int32_t>(ordinal), inSet,
										(object.flags & kObjectMember) != 0, noBindings, o < tables.setPhases.size() ? tables.setPhases[o] : 0,
										o < tables.residentSlot.size() ? tables.residentSlot[o] : 0, (object.flags >> kObjectAlphaThresholdShift) & 0xFF, state,
										object.geometryIndex));
				DumpLine(tag + fmt::format("ptr geometry={} parent={} top={}", static_cast<const void*>(geometry), static_cast<const void*>(parent), static_cast<const void*>(top)));
			}

			// Its draw input and its decal slot's template (row addresses are offsets into the row tables there).
			if (const auto found = inputs.find(o); found != inputs.end()) {
				const auto& in = found->second;
				DumpLine(tag + fmt::format("input set={} classWord={:#x} record={:#x} geometry={} flags={:08x} drawable={} viewMask={:#x} decalOrdinal={} partitions={:#x} "
									   "stream={} fadeRoot={} lodPartitions={:#x}",
										static_cast<std::int32_t>(in.pipelineIndex),
										classOf && in.pipelineIndex < classOf->size() ? (*classOf)[in.pipelineIndex] : kNoSetClass, in.recordIndex,
										static_cast<std::int32_t>(in.geometryIndex), in.flags, (in.flags & Draws::kInputDrawable) != 0, in.view.mask,
										static_cast<std::int32_t>(in.decalOrdinal), in.partitions, static_cast<std::int32_t>(in.streamIndex), static_cast<std::int32_t>(in.fadeRoot),
										in.lodPartitions));
			} else
				DumpLine(tag + "input none");
			if (group && ordinal < body.decalTemplates[group - 1].size()) {
				const auto& t = body.decalTemplates[group - 1][ordinal];
				DumpLine(tag + fmt::format("template set={} pipelineRowOffset={:#x} materialRowOffset={:#x} objectWord={:#x} vbSize={} stride={} streamSize={} streamStride={} "
									   "ibSize={} indexFormat={} indexCount={} instances={} firstIndex={} vertexOffset={} firstInstance={}",
										+t.pipelineIndex, +t.pipelineRowAddress, +t.materialRowAddress, +t.objectIndex, +t.vertexBufferSize, +t.vertexStride, +t.streamBufferSize,
										+t.streamStride, +t.indexBufferSize, +t.indexFormat, +t.indexCount, +t.instanceCount, +t.firstIndex, +t.vertexOffset, +t.firstInstance));
				// (DrawSequence is packed: its fields go by value, never bound to a reference.)
				DumpLine(tag + fmt::format("ptr template vb={:#x} stream={:#x} ib={:#x}", +t.vertexBufferAddress, +t.streamBufferAddress, +t.indexBufferAddress));
			} else
				DumpLine(tag + fmt::format("template none (group {} ordinal {} of {})", group, static_cast<std::int32_t>(ordinal),
								   group ? body.decalTemplates[group - 1].size() : 0));

			// The object record (BuildObjectRecord from the installed tables: what the stream views upload for it) and its shading row.
			{
				BindlessObject record{};
				BuildObjectRecord(tables, o, GeometryPort::kMainPassRenderFlags, record);
				std::string block;
				for (const auto word : record.featureBlock)
					block += fmt::format(" {:08x}", word);
				DumpLine(tag + fmt::format("record ancestor={} recordFlags={:08x} (fadeRoot+1={}) alphaTestRef={:.6g} lodFades={:#x} boneOffset={} prevBoneOffset={} boneRows={} "
									   "extraOffset={} featureBlock:{}",
										record.ancestorValue, record.recordFlags, record.recordFlags >> kRecordFadeRootShift, record.alphaTestRef, record.lodFades,
										record.boneOffset, record.previousBoneOffset, record.boneRows, record.extraOffset, block));
			}
			if (shadingRows && o < shadingRows->size()) {
				const auto& row = (*shadingRows)[o];
				const auto& sh = row.shading;
				DumpLine(tag + fmt::format("shading materialData={:.6g},{:.6g},{:.6g},{:.6g} emitColor={:.6g},{:.6g},{:.6g} ssrSpecular={:.6g} skin={:.6g},{:.6g},{:.6g},{:.6g} "
									   "emissiveMult={:.6g} written={:#x}",
										sh.materialData[0], sh.materialData[1], sh.materialData[2], sh.materialData[3], sh.emitColor[0], sh.emitColor[1], sh.emitColor[2],
										sh.ssrSpecular, row.skinPerGeometry[0], row.skinPerGeometry[1], row.skinPerGeometry[2], row.skinPerGeometry[3], row.emissiveMult,
										row.written));
				DumpLine(tag + "shading.words " + DumpHex(reinterpret_cast<const std::byte*>(&row), sizeof(row)));
			} else
				DumpLine(tag + fmt::format("shading none (rows {})", shadingRows ? shadingRows->size() : 0));

			if (noBindings) {
				DumpLine(tag + "bindings none (kObjectNoBindings)");
				continue;
			}

			// The pipeline: key, lookup entry, its row as uploaded.
			if (p < tables.pipelines.size()) {
				const auto& key = tables.pipelines[p];
				const Lookups::Pipeline* entry = p < lookups.pipelines.size() ? &lookups.pipelines[p] : nullptr;
				const std::uint32_t set = entry ? entry->setIndex : Lookups::kNone;
				const std::uint32_t word = classOf && set < classOf->size() ? (*classOf)[set] : kNoSetClass;
				DumpLine(tag + fmt::format("pipeline slot={} VS={:08x} PS={:08x} pass={:08x} raster={:08x} layout={:016x} used={} set={} class={} classIndex={} entryKeyMatch={} "
									   "entryVersion={} shadowMaskIndex={}",
										p, key.vertexDescriptor, key.pixelDescriptor, key.passDescriptor, key.rasterFlags, key.vertexLayout, tables.PipelineUsed(p),
										static_cast<std::int32_t>(set), word == kNoSetClass ? -1 : static_cast<std::int32_t>(word >> kSetClassShift),
										word == kNoSetClass ? -1 : static_cast<std::int32_t>(word & kSetClassIndexMask), entry && entry->key == key, entry ? entry->version : 0,
										entry ? static_cast<std::int32_t>(entry->shadowMaskIndex) : -1));
				if (entry) {
					const auto& colour = entry->usage[kColorVariant];
					DumpLine(tag + fmt::format("pipeline.usage(colour) vsCB={:#x} psCB={:#x} tex={:016x}{:016x} samplers={:#x}", colour.vertexConstants, colour.pixelConstants,
								   colour.textures[1], colour.textures[0], colour.samplers));
				}
				if (const auto* row = body.pipelineRows.At(p)) {
					const auto& header = *reinterpret_cast<const PipelineRowHeader*>(row->bytes.data() + kPipelineRowHeader);
					DumpLine(tag + fmt::format("pipeline.header vsTech={:#x} psTech={:#x} vsGeom={:#x} psGeom={:#x} vsPerm={:#x} psPerm={:#x} shadowMask={} shadowMaskSampler={} "
										   "vertexLayout={:016x}",
											header.vsTechnique, header.psTechnique, header.vsGeometry, header.psGeometry, header.vsPermutation, header.psPermutation,
											header.shadowMask, header.shadowMaskSampler, header.vertexLayout));
					const auto* bytes = row->bytes.data();
					DumpLine(tag + "pipeline.techVS " + DumpHex(bytes + kPipelineRowTechniqueVS, kPipelineRowTechniqueBytes));
					DumpLine(tag + "pipeline.techPS " + DumpHex(bytes + kPipelineRowTechniquePS, kPipelineRowTechniqueBytes));
					DumpLine(tag + "pipeline.geomVS " + DumpHex(bytes + kPipelineRowGeometryVS, kPipelineRowGeometryVSBytes));
					DumpLine(tag + "pipeline.geomPS " + DumpHex(bytes + kPipelineRowGeometryPS, kPipelineRowGeometryPSBytes));
					DumpLine(tag + "pipeline.geomPS.f " + DumpFloats(bytes + kPipelineRowGeometryPS, kPipelineRowGeometryPSBytes));
					DumpLine(tag + "pipeline.permutation " + DumpHex(bytes + kPipelineRowPermutation, 32));
					DumpLine(tag + fmt::format("pipeline.blocksHash={:016x}", DumpFnv(bytes, kPipelineRowHeader)));
				} else
					DumpLine(tag + fmt::format("pipeline.row none (rows {})", body.pipelineRows.Count()));
			} else
				DumpLine(tag + fmt::format("pipeline slot={} past the tables ({})", p, tables.pipelines.size()));

			// The material: slot key, record, lookup entry, its row as uploaded.
			if (m >= tables.materials.size()) {
				DumpLine(tag + fmt::format("material slot={} past the tables ({})", m, tables.materials.size()));
				continue;
			}
			const auto& record = tables.materials[m];
			const auto slotKey = m < tables.materialSlotKey.size() ? tables.materialSlotKey[m] : std::pair<const RE::BSShaderMaterial*, std::uint32_t>{};
			const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
			const Lookups::MaterialOwners* owners = lookup ? lookup->bindingBlock.get() : nullptr;
			const RE::BSShaderProperty* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const RE::BSShaderMaterial* liveMaterial = property ? property->material : nullptr;
			{
				std::string address, filter;
				for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
					address += fmt::format("{}{}", t ? "," : "", record.addressModes[t]);
					filter += fmt::format("{}{}", t ? "," : "", static_cast<std::int32_t>(record.filterModes[t]));
				}
				DumpLine(tag + fmt::format("material slot={} pass={:08x} kind={} textureWritten={:#06x} materialVersion={} frameVersion={} propertyMaterialIsSlotKey={} "
									   "materialType={} addressModes={} filterModes={}",
										m, slotKey.second, record.kind, record.textureWritten, m < tables.materialVersion.size() ? tables.materialVersion[m] : 0,
										m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0, liveMaterial == slotKey.first,
										liveMaterial ? static_cast<int>(liveMaterial->GetType()) : -1, address, filter));
				DumpLine(tag + fmt::format("material.lookup present={} resolved={} keyMatch={} version={} written={:#x} texturesGeneration={} recordVersion={} owners={}",
								   lookup != nullptr, lookup && lookup->resolved,
								   lookup && lookup->key.first == slotKey.first && lookup->key.second == slotKey.second, lookup ? lookup->version : 0,
								   lookup ? lookup->written : 0, lookup ? lookup->texturesGeneration : 0, lookup ? lookup->recordVersion : 0, owners != nullptr));
				DumpLine(tag + fmt::format("ptr material slotKey={} property={} propertyMaterial={} lookupKey={} bindingBlock={}", static_cast<const void*>(slotKey.first),
								   static_cast<const void*>(property), static_cast<const void*>(liveMaterial), lookup ? static_cast<const void*>(lookup->key.first) : nullptr,
								   static_cast<const void*>(owners)));
				DumpLine(tag + "material.recordPS " + DumpFloats(reinterpret_cast<const std::byte*>(record.ps.floats.data()), sizeof(record.ps.floats)));
				DumpLine(tag + "material.recordVS " + DumpFloats(reinterpret_cast<const std::byte*>(record.vs.floats.data()), sizeof(record.vs.floats)));
			}

			// The engine material's textures (the live property's: names and paths) for the views' labels.
			DumpKnownViews known = defaults;
			if (liveMaterial && liveMaterial->GetType() == RE::BSShaderMaterial::Type::kLighting) {
				const auto* lighting = static_cast<const RE::BSLightingShaderMaterialBase*>(liveMaterial);
				const std::pair<const char*, const RE::NiSourceTexture*> roles[] = { { "diffuse", lighting->diffuseTexture.get() },
					{ "normal", lighting->normalTexture.get() }, { "rimSoft", lighting->rimSoftLightingTexture.get() },
					{ "specBack", lighting->specularBackLightingTexture.get() } };
				std::string names;
				for (const auto& [role, texture] : roles) {
					known.Add(DumpViewOf(texture), role);
					names += fmt::format(" {}=\"{}\"", role, DumpTextureName(texture));
				}
				DumpLine(tag + fmt::format("material.engine materialAlpha={:.6g} clampMode={} diffuseRTIndex={}{}", lighting->materialAlpha, lighting->textureClampMode,
								   lighting->diffuseRenderTargetSourceIndex, names));
				if (auto* set = lighting->textureSet.get()) {
					std::string paths;
					for (std::uint32_t i = 0; i < 9; ++i) {
						const char* path = set->GetTexturePath(static_cast<RE::BSTextureSet::Texture>(i));
						paths += fmt::format(" {}=\"{}\"", i, path ? path : "");
					}
					DumpLine(tag + "material.textureSet" + paths);
				} else
					DumpLine(tag + "material.textureSet none");
			} else if (liveMaterial && liveMaterial->GetType() == RE::BSShaderMaterial::Type::kEffect) {
				const auto* effect = static_cast<const RE::BSEffectShaderMaterial*>(liveMaterial);
				known.Add(DumpViewOf(effect->sourceTexture.get()), "source");
				known.Add(DumpViewOf(effect->greyscaleTexture.get()), "greyscale");
				DumpLine(tag + fmt::format("material.engine effect source=\"{}\" greyscale=\"{}\" sourcePath=\"{}\" greyscalePath=\"{}\"", DumpTextureName(effect->sourceTexture.get()),
								   DumpTextureName(effect->greyscaleTexture.get()), effect->sourceTexturePath.c_str(), effect->greyscaleTexturePath.c_str()));
			} else
				DumpLine(tag + "material.engine none");

			// The material row as uploaded, and each texture index's import.
			const auto* row = body.materialRows.At(m);
			if (!row) {
				DumpLine(tag + fmt::format("material.row none (rows {})", body.materialRows.Count()));
				continue;
			}
			const auto* bytes = row->bytes.data();
			const auto& header = *reinterpret_cast<const MaterialRowHeader*>(bytes + kMaterialRowHeader);
			DumpLine(tag + fmt::format("material.header vsMaterial={:#x} psMaterial={:#x}", header.vsMaterial, header.psMaterial));
			DumpLine(tag + "material.rowVS " + DumpHex(bytes + kMaterialRowVS, kMaterialRowVSBytes));
			DumpLine(tag + "material.rowPS " + DumpHex(bytes + kMaterialRowPS, kMaterialRowPSBytes));
			DumpLine(tag + "material.rowPS.f " + DumpFloats(bytes + kMaterialRowPS, kMaterialRowPSBytes));
			DumpLine(tag + fmt::format("material.blocksHash={:016x}", DumpFnv(bytes, kMaterialRowHeader)));
			{
				std::string samplers;
				for (std::uint32_t i = 0; i < std::size(header.samplers); ++i)
					samplers += fmt::format(" {}", header.samplers[i]);
				DumpLine(tag + "material.samplers" + samplers);
			}
			auto texture = [&](const char* a_kind, std::uint32_t a_register, std::uint32_t a_rowIndex, std::uint32_t a_lookupIndex, ID3D11ShaderResourceView* a_lookupView,
							   ID3D11ShaderResourceView* a_recordView, bool a_written, bool a_pinned) {
				const bool isNull = a_rowIndex == lookups.nullTexture;
				const bool isProjected = std::ranges::find(lookups.projectedTextures, a_rowIndex) != lookups.projectedTextures.end();
				// The view is alive only while the lookup entry's owners pin its import.
				const std::string desc = a_pinned ? DumpDescribeView(a_lookupView) : std::string("view=unpinned");
				DumpLine(tag + fmt::format("tex {} t{} rowIndex={} lookupIndex={} rowIsLookup={} isNull={} isProjected={} written={} pinned={} viewIsRecord={} lookupView={} "
									   "recordView={} {}",
										a_kind, a_register, a_rowIndex, static_cast<std::int32_t>(a_lookupIndex), a_rowIndex == a_lookupIndex, isNull, isProjected, a_written,
										a_pinned, a_lookupView == a_recordView, known.Label(a_lookupView), known.Label(a_recordView), desc));
				DumpLine(tag + fmt::format("ptr tex {} t{} lookupView={} recordView={}", a_kind, a_register, static_cast<const void*>(a_lookupView),
								   static_cast<const void*>(a_recordView)));
			};
			for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
				if (t == kShadowMaskSlot)
					continue;
				texture("material", t, header.textures[t], lookup ? lookup->textureIndex[t] : Lookups::kNone, lookup ? lookup->views[t] : nullptr, record.textures[t],
					((record.textureWritten >> t) & 1) != 0, owners && owners->textures[t] != nullptr);
			}
			for (std::uint32_t f = 0; f < Extensions::kFeatureTextureSlots; ++f) {
				if (!record.featureTextures[f] && !header.features[f])
					continue;
				texture("feature", Extensions::FeatureTextureRegister(f), header.features[f], lookup ? lookup->featureIndex[f] : Lookups::kNone,
					lookup ? lookup->featureViews[f] : nullptr, record.featureTextures[f], record.featureTextures[f] != nullptr, owners && owners->features[f] != nullptr);
			}
		}
		DumpLine(fmt::format("END decals={} inputs={}", decals, inputs.size()));
	}

	// TEMPORARY (decal loss): subscribed by InstallDraws when CS_DCLF_DECAL_DUMP is set.
	void InstallDecalDump()
	{
		if (!DumpStateOf().wanted)
			return;
		logger::info("{}armed for Present count {} (Presents without a loading screen; dumps at the first colour main epoch from then)", kDumpPrefix, DumpStateOf().wanted);
		Subscribe<FramePoint::Present>([](const On<FramePoint::Present>&) {
			auto& s = DumpStateOf();
			if (!s.done && !Scene::FrameView::IsLoadingScreenUp())
				++s.presents;
		});
		Subscribe<MainEpochDone>(&DumpDecals);
	}
}
