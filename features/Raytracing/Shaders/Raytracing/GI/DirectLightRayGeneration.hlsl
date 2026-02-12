/**
 * Direct Light Ray Generation for Path Tracing
 * 
 * This pass handles:
 * 1. Primary ray tracing to find the first hit
 * 2. Direct lighting evaluation (NEE)
 * 3. Writing GBuffer for denoisers (Depth, Normal, Albedo, Motion Vectors, etc.)
 * 4. Writing Hit information for the Indirect Pass to reconstruct full Surface
 */

#include "Raytracing/Includes/Types.hlsli"

#include "Raytracing/Includes/RT/SHaRC.hlsli"
#include "Raytracing/Includes/Registers.hlsli"
#include "Raytracing/Includes/RT/SHaRCHelper.hlsli"

#include "Raytracing/Includes/Common.hlsli"
#include "Raytracing/Includes/ColorConversions.hlsli"
#include "Raytracing/Includes/RT/CommonRT.hlsli"
#include "Raytracing/Includes/RT/Shading.hlsli"
#include "Raytracing/Includes/RT/Geometry.hlsli"
#include "Raytracing/Includes/RT/SubsurfaceShading.hlsli"

#include "Common/Color.hlsli"
#include "Common/BRDF.hlsli"

#include "Raytracing/Includes/Surface.hlsli"

#include "Raytracing/Includes/MonteCarlo.hlsli"
#include "Raytracing/Includes/PBR.hlsli"

#include "Raytracing/Includes/Materials/BSDF.hlsli"
#include "Raytracing/Includes/Materials/TexLODHelpers.hlsli"

// Helper: Convert world position to NDC depth
float WorldPositionToNDCDepth(float3 worldPos, float3 cameraPos, float4x4 viewProj)
{
    float4 clipPos = mul(viewProj, float4(worldPos - cameraPos, 1.0));
    return clipPos.z / clipPos.w;
}

// Helper: Compute motion vector from current and previous frame
float2 ComputeMotionVector(float3 worldPos, float3 cameraPosNow, float3 cameraPosPrev, 
                            float4x4 viewProjNow, float4x4 viewProjPrev, float2 currentUV)
{
    // Project to previous frame
    float4 prevClip = mul(viewProjPrev, float4(worldPos - cameraPosPrev, 1.0));
    float2 prevNDC = prevClip.xy / prevClip.w;
    float2 prevUV = prevNDC * float2(0.5, -0.5) + 0.5;
    
    // Motion vector = current - previous (in UV space)
    return currentUV - prevUV;
}

// Hash function for SHaRC pixel randomization
uint Hash(uint2 idx)
{
    return (idx.x * 73856093u) ^ (idx.y * 19349663u);
}

[shader("raygeneration")]
void DirectLightRayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;

#if defined(SHARC) && defined(SHARC_UPDATE)
    // SHaRC Update Pass: randomize pixel location within 5x5 blocks
    // This is critical for proper cache population
    [branch]
    if (Frame.SHaRC.UpdatePass) {
        uint startIndex = Hash(idx) % 25;
        uint2 blockOrigin = idx * 5;
        uint pixelIndex = (startIndex + Frame.FrameCount) % 25;
        idx = blockOrigin + uint2(pixelIndex % 5, pixelIndex / 5);

        if (any(idx >= Frame.DispatchSize))
            return;

        size = Frame.DispatchSize;
    }
#endif

#if defined(CHECKERBOARD)    
    if ((idx.x + idx.y) & 1)
#elif defined(TEMPORAL_CHECKERBOARD)
    if ((idx.x + idx.y + Frame.FrameCount) & 1)
#endif
#if defined(CHECKERBOARD) || defined(TEMPORAL_CHECKERBOARD)
    {
        OutputTexture[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        DiffuseAlbedoPathTracing[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        NormalRoughnessPathTracing[idx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        SpecularAlbedo[idx] = float4(0.5f, 0.5f, 0.5f, 0.0f);
        SpecularHitDist[idx] = RAY_TMAX;
        DepthPathTracing[idx] = 1.0f;
        MotionVectorsPathTracing[idx] = float2(0.0f, 0.0f);
        HitInfoPathTracing[idx] = uint4(0, 0, 0, 0);
        return;
    }       
#endif   
    
    uint randomSeed = InitRandomSeed(idx, size, Frame.FrameCount);
    
    const float2 uv = float2(idx + 0.5f) / size;
    
    float2 screenPos = uv * 2.0f - 1.0f;
    screenPos.y = -screenPos.y;

    const float4 clip = float4(screenPos, 1.0f, 1.0f);
    float4 view = mul(Frame.ProjInverse, clip);
    view /= view.w;

    float3 sourceDirection = normalize(mul((float3x3)Frame.ViewInverse, view.xyz));

    RayDesc sourceRay;
    sourceRay.Origin = Frame.Position.xyz;
    sourceRay.Direction = sourceDirection;
    sourceRay.TMin = 0.1f;
    sourceRay.TMax = 1e30;

    Payload sourcePayload;
    sourcePayload.hitDistance = -1.0f;
    sourcePayload.primitiveIndex = 0;
    sourcePayload.PackBarycentrics(float2(0.0f, 0.0f));
    sourcePayload.PackInstanceGeometryIndex(0, 0);
    sourcePayload.randomSeed = randomSeed;

    TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, DIFFUSE_RAY_HITGROUP_IDX, 0, DIFFUSE_RAY_MISS_IDX, sourceRay, sourcePayload);
    randomSeed = sourcePayload.randomSeed;

    RayCone sourceRayCone = RayCone::make(Frame.PixelConeSpreadAngle * sourcePayload.hitDistance, Frame.PixelConeSpreadAngle);    

    // Miss case - hit sky
    if (!sourcePayload.Hit())
    {
#if defined(SHARC) && defined(SHARC_UPDATE)
        // SHaRC Update Pass: primary miss doesn't contribute to cache, skip
        [branch]
        if (Frame.SHaRC.UpdatePass)
            return;
#endif

        const float4 mainColor = MainTexture.SampleLevel(BaseSampler, uv, 0);
    
        OutputTexture[idx] = float4(LLGammaToTrueLinear(mainColor.rgb), 0.0f);
        DiffuseAlbedoPathTracing[idx] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        NormalRoughnessPathTracing[idx] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        SpecularAlbedo[idx] = float4(0.5f, 0.5f, 0.5f, 0.0f);
        SpecularHitDist[idx] = RAY_TMAX;
        DepthPathTracing[idx] = 1.0f;  // Far plane
        MotionVectorsPathTracing[idx] = float2(0.0f, 0.0f);
        HitInfoPathTracing[idx] = uint4(0, 0, 0, 0);  // No hit
        return;
    }

    // Hit case - build surface and evaluate direct lighting
    float3 sourcePosition = Frame.Position.xyz + sourceDirection * sourcePayload.hitDistance;

    Instance sourceInstance;
    Material sourceMaterial;

    Surface sourceSurface = Surface(sourcePosition, sourcePayload, sourceDirection, sourceRayCone, sourceInstance, sourceMaterial);
    BRDFContext sourceBRDFContext = BRDFContext(sourceSurface, -sourceDirection);
    if (dot(sourceSurface.FaceNormal, sourceBRDFContext.ViewDirection) < 0.0f) sourceSurface.FlipNormal();

    StandardBSDF sourceBSDF = StandardBSDF::make(sourceSurface, true);

    AdjustShadingNormal(sourceSurface, sourceBRDFContext, true, false);

    // Direct Light evaluation
    float3 direct = sourceSurface.Emissive;
    bool isSssPath = false;
    
#ifdef SUBSURFACE_SCATTERING
    if (sourceSurface.SubsurfaceData.HasSubsurface != 0) {
        direct += EvaluateSubsurfaceNEE(sourceSurface, sourceBRDFContext, sourceMaterial, sourceInstance, sourcePayload, sourceRayCone, randomSeed);
        isSssPath = true;
    }
    else
#endif
    {
        direct += EvaluateDirectRadiance(sourceMaterial, sourceSurface, sourceBRDFContext, sourceInstance, sourceBSDF, randomSeed);
    }

    // === Write outputs ===
    
    // 1. Direct radiance output
    OutputTexture[idx] = float4(direct, isSssPath ? 1.0f : 0.0f);
    
    // 2. GBuffer for denoiser: DiffuseAlbedo + Metalness
    DiffuseAlbedoPathTracing[idx] = float4(sourceSurface.DiffuseAlbedo, sourceSurface.Metallic);
    
    // 3. GBuffer for denoiser: Normal + Roughness
    NormalRoughnessPathTracing[idx] = float4(sourceSurface.Normal, sourceSurface.Roughness);
    
    // 4. Specular Albedo (F0 * envBRDF)
    const float2 envBRDF = BRDF::EnvBRDFApproxHirvonen(sourceSurface.Roughness, sourceBRDFContext.NdotV);
    const float3 specularAlbedo = float3(sourceSurface.F0 * envBRDF.x + envBRDF.y);
    SpecularAlbedo[idx] = float4(specularAlbedo, 0.0f);
    
    // 5. Specular hit distance (primary hit for now)
    SpecularHitDist[idx] = sourcePayload.hitDistance;
    
    // 6. Depth (NDC depth for denoiser compatibility)
    float ndcDepth = WorldPositionToNDCDepth(sourcePosition, Frame.Position.xyz, Frame.ViewProj);
    DepthPathTracing[idx] = ndcDepth;
    
    // 7. Motion Vectors
    float2 motionVector = ComputeMotionVector(
        sourcePosition,
        Frame.Position.xyz,
        Frame.PositionPrev.xyz,
        Frame.ViewProj,
        Frame.ViewProjPrev,
        uv
    );
    MotionVectorsPathTracing[idx] = motionVector;
    
    // 8. Hit information for Indirect Pass to reconstruct full Surface
    // Pack: hitDistance (as uint bits), primitiveIndex, barycentricsPacked, instanceGeometryIndexPacked
    HitInfoPathTracing[idx] = uint4(
        asuint(sourcePayload.hitDistance),
        sourcePayload.primitiveIndex,
        sourcePayload.barycentricsPacked,
        sourcePayload.instanceGeometryIndexPacked
    );
}
