#include "Common/NamespacedCBuffer.hlsli"

// GRASS_COLLISION_EXTERNAL: the includer supplies the texture and the per-frame values (RunGrass.hlsl under Drawcall Limit Fix's
// DCLF_PULLED, whose vertex stage can bind neither t100 nor b5: they come from its draw row and the descriptor heap). The includer
// defines GrassCollisionTexture() before including and assigns the statics below before calling GetDisplacedPosition. The
// displacement is then part of a position both DCLF's Z-prepass and colour builds compute, which test EQUAL: precise throughout
// (precise does not reach into a called function's body, so each function's own values are).
#if defined(GRASS_COLLISION_EXTERNAL)
#	define GRASS_COLLISION_PRECISE precise
#	define Collision GrassCollisionTexture()
#else
#	define GRASS_COLLISION_PRECISE
#endif

namespace GrassCollision
{
#if defined(GRASS_COLLISION_EXTERNAL)
	static float2 PosOffset;   // cell origin in camera space
	static uint2 ArrayOrigin;  // xy: array origin (clipmap wrapping)
	static int2 ValidMargin;
	static float TimeDelta;
	static uint BoundingBoxCount;
	static float CameraHeightDelta;
#else
	Texture2D<float4> Collision : register(t100);

	cbuffer GrassCollisionPerFrame : register(b5)
	{
		float2 NSCB(GrassCollision, PosOffset);   // cell origin in camera space
		uint2 NSCB(GrassCollision, ArrayOrigin);  // xy: array origin (clipmap wrapping)

		int2 NSCB(GrassCollision, ValidMargin);
		float NSCB(GrassCollision, TimeDelta);
		uint NSCB(GrassCollision, BoundingBoxCount);

		float NSCB(GrassCollision, CameraHeightDelta);
	}
	NSCB_ALIAS(GrassCollision, float2, PosOffset)
	NSCB_ALIAS(GrassCollision, uint2, ArrayOrigin)
	NSCB_ALIAS(GrassCollision, int2, ValidMargin)
	NSCB_ALIAS(GrassCollision, float, TimeDelta)
	NSCB_ALIAS(GrassCollision, uint, BoundingBoxCount)
	NSCB_ALIAS(GrassCollision, float, CameraHeightDelta)
#endif  // GRASS_COLLISION_EXTERNAL

	const static uint TEXTURE_SIZE = 512;
	const static float WORLD_SIZE = 4096;
	const static float CELL_SIZE = WORLD_SIZE / TEXTURE_SIZE;
	const static float2 ZRANGE = float2(2048.0, -2048.0);

	float ProceduralAnimation(GRASS_COLLISION_PRECISE float x, float distanceFromCenter)
	{
		float fadeRate = 250;
		x /= fadeRate;
		x /= distanceFromCenter;
		x *= 100;
		float frequency = 4 * Math::PI;
		GRASS_COLLISION_PRECISE float animation = cos(x * frequency) * exp(-x * 4);
		return animation;
	}

	void GetCollision(float3 worldPosition, float maximumDepth, float distanceFromCenter, out GRASS_COLLISION_PRECISE float collisionHeights,
		out GRASS_COLLISION_PRECISE float collisionAmount, out GRASS_COLLISION_PRECISE float previousCollisionHeights, out GRASS_COLLISION_PRECISE float previousCollisionAmount)
	{
		GRASS_COLLISION_PRECISE float2 positionMSAdjusted = worldPosition.xy - PosOffset.xy;
		GRASS_COLLISION_PRECISE float2 uv = positionMSAdjusted / WORLD_SIZE + .5;

		GRASS_COLLISION_PRECISE float2 cellVxCoord = uv * TEXTURE_SIZE;
		int2 cell000 = floor(cellVxCoord - 0.5);
		GRASS_COLLISION_PRECISE float2 bilinearPos = cellVxCoord - 0.5 - cell000;

		int2 cellID = cell000;

		collisionHeights = 0.0;
		collisionAmount = 0.0;

		previousCollisionHeights = 0.0;
		previousCollisionAmount = 0.0;

		GRASS_COLLISION_PRECISE float wsum = 0;

		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++) {
				int2 offset = int2(i, j);
				int2 cellID = cell000 + offset;

				if (any(cellID < 0) || any((uint2)cellID >= TEXTURE_SIZE))
					continue;

				float2 cellCentreMS = cellID + 0.5 - TEXTURE_SIZE / 2;
				cellCentreMS = cellCentreMS * CELL_SIZE;

				GRASS_COLLISION_PRECISE float2 bilinearWeights = 1 - abs(offset - bilinearPos);
				GRASS_COLLISION_PRECISE float w = bilinearWeights.x * bilinearWeights.y;

				uint2 cellTexID = (cellID + ArrayOrigin.xy) % TEXTURE_SIZE;

				GRASS_COLLISION_PRECISE float4 collisionSample = Collision[cellTexID];
				collisionSample = lerp(ZRANGE.x, ZRANGE.y, collisionSample);

				collisionHeights += collisionSample.x * w;
				collisionAmount += max(0, min(maximumDepth, worldPosition.z - collisionSample.x)) * ProceduralAnimation(collisionSample.y - collisionSample.x, distanceFromCenter) * w;

				previousCollisionHeights += collisionSample.z * w;
				previousCollisionAmount += max(0, min(maximumDepth, worldPosition.z - collisionSample.z)) * ProceduralAnimation(collisionSample.w - collisionSample.z, distanceFromCenter) * w;

				wsum += w;
			}

		if (wsum > 0.0) {
			collisionHeights /= wsum;
			collisionAmount /= wsum;
			previousCollisionHeights /= wsum;
			previousCollisionAmount /= wsum;
		} else {
			collisionHeights = TEXTURE_SIZE;
			collisionAmount = 0.0;
			previousCollisionHeights = TEXTURE_SIZE;
			previousCollisionAmount = 0.0;
		}
	}

	float3 ComputeNormalFromHeights(float h0, float hX, float hY, float delta)
	{
		GRASS_COLLISION_PRECISE float3 tangentX = float3(2.0 * delta, delta, hX - h0);
		GRASS_COLLISION_PRECISE float3 tangentY = float3(delta, 2.0 * delta, hY - h0);
		GRASS_COLLISION_PRECISE float3 crossProd = cross(tangentX, tangentY) * float3(1.0, 1.0, 0.1 / 3.0);

		GRASS_COLLISION_PRECISE float lenSq = dot(crossProd, crossProd);
		GRASS_COLLISION_PRECISE float3 normal = lenSq > 1e-12 ? -crossProd * rsqrt(lenSq) : float3(0, 0, -1);
		return normal;
	}

	void ComputeCollision(float3 worldPosition, float maximumDepth, float distanceFromCenter, float delta, out GRASS_COLLISION_PRECISE float3 collision,
		out GRASS_COLLISION_PRECISE float3 previousCollision)
	{
		// Sample collision at three points forming a small triangle
		float collisionCenter;
		float collisionX;
		float collisionY;

		float collisionCenterAmount;
		float collisionXAmount;
		float collisionYAmount;

		float previousCollisionCenter;
		float previousCollisionX;
		float previousCollisionY;

		float previousCollisionCenterAmount;
		float previousCollisionXAmount;
		float previousCollisionYAmount;

		GetCollision(worldPosition + float3(-delta, -delta, 0), maximumDepth, distanceFromCenter, collisionCenter, collisionCenterAmount, previousCollisionCenter, previousCollisionCenterAmount);
		GetCollision(worldPosition + float3(delta, 0, 0), maximumDepth, distanceFromCenter, collisionX, collisionXAmount, previousCollisionX, previousCollisionXAmount);
		GetCollision(worldPosition + float3(0, delta, 0), maximumDepth, distanceFromCenter, collisionY, collisionYAmount, previousCollisionY, previousCollisionYAmount);

		// Process current collision
		float3 currentAmounts = float3(collisionCenterAmount, collisionXAmount, collisionYAmount);
		GRASS_COLLISION_PRECISE float avgCurrentAmount = dot(currentAmounts, float3(1.0, 1.0, 1.0)) / 3.0;
		collision = ComputeNormalFromHeights(collisionCenter, collisionX, collisionY, delta) * avgCurrentAmount;

		// Process previous collision
		float3 previousAmounts = float3(previousCollisionCenterAmount, previousCollisionXAmount, previousCollisionYAmount);
		GRASS_COLLISION_PRECISE float avgPreviousAmount = dot(previousAmounts, float3(1.0, 1.0, 1.0)) / 3.0;
		previousCollision = ComputeNormalFromHeights(previousCollisionCenter, previousCollisionX, previousCollisionY, delta) * avgPreviousAmount;
	}

#if defined(GRASS_OPTIMIZATIONS) || defined(GRASS_COLLISION_EXTERNAL)
	// Positions arrive camera-relative: instances are already in world space here, so applying World again would double the offset.
	void GetDisplacedPosition(VS_INPUT input, float3 worldPosition, float3 worldPositionCentre, out GRASS_COLLISION_PRECISE float3 displacement,
		out GRASS_COLLISION_PRECISE float3 previousDisplacement)
	{
#else
	void GetDisplacedPosition(VS_INPUT input, float3 position, out float3 displacement, out float3 previousDisplacement)
	{
		float3 worldPosition = mul(World, float4(position.xyz, 1.0)).xyz;
#endif
		GRASS_COLLISION_PRECISE float nearFactor = smoothstep(2048.0, 0.0, length(worldPosition));

		if (input.Color.w > 0.0 && nearFactor > 0.0) {
#if !defined(GRASS_OPTIMIZATIONS) && !defined(GRASS_COLLISION_EXTERNAL)
			float3 worldPositionCentre = mul(World, float4(input.InstanceData1.xyz, 1.0)).xyz;
#endif
			// Limit stretching
			GRASS_COLLISION_PRECISE float3 remappedWorldPosition = lerp(worldPosition, worldPositionCentre, float3(0.95, 0.95, 0.0));

			GRASS_COLLISION_PRECISE float distanceFromCenter = length(worldPosition - worldPositionCentre) + 0.01;
			GRASS_COLLISION_PRECISE float maximumDepth = worldPosition.z - worldPositionCentre.z;

			// Return base collision
			GRASS_COLLISION_PRECISE float3 collision, previousCollision;
			ComputeCollision(remappedWorldPosition, maximumDepth, distanceFromCenter, CELL_SIZE, collision, previousCollision);

			// Do not let collision move upwards
			collision.z = -abs(collision.z);
			previousCollision.z = -abs(previousCollision.z);

			// Scale grass by wind amount (detect rocks and bottom of some grass)
			GRASS_COLLISION_PRECISE float alpha = saturate(input.Color.w * 10.0);

			displacement = collision * alpha * nearFactor * 0.75;
			previousDisplacement = previousCollision * alpha * nearFactor * 0.75;
		} else {
			displacement = 0.0;
			previousDisplacement = 0.0;
		}
	}
}

#if defined(GRASS_COLLISION_EXTERNAL)
#	undef Collision
#endif
#undef GRASS_COLLISION_PRECISE
