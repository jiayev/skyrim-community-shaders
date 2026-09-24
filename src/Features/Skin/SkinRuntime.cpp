#include "Features/Skin.h"
#include "SkinSources.h"

#include "Globals.h"
#include "State.h"

#include <DirectXTex.h>
#include <cmath>

using namespace SkinMaterials;

namespace
{
	bool IsSkin(const RE::BSShaderMaterial* a_material)
	{
		return a_material && a_material->GetType() == RE::BSShaderMaterial::Type::kLighting &&
		       (a_material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGen ||
				   a_material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
	}

	bool IsHead(RE::BSGeometry* a_geometry)
	{
		for (auto* node = a_geometry->parent; node; node = node->parent) {
			if (netimmerse_cast<RE::BSFaceGenNiNode*>(node))
				return true;
		}
		return false;
	}
}

void Skin::Invalidate()
{
	geometries.clear();
	textureCache.clear();
	++editor.revision;
}

void Skin::SetupResources()
{
	std::lock_guard lock(mutex);
	LoadSkinDetailTexture();
	PerGeometryCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerGeometryData>(), "Skin::PerGeometryCB");
	Invalidate();
	materials.Refresh(true);
}

void Skin::ReloadSkinDetail()
{
	std::lock_guard lock(mutex);
	LoadSkinDetailTexture();
	Invalidate();
}

void Skin::DataLoaded()
{
	std::lock_guard lock(mutex);
	materials.Refresh(true);
	Invalidate();
}

void Skin::Prepass()
{
	std::lock_guard lock(mutex);
	const auto frame = globals::state->frameCount;
	if (frame - lastScanFrame >= 120 || frame < lastScanFrame) {
		lastScanFrame = frame;
		const auto revision = materials.Revision();
		materials.Refresh();
		if (revision != materials.Revision())
			Invalidate();
		std::erase_if(geometries, [frame](const auto& entry) { return frame - entry.second.lastFrame > 600; });
		SkinSources::Prune();
	}
	ID3D11ShaderResourceView* view = texSkinDetail ? texSkinDetail->srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(72, 1, &view);
}

void Skin::PostPostLoad()
{
	SkinSources::Install();
	stl::write_vfunc<0x6, Hooks::BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Advanced Skin] Asset material renderer and source receipts installed");
}

bool Skin::PreviewMatches(const GeometryEntry& a_entry) const
{
	if (!editor.preview || !editor.valid || !editor.geometry)
		return false;
	const auto& target = editor.edit.target;
	if (!target.HasSurface() && !target.npc)
		return editor.geometry.get() == a_entry.geometry.get();
	if (target.npc && GetFormKey(RE::TESForm::LookupByID(a_entry.npc)) != target.npc)
		return false;
	if (!target.surfaceID.empty() && target.surfaceID != a_entry.surface.surfaceID)
		return false;
	if (target.txst && target.txst != a_entry.surface.txst)
		return false;
	return true;
}

Skin::GeometryEntry& Skin::ResolveGeometry(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
{
	if (geometries.size() >= 4096 && !geometries.contains(a_geometry))
		geometries.clear();
	auto& entry = geometries[a_geometry];
	const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property->GetBaseMaterial());
	auto* actor = a_geometry->GetUserData() ? a_geometry->GetUserData()->As<RE::Actor>() : nullptr;
	auto* npc = actor ? actor->GetActorBase() : nullptr;
	auto* race = actor ? actor->GetRace() : nullptr;
	const RE::FormID npcID = npc ? npc->GetFormID() : 0;
	const RE::FormID raceID = race ? race->GetFormID() : 0;
	const bool head = IsHead(a_geometry);
	const auto txst = SkinSources::Resolve(a_property, head);
	const bool changed = !entry.geometry || entry.property.get() != a_property || entry.material != material ||
	                     entry.textureSet.get() != material->textureSet.get() || entry.materialHash != material->hashKey ||
	                     entry.normal.get() != material->normalTexture.get() || entry.diffuse.get() != material->diffuseTexture.get() ||
	                     entry.npc != npcID || entry.race != raceID || entry.head != head || entry.txst != txst;
	entry.lastFrame = globals::state->frameCount;
	if (changed) {
		entry.geometry = RE::NiPointer(a_geometry);
		entry.property = RE::NiPointer(a_property);
		entry.material = material;
		entry.textureSet = material->textureSet;
		entry.normal = material->normalTexture;
		entry.diffuse = material->diffuseTexture;
		entry.materialHash = material->hashKey;
		entry.npc = npcID;
		entry.race = raceID;
		entry.head = head;
		entry.nif = ReadNif(a_geometry);
		entry.surface = {};
		entry.surface.surfaceID = entry.nif.surfaceID;
		entry.txst = txst;
		if (entry.surface.surfaceID.empty() && txst)
			entry.surface.txst = GetFormKey(RE::TESForm::LookupByID(txst));
	}
	if (changed || entry.revision != materials.Revision() || entry.previewRevision != editor.revision) {
		entry.revision = materials.Revision();
		entry.previewRevision = editor.revision;
		const bool preview = PreviewMatches(entry);
		const auto draft = preview ? DraftEdit() : UserEdit{};
		entry.resolved = materials.Resolve(entry.nif, entry.txst, entry.race, entry.npc, entry.surface, settings.DefaultProfile,
			preview ? &draft : nullptr);
		entry.textures = LoadConfiguredExtraTextures(entry.resolved.base.rfaos, entry.resolved.base.wetness);
	}
	return entry;
}

Skin::ExtraTextures Skin::LoadConfiguredExtraTextures(const std::string& a_rfaos, const std::string& a_wetness)
{
	const auto key = a_rfaos + '\n' + a_wetness;
	if (auto it = textureCache.find(key); it != textureCache.end())
		return it->second;
	if (textureCache.size() >= 1024)
		textureCache.clear();
	ExtraTextures result;
	const auto& state = globals::game::graphicsState->GetRuntimeData();
	result.rfaosTexture = state.defaultTextureBlack;
	result.wetnessTexture = state.defaultTextureBlack;
	auto load = [&state](const std::string& a_path, RE::NiSourceTexturePtr& a_output) {
		if (a_path.empty())
			return false;
		std::string resourcePath = a_path;
		std::replace(resourcePath.begin(), resourcePath.end(), '/', '\\');
		RE::BSResourceNiBinaryStream resource(resourcePath);
		if (!resource.good() || !resource.stream || resource.stream->totalSize < 128 || resource.stream->totalSize > 256 * 1024 * 1024)
			return false;
		std::array<char, 148> header{};
		const auto bytes = static_cast<uint32_t>(std::min<size_t>(header.size(), resource.stream->totalSize));
		if (!resource.read(header.data(), bytes))
			return false;
		DirectX::TexMetadata metadata{};
		if (FAILED(DirectX::GetMetadataFromDDSMemory(header.data(), bytes, DirectX::DDS_FLAGS_NONE, metadata)) ||
			metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.IsCubemap())
			return false;
		RE::NiPointer<RE::NiTexture> texture;
		RE::BSShaderManager::GetTexture(a_path.c_str(), true, texture, false);
		if (!texture || texture->GetRTTI() != globals::rtti::NiSourceTextureRTTI.get())
			return false;
		auto* source = static_cast<RE::NiSourceTexture*>(texture.get());
		if (!source->rendererTexture || !source->rendererTexture->resourceView)
			return false;
		for (const auto& fallback : { state.defaultTextureBlack, state.defaultTextureWhite, state.defaultTextureGrey, state.defaultTextureNormalMap }) {
			if (fallback && fallback->rendererTexture && fallback->rendererTexture->resourceView == source->rendererTexture->resourceView)
				return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		source->rendererTexture->resourceView->GetDesc(&desc);
		if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
			return false;
		a_output = RE::NiPointer(source);
		return true;
	};
	result.hasExtraTexture = load(a_rfaos, result.rfaosTexture);
	result.hasWetnessTexture = load(a_wetness, result.wetnessTexture);
	if (!a_rfaos.empty() && !result.hasExtraTexture)
		logger::warn("[Advanced Skin] Cannot load 2D RFAOS DDS: {}", a_rfaos);
	if (!a_wetness.empty() && !result.hasWetnessTexture)
		logger::warn("[Advanced Skin] Cannot load 2D wetness DDS: {}", a_wetness);
	textureCache.emplace(key, result);
	return result;
}

void Skin::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty || !IsSkin(a_pass->shaderProperty->GetBaseMaterial()))
		return;
	std::lock_guard lock(mutex);
	if (!PerGeometryCB)
		return;
	auto& entry = ResolveGeometry(a_pass->geometry, a_pass->shaderProperty);
	pendingTextures = entry.textures;
	texturesDirty = true;
	PerGeometryData data{};
	data.skinPerGeometry = GetWetness(a_pass->geometry);
	data.materialFlags = float4(float(entry.textures.hasExtraTexture), float(entry.textures.hasWetnessTexture), 0.0f, 0.0f);
	data.profile = MakeProfileData(entry.resolved.parameters);
	// A single shared buffer serves consecutive draws with different materials.
	PerGeometryCB->Update(data);
	ID3D11Buffer* buffer = PerGeometryCB->CB();
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
}

void Skin::SetShaderResources(ID3D11DeviceContext* a_context)
{
	std::lock_guard lock(mutex);
	if (!texturesDirty)
		return;
	auto view = [](const RE::NiSourceTexturePtr& texture) -> ID3D11ShaderResourceView* {
		return texture && texture->rendererTexture ? texture->rendererTexture->resourceView : nullptr;
	};
	auto* rfaos = view(pendingTextures.rfaosTexture);
	auto* wetness = view(pendingTextures.wetnessTexture);
	a_context->PSSetShaderResources(71, 1, &rfaos);
	a_context->PSSetShaderResources(74, 1, &wetness);
	texturesDirty = false;
}

void Skin::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
{
	globals::features::skin.BSLightingShader_SetupGeometry(a_pass);
	func(a_shader, a_pass, a_flags);
}

void Skin::LoadSettings(json& a_json)
{
	std::lock_guard lock(mutex);
	Settings next;
	try {
		if (a_json.contains("EnableSkin"))
			next.EnableSkin = a_json.at("EnableSkin").get<bool>();
		auto number = [&](const char* key, float& value, float minimum, float maximum) {
			if (a_json.contains(key)) {
				auto input = a_json.at(key).get<float>();
				if (!std::isfinite(input) || input < minimum || input > maximum)
					throw std::runtime_error(std::string("Invalid setting: ") + key);
				value = input;
			}
		};
		number("ExtraSkinWetness", next.ExtraSkinWetness, 0, 2);
		number("WetFadeTime", next.WetFadeTime, 0, 50);
		number("StartSweat", next.StartSweat, 0, 1);
		number("FullSweat", next.FullSweat, 0, 1);
		if (a_json.contains("WetParams")) {
			next.WetParams = a_json.at("WetParams").get<float4>();
			const auto& w = next.WetParams;
			if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z) || !std::isfinite(w.w) ||
				w.x < 0 || w.x > 1024 || w.y < 0 || w.y > 2 || w.z < 0 || w.z > 20 || w.w < 0 || w.w > 20)
				throw std::runtime_error("Invalid WetParams");
		}
		json defaults = a_json.value("DefaultProfile", json::object());
		if (!a_json.contains("DefaultProfile")) {
			for (const auto& field : ParameterTable()) {
				if (a_json.contains(field.name))
					defaults[field.name] = a_json.at(field.name);
			}
		}
		try {
			Apply(next.DefaultProfile, ReadParameters(defaults, false));
		} catch (const std::exception& e) {
			logger::warn("[Advanced Skin] Invalid legacy/default parameters reset without discarding global wetness settings: {}", e.what());
		}
		settings = next;
	} catch (const std::exception& e) {
		logger::warn("[Advanced Skin] Settings retained: {}", e.what());
	}
	if (a_json.contains("Profiles") || a_json.contains("RaceProfiles") || a_json.contains("EnableLegacyExtraTextureDiscovery"))
		logger::warn("[Advanced Skin] Legacy profiles/path discovery are inactive; import them explicitly with the authoring tool");
	Invalidate();
}

void Skin::SaveSettings(json& a_json)
{
	std::lock_guard lock(mutex);
	a_json = { { "EnableSkin", settings.EnableSkin }, { "ExtraSkinWetness", settings.ExtraSkinWetness },
		{ "WetFadeTime", settings.WetFadeTime }, { "StartSweat", settings.StartSweat }, { "FullSweat", settings.FullSweat },
		{ "WetParams", settings.WetParams }, { "DefaultProfile", ToJSON(settings.DefaultProfile) } };
}

void Skin::RestoreDefaultSettings()
{
	std::lock_guard lock(mutex);
	settings = {};
	editor = {};
	actorWetnessMap.clear();
	Invalidate();
}
