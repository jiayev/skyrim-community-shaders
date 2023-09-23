
cbuffer GrassCollisionPerFrame : register(b5)
{
	float4 boundCentre[2];
	float boundRadius;
	bool EnableGrassCollision;
	float RadiusMultiplier;
	float DisplacementMultiplier;
	float maxDistance;
}

struct StructuredCollision
{
	float3 centre[2];
	float radius;
};

StructuredBuffer<StructuredCollision> collisions : register(t0);

float3 GetDisplacedPosition(float3 position, float alpha, uint eyeIndex = 0)
{
	float3 worldPosition = mul(World[eyeIndex], float4(position, 1)).xyz;
	float3 displacement = 0;

	// Player bound culling and distance from player culling
	{
		float dist = distance(boundCentre[eyeIndex].xyz, worldPosition);
		if ((dist > boundRadius) && (dist > maxDistance)) {
			return 0;
		}
	}

	if (EnableGrassCollision) {
		uint counter = 0;
		uint collision_count, dummy;
		collisions.GetDimensions(collision_count, dummy);
		for (uint collision_index = 0; collision_index < collision_count; collision_index++) {
			StructuredCollision collision = collisions[collision_index];

			float dist = distance(collision.centre[eyeIndex], worldPosition);
			float power = smoothstep(collision.radius, 0.0, dist);
			float3 direction = worldPosition - collision.centre[eyeIndex];
			direction.y = 0;  // stops expanding/stretching
			direction = normalize(direction);
			float3 shift = power * direction;
			shift.z -= power;  // bias downwards
			displacement += shift.xyz;
		}
	}

	return displacement * saturate(alpha * alpha * 5) * DisplacementMultiplier;
}
