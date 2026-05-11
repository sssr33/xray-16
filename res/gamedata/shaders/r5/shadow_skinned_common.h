#ifndef SHADOW_SKINNED_COMMON_H
#define SHADOW_SKINNED_COMMON_H

static const uint NUM_SHADOW_CASCADES = 4;

cbuffer DynamicTransforms : register(b0) { float4x4 m_W; };
cbuffer ShadowViewCB : register(b3) { float4x4 shadow_lightVP[NUM_SHADOW_CASCADES]; uint shadow_cascadeIdx; float shadow_smapSize; float2 shadow_padding; };
StructuredBuffer<float4x4> g_BoneMatrices : register(t3);
cbuffer SkinnedMaterialCB : register(b4) { uint g_SkinnedMaterialID; uint g_SkeletonBoneOffset; uint g_SplatOffset; uint g_SplatCount; };

float4x4 get_bone(int i) { return g_BoneMatrices[g_SkeletonBoneOffset + (i / 3)]; }

struct ShadowSkinnedVS_OUTPUT
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    nointerpolation uint materialID : TEXCOORD1;
};

#endif
