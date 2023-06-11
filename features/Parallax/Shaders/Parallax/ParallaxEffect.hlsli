
//https://github.com/alandtse/SSEShaderTools/tree/main/shaders

#include "Parallax/TerrainParallax.hlsli"

//POM shader based off https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl under MIT
//v1, v6, v3,v4,v5
#if defined(PARALLAX)
float2 GetParallaxCoords(float2 coords, float3 viewDir, float3 bitangent, float3 tangent, float3 normal)
{
	float3x3 modelTangentMatrix = transpose(float3x3(bitangent, tangent, normal));
	float4 tangentViewDir;
	tangentViewDir.x = dot(modelTangentMatrix[0].xyz, viewDir.xyz);
	tangentViewDir.y = dot(modelTangentMatrix[1].xyz, viewDir.xyz);
	tangentViewDir.z = dot(modelTangentMatrix[2].xyz, viewDir.xyz);
	tangentViewDir.w = dot(tangentViewDir.xyz, tangentViewDir.xyz);
	tangentViewDir.w = rsqrt(tangentViewDir.w);
	tangentViewDir.xyz = tangentViewDir.xyz * tangentViewDir.www;

	// Compute the current gradients:
	float2 textureDims;
	TexParallaxSampler.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = coords * textureDims;

	// Compute all 4 derivatives in x and y in a single instruction to optimize:
	float2 dxSize, dySize;
	float2 dx, dy;

	float4( dxSize, dx ) = ddx( float4( texCoordsPerSize, coords ) );
	float4( dySize, dy ) = ddy( float4( texCoordsPerSize, coords ) );
					
	// Find min of change in u and v across quad: compute du and dv magnitude across quad
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;

	// Standard mipmapping uses max here
	float minTexCoordDelta = max( dTexCoords.x, dTexCoords.y );

	// Compute the current mip level  (* 0.5 is effectively computing a square root before )
	float mipLevel = max( 0.5 * log2( minTexCoordDelta ), 0 );

	float2 output;
	if ( mipLevel <= 6 )
	{
		float2 dx = ddx(coords);
		float2 dy = ddy(coords);

		int numSteps = lerp(36, 4, tangentViewDir.z * ((mipLevel / 6)));

		float currHeight = 0.0;
		float stepSize = 1.0/ (float)numSteps;

		float2 offsetPerStep = tangentViewDir.xy * float2(0.0800,0.0800) * stepSize.xx;
		float2 currentOffset = tangentViewDir.xy * float2(0.0400,0.0400) + coords.xy;
		float heightCorrectionScale = ((-1.0*tangentViewDir.z)+2.0);
		float prevHeight = 1.0;
		float currentBound = 1.0;
		float parallaxAmount = 0.0;

		float2 pt1 = 0;
		float2 pt2 = 0;

		while(numSteps > 0)
		{
			currentOffset -= offsetPerStep;
			
			currHeight = TexParallaxSampler.SampleGrad(SampParallaxSampler, currentOffset,dx,dy).x - 0.5;
			currHeight = heightCorrectionScale * currHeight + 0.5;
			
			currentBound -= stepSize;
			if(currHeight>currentBound)
			{
				pt1 = float2(currentBound, currHeight);
				pt2 = float2(currentBound + stepSize, prevHeight);
				
				break;
			} 
			else
			{
				numSteps-=1;
				prevHeight=currHeight;
			}
		}

		float delta2 = pt2.x - pt2.y;
		float delta1 = pt1.x - pt1.y;
		float denominator = delta2-delta1;

		if(denominator==0.0){
			parallaxAmount=0.0;
		} 
		else 
		{
			parallaxAmount = (pt1.x * delta2 - pt2.x * delta1) / denominator;
		}

		float offset3 = (1.0 - parallaxAmount) * -0.0800 + 0.0400;
		output = tangentViewDir.xy * offset3.xx + coords.xy;

		if ( mipLevel > (float)(6 - 1) )
		{
			// Lerp based on the fractional part:
			float mipLevelInt;
			float mipLevelFrac = modf( mipLevel, mipLevelInt );

			// Lerp the texture coordinate from parallax occlusion mapped coordinate to bump mapping
			// smoothly based on the current mip level:
			float height = TexParallaxSampler.Sample(SampParallaxSampler, coords.xy).x;
			height = height * 0.0800 - 0.0400;
			output = lerp( output, tangentViewDir.xy * height.xx + coords.xy, mipLevelFrac);
		}  
	} 
	else 
	{
		float height = TexParallaxSampler.Sample(SampParallaxSampler, coords.xy).x;
		height = height * 0.0800 - 0.0400;
		output = tangentViewDir.xy * height.xx + coords.xy;
	}
	return output;
}
#endif

float2 GetParallaxCoordsTerrain(float2 coords, float3 viewDir, float3 bitangent, float3 tangent, float3 normal, Texture2D<float4> TexLandColor, float blend)
{
#if defined(TERRAIN_PARALLAX)
	float3x3 modelTangentMatrix = transpose(float3x3(bitangent, tangent, normal));
	float4 tangentViewDir;
	tangentViewDir.x = dot(modelTangentMatrix[0].xyz, viewDir.xyz);
	tangentViewDir.y = dot(modelTangentMatrix[1].xyz, viewDir.xyz);
	tangentViewDir.z = dot(modelTangentMatrix[2].xyz, viewDir.xyz);
	tangentViewDir.w = dot(tangentViewDir.xyz, tangentViewDir.xyz);
	tangentViewDir.w = rsqrt(tangentViewDir.w);
	tangentViewDir.xyz = tangentViewDir.xyz * tangentViewDir.www;

	// Compute the current gradients:
	float2 textureDims;
	TexLandColor.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = coords * textureDims;

	// Compute all 4 derivatives in x and y in a single instruction to optimize:
	float2 dxSize, dySize;
	float2 dx, dy;

	float4( dxSize, dx ) = ddx( float4( texCoordsPerSize, coords ) );
	float4( dySize, dy ) = ddy( float4( texCoordsPerSize, coords ) );
					
	// Find min of change in u and v across quad: compute du and dv magnitude across quad
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;

	// Standard mipmapping uses max here
	float minTexCoordDelta = max( dTexCoords.x, dTexCoords.y );

	// Compute the current mip level  (* 0.5 is effectively computing a square root before )
	float mipLevel = max( 0.5 * log2( minTexCoordDelta ), 0 );

	float2 output;
	if ( mipLevel <= 6 )
	{
		float2 dx = ddx(coords);
		float2 dy = ddy(coords);

		int numSteps = lerp(128, 4, tangentViewDir.z * (mipLevel / 6)) * blend;

		float currHeight = 0.0;
		float stepSize = 1.0 / (float)numSteps;

		float2 offsetPerStep = tangentViewDir.xy * float2(0.0800,0.0800) * stepSize.xx;
		float2 currentOffset = tangentViewDir.xy * float2(0.0400,0.0400) + coords.xy;
		float heightCorrectionScale = ((-1.0*tangentViewDir.z)+2.0);
		float prevHeight = 1.0;
		float currentBound = 1.0;
		float parallaxAmount = 0.0;

		float2 pt1 = 0;
		float2 pt2 = 0;

		while(numSteps > 0)
		{
			currentOffset -= offsetPerStep;
			
			currHeight = TexLandColor.SampleGrad(SampColorSampler, currentOffset,dx,dy).w - 0.5;
			currHeight = heightCorrectionScale * currHeight + 0.5;
			
			currentBound -= stepSize;
			if(currHeight>currentBound)
			{
				pt1 = float2(currentBound, currHeight);
				pt2 = float2(currentBound + stepSize, prevHeight);
				
				break;
			} 
			else
			{
				numSteps-=1;
				prevHeight=currHeight;
			}
		}

		float delta2 = pt2.x - pt2.y;
		float delta1 = pt1.x - pt1.y;
		float denominator = delta2-delta1;

		if(denominator==0.0){
			parallaxAmount=0.0;
		} 
		else 
		{
			parallaxAmount = (pt1.x * delta2 - pt2.x * delta1) / denominator;
		}

		float offset3 = (1.0 - parallaxAmount) * -0.0800 + 0.0400;
		output = tangentViewDir.xy * offset3.xx + coords.xy;

		if ( mipLevel > (float)(6 - 1) )
		{
			// Lerp based on the fractional part:
			float mipLevelInt;
			float mipLevelFrac = modf( mipLevel, mipLevelInt );

			// Lerp the texture coordinate from parallax occlusion mapped coordinate to bump mapping
			// smoothly based on the current mip level:
			float height = TexLandColor.Sample(SampColorSampler, coords.xy).w;
			height = height * 0.0800 - 0.0400;
			output = lerp( output, tangentViewDir.xy * height.xx + coords.xy, mipLevelFrac);
		}  
	}
	else
	{
		float height = TexLandColor.Sample(SampColorSampler, coords.xy).w;
		height = height * 0.0800 - 0.0400;
		output = tangentViewDir.xy * height.xx + coords.xy;
	}

	return output;
#else
	return coords;
#endif
}
