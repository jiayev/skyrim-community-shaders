
cbuffer GrassCollisionPerFrame : register(b4)
{
	bool  EnableGrassCollision;
	float RadiusMultiplier;
	float DisplacementMultiplier;
}

struct StructuredCollision
{
	float3 centre;
	float radius;
};

StructuredBuffer<StructuredCollision> collisions : register(t0);

float3 GetDisplacedPosition(float3 position, float alpha)
{		
	float3 displacement = 0;
	
	if (EnableGrassCollision){
		uint counter = 0;
		uint collision_count, dummy;
		collisions.GetDimensions(collision_count, dummy);
		for (uint collision_index = 0; collision_index < collision_count; collision_index++)
		{
			float3 worldPosition = mul(World, float4(position, 1)).xyz;

			StructuredCollision collision = collisions[collision_index];
			collision.radius *= RadiusMultiplier;

			float dist = distance(collision.centre, worldPosition);
			float power = smoothstep(collision.radius, 0.0, dist);
			float3 direction = worldPosition - collision.centre;
			direction = normalize(direction);
			float3 shift = direction * power * alpha;
			shift.z = -abs(shift.z);
			displacement += shift.xyz * DisplacementMultiplier;
		}
	}
		
	return displacement;
}