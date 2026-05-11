#ifndef SHADOW_SAMPLING_H_INCLUDED
#define SHADOW_SAMPLING_H_INCLUDED

#include "shared/shadow_common.h"

#ifdef CSM_SHADOW_FORWARD
Texture2DArray<float> g_SunShadowArray : register(t23);
SamplerComparisonState s_ShadowCmp : register(s4);
#endif

uint GetCascadeIndex(float viewDepth)
{
    uint cascadeCount = (uint)shadow_params.x;
    [unroll]
    for (uint i = 0; i < MAX_SUN_CASCADES; ++i)
    {
        if (i >= cascadeCount)
            break;
        if (viewDepth < shadow_cascades[i].farBound)
            return i;
    }
    return SHADOW_NO_CASCADE;
}

float SampleSunCascade(uint cascadeIndex, float3 worldPos, float3 normalWS)
{
#ifndef CSM_SHADOW_FORWARD
    return 1.0;
#else
    GpuShadowCascade cascade = shadow_cascades[cascadeIndex];

    float3 offsetPos = worldPos;
    offsetPos += normalWS * shadow_params.w * cascade.texelSize;
    offsetPos += shadow_lightDirWS.xyz * shadow_params.z;

    float4 shadowClip = mul(cascade.worldToShadowClip, float4(offsetPos, 1.0));
    float3 ndc = shadowClip.xyz / shadowClip.w;

    if (any(ndc.xy < -1.0) || any(ndc.xy > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;

    float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    return g_SunShadowArray.SampleCmpLevelZero(
        s_ShadowCmp,
        float3(uv, (float)cascadeIndex),
        ndc.z);
#endif
}

float SampleSunShadow(float3 worldPos, float3 normalWS, float viewDepth)
{
    if (shadow_textureParams.w < 0.5)
        return 1.0;

    uint cascadeIndex = GetCascadeIndex(viewDepth);
    if (cascadeIndex == SHADOW_NO_CASCADE)
        return 1.0;

    float shadow = SampleSunCascade(cascadeIndex, worldPos, normalWS);

    uint nextCascade = cascadeIndex + 1;
    if (nextCascade < (uint)shadow_params.x)
    {
        float farBound = shadow_cascades[cascadeIndex].farBound;
        float blendStart = (1.0 - shadow_params.y) * farBound;
        if (viewDepth >= blendStart)
        {
            float nextShadow = SampleSunCascade(nextCascade, worldPos, normalWS);
            float t = saturate((viewDepth - blendStart) / (farBound - blendStart));
            shadow = lerp(shadow, nextShadow, t);
        }
    }

    return shadow;
}

#endif
