#ifndef SHADOW_COMMON_H_INCLUDED
#define SHADOW_COMMON_H_INCLUDED

#define MAX_SUN_CASCADES 4
#define SHADOW_NO_CASCADE 0xFFFFFFFF

struct GpuShadowCascade
{
    float4x4 worldToShadowClip;
    float4x4 worldToShadowTex;
    float farBound;
    float texelSize;
    float _pad0;
    float _pad1;
};

cbuffer SunShadowCB : register(b3)
{
    GpuShadowCascade shadow_cascades[MAX_SUN_CASCADES];
    float4 shadow_lightDirWS;
    float4 shadow_params;
    float4 shadow_textureParams;
};

#endif
