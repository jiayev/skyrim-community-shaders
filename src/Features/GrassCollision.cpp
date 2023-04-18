#include "GrassCollision.h"

#include "State.h"
#include "Util.h"

#include "Features/Clustered.h"

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void GrassCollision::DrawSettings()
{
	if (ImGui::BeginTabItem("Grass Collision")) {
		if (ImGui::TreeNodeEx("Grass Collision", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Allows player collision to modify grass position.");
			
			ImGui::Checkbox("Enable Grass Collision", (bool*)&settings.EnableGrassCollision);
			ImGui::Text("Distance from collision centres to apply collision");
			ImGui::SliderFloat("Radius Multiplier", &settings.RadiusMultiplier, 0.0f, 8.0f);
			
			ImGui::Text("Strength of each collision on grass position.");
			ImGui::SliderFloat("Displacement Multiplier", &settings.DisplacementMultiplier, 0.0f, 16.0f);
		
			ImGui::TreePop();
		}

		ImGui::EndTabItem();
	}
}

static bool GetShapeBound(RE::NiAVObject* a_node, RE::NiPoint3& centerPos, float& radius)
{
	RE::bhkNiCollisionObject* Colliedobj = nullptr;
	if (a_node->collisionObject)
		Colliedobj = a_node->collisionObject->AsBhkNiCollisionObject();

	if (!Colliedobj)
		return false;

	RE::bhkRigidBody* bhkRigid = Colliedobj->body.get() ? Colliedobj->body.get()->AsBhkRigidBody() : nullptr;
	RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
	if (bhkRigid && hkpRigid) {
		RE::hkVector4 massCenter;
		bhkRigid->GetCenterOfMassWorld(massCenter);
		float massTrans[4];
		_mm_store_ps(massTrans, massCenter.quad);
		centerPos = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();

		const RE::hkpShape* shape = hkpRigid->collidable.GetShape();
		if (shape) {
			float upExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 0.0f, 1.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float downExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 0.0f, -1.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto z_extent = (upExtent + downExtent) / 2.0f;

			float forwardExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 1.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float backwardExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, -1.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto y_extent = (forwardExtent + backwardExtent) / 2.0f;

			float leftExtent = shape->GetMaximumProjection(RE::hkVector4{ 1.0f, 0.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float rightExtent = shape->GetMaximumProjection(RE::hkVector4{ -1.0f, 0.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto x_extent = (leftExtent + rightExtent) / 2.0f;

			radius = sqrtf(x_extent * x_extent + y_extent * y_extent + z_extent * z_extent);

			return true;
		}
	}

	return false;
}

static bool GetShapeBound(RE::bhkNiCollisionObject* Colliedobj, RE::NiPoint3& centerPos, float& radius)
{
	if (!Colliedobj)
		return false;

	RE::bhkRigidBody* bhkRigid = Colliedobj->body.get() ? Colliedobj->body.get()->AsBhkRigidBody() : nullptr;
	RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
	if (bhkRigid && hkpRigid) {
		RE::hkVector4 massCenter;
		bhkRigid->GetCenterOfMassWorld(massCenter);
		float massTrans[4];
		_mm_store_ps(massTrans, massCenter.quad);
		centerPos = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();

		const RE::hkpShape* shape = hkpRigid->collidable.GetShape();
		if (shape) {
			float upExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 0.0f, 1.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float downExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 0.0f, -1.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto z_extent = (upExtent + downExtent) / 2.0f;

			float forwardExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, 1.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float backwardExtent = shape->GetMaximumProjection(RE::hkVector4{ 0.0f, -1.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto y_extent = (forwardExtent + backwardExtent) / 2.0f;

			float leftExtent = shape->GetMaximumProjection(RE::hkVector4{ 1.0f, 0.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			float rightExtent = shape->GetMaximumProjection(RE::hkVector4{ -1.0f, 0.0f, 0.0f, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
			auto x_extent = (leftExtent + rightExtent) / 2.0f;

			radius = sqrtf(x_extent * x_extent + y_extent * y_extent + z_extent * z_extent);

			return true;
		}
	}

	return false;
}

void GrassCollision::UpdateCollisions()
{
	auto state = BSGraphics::RendererShadowState::QInstance();

	std::uint32_t currentCollisionCount = 0;

	std::vector<CollisionSData> collisionsData{};

	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		if (auto root = player->Get3D(false)) {
			auto position = player->GetPosition();
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (GetShapeBound(a_object, centerPos, radius)) {
					radius *= settings.RadiusMultiplier;
					CollisionSData data{};
					data.centre.x = centerPos.x - state->m_PosAdjust.x;
					data.centre.y = centerPos.y - state->m_PosAdjust.y;
					data.centre.z = centerPos.z - state->m_PosAdjust.z;
					data.radius = radius;
					currentCollisionCount++;
					collisionsData.push_back(data);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	if (!currentCollisionCount) {
		CollisionSData data{};
		ZeroMemory(&data, sizeof(data));
		collisionsData.push_back(data);
		currentCollisionCount = 1;
	}

	static std::uint32_t colllisionCount = 0;
	bool collisionCountChanged = currentCollisionCount != colllisionCount;

	if (!collisions || collisionCountChanged) {
		colllisionCount = currentCollisionCount;

		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(CollisionSData);
		sbDesc.ByteWidth = sizeof(CollisionSData) * colllisionCount;
		collisions = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = colllisionCount;
		collisions->CreateSRV(srvDesc);
	}

	auto context = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;
	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(collisions->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(CollisionSData) * colllisionCount;
	memcpy_s(mapped.pData, bytes, collisionsData.data(), bytes);
	context->Unmap(collisions->resource.get(), 0);
}

void GrassCollision::ModifyGrass(const RE::BSShader*, const uint32_t)
{
	if (updatePerFrame) {
		if (settings.EnableGrassCollision) {
			UpdateCollisions();
		}

		PerFrame perFrameData{};
		ZeroMemory(&perFrameData, sizeof(perFrameData));

		auto state = BSGraphics::RendererShadowState::QInstance();
		auto shaderState = BSGraphics::ShaderState::QInstance();

		auto bound = shaderState->kCachedPlayerBound;
		perFrameData.boundCentre.x = bound.center.x - state->m_PosAdjust.x;
		perFrameData.boundCentre.y = bound.center.y - state->m_PosAdjust.y;
		perFrameData.boundCentre.z = bound.center.z - state->m_PosAdjust.z;
		perFrameData.boundRadius = bound.radius * settings.RadiusMultiplier;

		perFrameData.Settings = settings;

		perFrame->Update(perFrameData);

		updatePerFrame = false;
	}

	auto context = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;

	ID3D11ShaderResourceView* views[1]{};
	views[0] = collisions->srv.get();
	context->VSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11Buffer* buffers[1];
	buffers[0] = perFrame->CB();
	context->VSSetConstantBuffers(4, ARRAYSIZE(buffers), buffers);
}

void GrassCollision::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Grass:
		ModifyGrass(shader, descriptor);
		break;
	}
}

void GrassCollision::Load(json& o_json)
{
	if (o_json["Grass Collision"].is_object()) {
		json& grassCollision = o_json["Grass Collision"];
		if (grassCollision["EnableGrassCollision"].is_boolean())
			settings.EnableGrassCollision = grassCollision["EnableGrassCollision"];
		if (grassCollision["RadiusMultiplier"].is_number())
			settings.RadiusMultiplier = grassCollision["RadiusMultiplier"];
		if (grassCollision["DisplacementMultiplier"].is_number())
			settings.DisplacementMultiplier = grassCollision["DisplacementMultiplier"];
	}
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(L"Data\\Shaders\\Features\\GrassCollision.ini");
	if (auto value = ini.GetValue("Info", "Version")) {
		enabled = true;
		version = value;
	} else {
		enabled = false;
	}
}

void GrassCollision::Save(json& o_json)
{
	json grassCollision;
	grassCollision["EnableGrassCollision"] = (bool)settings.EnableGrassCollision;
	grassCollision["RadiusMultiplier"] = settings.RadiusMultiplier;
	grassCollision["DisplacementMultiplier"] = settings.DisplacementMultiplier;
	o_json["Grass Collision"] = grassCollision;
}

void GrassCollision::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());
}

void GrassCollision::Reset()
{
	updatePerFrame = true;
}

bool GrassCollision::ValidateCache(CSimpleIniA& a_ini)
{
	logger::info("Validating Grass Collision");

	auto enabledInCache = a_ini.GetBoolValue("Grass Collision", "Enabled", false);
	if (enabledInCache && !enabled) {
		logger::info("Feature was uninstalled");
		return false;
	}
	if (!enabledInCache && enabled) {
		logger::info("Feature was installed");
		return false;
	}

	if (enabled) {
		auto versionInCache = a_ini.GetValue("Grass Collision", "Version");
		if (strcmp(versionInCache, version.c_str()) != 0) {
			logger::info("Change in version detected. Installed {} but {} in Disk Cache", version, versionInCache);
			return false;
		} else {
			logger::info("Installed version and cached version match.");
		}
	}

	logger::info("Cached feature is valid");
	return true;
}

void GrassCollision::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	a_ini.SetBoolValue("Grass Collision", "Enabled", enabled);
	a_ini.SetValue("Grass Collision", "Version", version.c_str());
}