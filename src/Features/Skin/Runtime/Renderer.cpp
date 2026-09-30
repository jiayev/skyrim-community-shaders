#include "Features/Skin.h"

#include "CSEditor/EditorWindow.h"
#include "Globals.h"
#include "State.h"

#include <cmath>

using namespace SkinMaterials;

void Skin::Invalidate(bool a_resources)
{
	++revision;
	if (a_resources) {
		textures.clear();
		resourceRevisions.clear();
		for (auto& [geometry, entry] : geometries)
			entry.textures = {};
	}
}

void Skin::SetupResources()
{
	std::lock_guard lock(mutex);
	RestoreSampler();
	detailSampler = nullptr;
	PerGeometryCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerGeometryData>(), "Skin::PerGeometryCB");
	D3D11_SAMPLER_DESC sampler{};
	sampler.Filter = D3D11_FILTER_ANISOTROPIC;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sampler.MaxAnisotropy = 16;
	sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&sampler, detailSampler.put()));
	Invalidate(true);
}

void Skin::DataLoaded()
{
	SkinActors::Install();
	SkinActors::InstallLifecycle();
	std::lock_guard lock(mutex);
	Invalidate(true);
}

void Skin::GameReset()
{
	SkinActors::Reset();
}

void Skin::PostPostLoad()
{
	SkinSources::Install();
	stl::write_vfunc<0x6, Hooks::SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x7, Hooks::RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
}

std::shared_ptr<Skin::TextureEntry> Skin::RequestTexture(const std::string& a_path)
{
	if (a_path.empty())
		return {};
	auto& entry = textures[a_path];
	if (!entry)
		entry = std::make_shared<TextureEntry>();
	return entry;
}

bool Skin::PreviewMatches(const GeometryEntry& a_entry) const
{
	return (editor.transfer && editor.transfer->preview && a_entry.transferPreview) || (editor.preview && (editor.character ?
																												  std::find(editor.targets.begin(), editor.targets.end(), a_entry.geometry) != editor.targets.end() :
																												  editor.geometry.get() == a_entry.geometry.get()));
}

Skin::GeometryEntry& Skin::Observe(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
{
	auto& entry = geometries[a_geometry];
	entry.frame = globals::state->frameCount;
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
	const auto source = SkinSources::Get(a_property);
	auto* actor = SkinActors::Owner(a_geometry);
	const auto previousActor = entry.actor.get();
	if (!actor && SkinActors::Belongs(a_geometry, previousActor.get()))
		actor = previousActor.get();
	const auto actorHandle = actor ? actor->GetHandle() : RE::ActorHandle{};
	auto* npc = actor ? actor->GetActorBase() : nullptr;
	const auto race = actor && actor->GetRace() ? actor->GetRace()->GetFormID() : 0;
	const auto sex = npc ? static_cast<uint32_t>(npc->GetSex()) : 0;
	const std::array<float, 4> transform{ material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	const auto& geometryData = a_geometry->GetGeometryRuntimeData();
	auto* partition = geometryData.skinInstance ? geometryData.skinInstance->skinPartition.get() : nullptr;
	const bool modelSpace = a_property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
	if ((entry.target.character && !SkinActors::Current(entry.target, a_geometry)) || entry.actor != actorHandle || !entry.geometry || entry.property.get() != a_property || entry.materialPointer != material ||
		entry.textureSet != material->textureSet || entry.normal != material->normalTexture ||
		entry.diffuse != material->diffuseTexture || entry.source.revision != source.revision || entry.race != race || entry.sex != sex ||
		entry.parent != a_geometry->parent || entry.buffer != geometryData.rendererData || entry.partition != partition || entry.uvTransform != transform || entry.modelSpace != modelSpace) {
		entry.actor = actorHandle;
		entry.geometry = RE::NiPointer(a_geometry);
		entry.property = RE::NiPointer(a_property);
		entry.materialPointer = material;
		entry.parent = a_geometry->parent;
		entry.buffer = geometryData.rendererData;
		entry.partition = partition;
		entry.uvTransform = transform;
		entry.modelSpace = modelSpace;
		entry.textureSet = material->textureSet;
		entry.normal = material->normalTexture;
		entry.diffuse = material->diffuseTexture;
		entry.source = source;
		entry.race = race;
		entry.sex = sex;
		entry.ready = false;
		entry.head = false;
		for (auto* parent = a_geometry->parent; parent; parent = parent->parent)
			entry.head |= netimmerse_cast<RE::BSFaceGenNiNode*>(parent) != nullptr;
	}
	return entry;
}

void Skin::Prepare(GeometryEntry& a_entry, const SkinActors::Snapshot& a_state)
{
	a_entry.diagnostic.clear();
	a_entry.overridden = false;
	a_entry.transferPreview = false;
	a_entry.textures = {};
	try {
		a_entry.base = SkinSources::AssignedMaterial(a_entry.source);
		a_entry.thickness = a_entry.source.asset && !a_entry.source.asset->textures[2].empty();
		if (auto* txst = RE::TESForm::LookupByID<RE::BGSTextureSet>(a_entry.source.txst))
			a_entry.thickness = !SkinSources::Paths(txst)[2].empty();
		const auto actor = a_entry.actor.get();
		if (a_entry.inspect || (actor && SkinActors::Find(*a_state, SkinActors::ActorKey(actor.get()))))
			a_entry.target = SkinActors::Describe(a_entry.geometry.get(), a_entry.source, [&](const std::string& path) {
			if (path.empty())
				return uint64_t{ 0 };
			const auto found = resourceRevisions.find(path);
			if (found != resourceRevisions.end())
				return found->second;
			const auto fingerprint = SkinEditor::Fingerprint(SkinEditor::ReadResource(path));
			resourceRevisions.emplace(path, fingerprint);
			return fingerprint; }, actor.get());
		else
			a_entry.target = {};
		a_entry.effective = a_entry.base;
		const bool valid = !a_entry.source.asset || a_entry.source.asset->definition.status != Status::Invalid;
		if (!valid)
			a_entry.diagnostic = a_entry.source.asset->definition.diagnostic;
		if (valid && a_entry.target.character) {
			const auto* binding = SkinActors::Find(*a_state, a_entry.target.key);
			const auto* surface = SkinActors::Find(binding, a_entry.target.part, a_entry.target.guard);
			if (surface && a_entry.target.error.empty()) {
				a_entry.effective = Apply(a_entry.base, surface->changes);
				a_entry.overridden = true;
			} else if (binding && a_entry.target.part < SkinActors::Part::Count && !binding->parts[static_cast<size_t>(a_entry.target.part)].empty()) {
				a_entry.diagnostic = T("feature.skin.character_customization_paused", "Character customization is paused: appearance or surface assignment changed.");
			}
		}
		if (valid && editor.preview && (editor.character ? std::find(editor.targets.begin(), editor.targets.end(), a_entry.geometry) != editor.targets.end() : editor.geometry == a_entry.geometry))
			a_entry.effective = editor.character ? Apply(a_entry.base, editor.changes) : editor.draft;
		if (valid && editor.transfer && editor.transfer->preview)
			if (const auto* sample = TransferSample(a_entry)) {
				a_entry.effective = sample->material;
				a_entry.transferPreview = true;
			}
		for (size_t i = 0; i < a_entry.textures.size(); ++i) {
			try {
				a_entry.textures[i] = RequestTexture(NormalizeTexturePath(a_entry.effective.textures[i]));
			} catch (const std::exception& error) {
				a_entry.diagnostic += std::string(TextureLabels()[i]) + ": " + error.what() + " ";
			}
		}
		for (size_t i = 0; i < 2; ++i)
			if (a_entry.textures[i] && a_entry.textures[i]->prepared && a_entry.textures[i]->resource.reconstructNormal)
				a_entry.diagnostic += T("feature.skin.bc5_unorm_is_only_supported_for_detail_normals_without_a", "BC5 UNORM is only supported for detail normals without a mask.");
		if (a_entry.effective.textures[2].empty())
			a_entry.textures[2] = RequestTexture(settings.DetailTexture);
	} catch (const std::exception& e) {
		a_entry.effective = {};
		a_entry.diagnostic = e.what();
	}
	a_entry.revision = revision;
	a_entry.actorRevision = a_state->revision;
	a_entry.ready = true;
}

void Skin::Prepass()
{
	std::lock_guard lock(mutex);
	const auto characters = SkinActors::Get();
	RestoreSampler();
	if ((editor.preview || (editor.transfer && editor.transfer->preview)) && !EditorWindow::GetSingleton()->open) {
		editor.preview = false;
		if (editor.transfer)
			editor.transfer->preview = false;
		editor.previewWetness = -1;
		Invalidate();
	}
	if (session != characters->session) {
		geometries.clear();
		actorWetnessMap.clear();
		editor = {};
		pendingViews = {};
		textures.clear();
		resourceRevisions.clear();
		texturesDirty = true;
		session = characters->session;
	}
	for (auto& [geometry, entry] : geometries) {
		if (entry.actor && !SkinActors::Belongs(geometry, entry.actor.get().get())) {
			entry.ready = false;
			continue;
		}
		if (!SkinMaterials::IsCompatible(entry.property.get())) {
			entry.ready = false;
			continue;
		}
		if (!entry.ready || entry.revision != revision || entry.actorRevision != characters->revision)
			Prepare(entry, characters);
	}
	for (auto& [path, entry] : textures) {
		if (!entry->prepared) {
			entry->resource = SkinTextures::Load(path);
			entry->prepared = true;
		}
	}
	const auto frame = globals::state->frameCount;
	if (frame - lastPrune >= 120) {
		lastPrune = frame;
		std::erase_if(geometries, [frame, this](const auto& item) {
			return frame - item.second.frame > 600 && editor.geometry.get() != item.first &&
			       std::none_of(editor.surfaces.begin(), editor.surfaces.end(), [&](const auto& surface) { return surface.get() == item.first; });
		});
		std::erase_if(textures, [](const auto& item) { return item.second.use_count() == 1; });
		SkinSources::Prune();
	}
}

Skin::SkinData Skin::MakeData(const Material& a_material, bool a_head) const
{
	auto p = a_material.parameters;
	const auto fields = ParameterTable();
	for (size_t i = 0; i < fields.size(); ++i) {
		const auto& field = fields[i];
		auto& value = p.values[i];
		value = std::isfinite(value) ? std::clamp(value, field.minimum, field.maximum) : field.initial;
	}
	const bool globalDetail = a_material.textures[2].empty();
	SkinData data{};
	data.skinParams = { p[Parameter::Roughness], 0, 0, float(settings.EnableSkin && a_material.enabled) };
	data.skinParams2 = { 0.15f, settings.ExtraSkinWetness, p[Parameter::Reflectance], 1.0f };
	data.skinDetailParams = {
		globalDetail ? settings.DetailTiling * (a_head ? 1.0f : settings.BodyTiling) : p[Parameter::DetailTiling],
		1.0f, globalDetail ? settings.DetailStrength : p[Parameter::DetailStrength],
		float(settings.EnableDetail && p[Parameter::DetailEnabled] != 0)
	};
	data.sssParams = { p[Parameter::Transmission], p[Parameter::TransmissionDepth], p[Parameter::SSSAmount], 1.0f };
	data.fuzzParams = { p[Parameter::Fuzz], 0.5f, 0.04f, 0.0f };
	data.physicalParams = { p[Parameter::WetResponse], 0, 0, 0 };
	data.wetParams = settings.WetParams;
	return data;
}

Skin::SkinData Skin::GetCommonBufferData()
{
	std::lock_guard lock(mutex);
	Material material;
	material.enabled = true;
	return MakeData(material, false);
}

void Skin::RestoreSampler()
{
	std::lock_guard lock(mutex);
	if (samplerBound) {
		auto* sampler = savedSampler.get();
		globals::d3d::context->PSSetSamplers(15, 1, &sampler);
		savedSampler = nullptr;
		samplerBound = false;
	}
}

void Skin::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	std::lock_guard lock(mutex);
	RestoreSampler();
	pendingViews = {};
	texturesDirty = true;
	if (!PerGeometryCB)
		return;
	PerGeometryData data{};
	data.profile = MakeData(Material{}, false);
	if (a_pass && a_pass->geometry) {
		data.skinPerGeometry = GetWetness(a_pass->geometry);
		if (SkinMaterials::IsCompatible(a_pass->shaderProperty)) {
			auto& entry = Observe(a_pass->geometry, a_pass->shaderProperty);
			if (entry.ready && entry.revision == revision && session == SkinActors::Session() && entry.actorRevision == SkinActors::Revision()) {
				data.profile = MakeData(entry.effective, entry.head);
				for (size_t i = 0; i < entry.textures.size(); ++i) {
					if (entry.textures[i] && (i == 2 || !entry.textures[i]->resource.reconstructNormal))
						pendingViews[i] = entry.textures[i]->resource.view;
				}
				data.materialFlags = { float(bool(pendingViews[0])), float(bool(pendingViews[1])),
					float(entry.textures[2] && entry.textures[2]->resource.reconstructNormal), float(entry.thickness) };
				if (!pendingViews[2])
					data.profile.skinDetailParams.w = 0;
				if (PreviewMatches(entry) && editor.previewWetness >= 0)
					data.skinPerGeometry = { editor.previewWetness, 0, 0, 0 };
			}
		}
	}
	PerGeometryCB->Update(data);
	ID3D11Buffer* buffer = PerGeometryCB->CB();
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
}

void Skin::SetShaderResources(ID3D11DeviceContext* a_context)
{
	std::lock_guard lock(mutex);
	if (!texturesDirty)
		return;
	constexpr std::array<UINT, 3> slots{ 71, 74, 72 };
	for (size_t i = 0; i < slots.size(); ++i) {
		auto* view = pendingViews[i].get();
		a_context->PSSetShaderResources(slots[i], 1, &view);
	}
	if (pendingViews[2] && detailSampler) {
		a_context->PSGetSamplers(15, 1, savedSampler.put());
		auto* sampler = detailSampler.get();
		a_context->PSSetSamplers(15, 1, &sampler);
		samplerBound = true;
	}
	texturesDirty = false;
}

void Skin::Hooks::SetupGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
{
	globals::features::skin.BSLightingShader_SetupGeometry(a_pass);
	func(a_shader, a_pass, a_flags);
}

void Skin::Hooks::RestoreGeometry::thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
{
	globals::features::skin.RestoreSampler();
	func(a_shader, a_pass, a_flags);
}
