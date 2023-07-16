#include "GrassCollision.h"

#include "State.h"
#include "Util.h"

#include "Features/Clustered.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassCollision::Settings,
	EnableGrassCollision,
	RadiusMultiplier,
	DisplacementMultiplier)

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void GrassCollision::DrawSettings()
{
	if (ImGui::TreeNodeEx("Grass Collision", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Allows player collision to modify grass position.");

		ImGui::Checkbox("Enable Grass Collision", (bool*)&settings.EnableGrassCollision);
		ImGui::TextWrapped("Distance from collision centres to apply collision");
		ImGui::SliderFloat("Radius Multiplier", &settings.RadiusMultiplier, 0.0f, 8.0f);

		ImGui::TextWrapped("Strength of each collision on grass position.");
		ImGui::SliderFloat("Displacement Multiplier", &settings.DisplacementMultiplier, 0.0f, 32.0f);

		ImGui::TreePop();
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
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

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
					RE::NiPoint3 eyePosition{};
					if (REL::Module::IsVR()) {
						// find center of eye position
						eyePosition = state->GetVRRuntimeData().posAdjust.getEye() + state->GetVRRuntimeData().posAdjust.getEye(1);
						eyePosition /= 2;
					} else
						eyePosition = state->GetRuntimeData().posAdjust.getEye();
					data.centre.x = centerPos.x - eyePosition.x;
					data.centre.y = centerPos.y - eyePosition.y;
					data.centre.z = centerPos.z - eyePosition.z;
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

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(collisions->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(CollisionSData) * colllisionCount;
	memcpy_s(mapped.pData, bytes, collisionsData.data(), bytes);
	context->Unmap(collisions->resource.get(), 0);
}

void GrassCollision::ModifyGrass(const RE::BSShader*, const uint32_t)
{
	if (!loaded)
		return;

	if (updatePerFrame) {
		if (settings.EnableGrassCollision) {
			UpdateCollisions();
		}

		PerFrame perFrameData{};
		ZeroMemory(&perFrameData, sizeof(perFrameData));

		auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto& shaderState = RE::BSShaderManager::State::GetSingleton();

		auto bound = shaderState.cachedPlayerBound;
		RE::NiPoint3 eyePosition{};
		if (REL::Module::IsVR()) {
			// find center of eye position
			eyePosition = state->GetVRRuntimeData().posAdjust.getEye() + state->GetVRRuntimeData().posAdjust.getEye(1);
			eyePosition /= 2;
		} else
			eyePosition = state->GetRuntimeData().posAdjust.getEye();
		perFrameData.boundCentre.x = bound.center.x - eyePosition.x;
		perFrameData.boundCentre.y = bound.center.y - eyePosition.y;
		perFrameData.boundCentre.z = bound.center.z - eyePosition.z;
		perFrameData.boundRadius = bound.radius * settings.RadiusMultiplier;

		perFrameData.Settings = settings;

		perFrame->Update(perFrameData);

		updatePerFrame = false;
	}

	if (settings.EnableGrassCollision) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		ID3D11ShaderResourceView* views[1]{};
		views[0] = collisions->srv.get();
		context->VSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11Buffer* buffers[1];
		buffers[0] = perFrame->CB();
		context->VSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);
	}
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
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void GrassCollision::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void GrassCollision::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());
}

void GrassCollision::Reset()
{
	updatePerFrame = true;
}