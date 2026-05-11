#define SM_6_0
#include "shadow_skinned_common.h"

struct VS_INPUT { float4 P : POSITION; float4 N : NORMAL; float4 T : TANGENT; float4 B : BINORMAL; float2 tc : TEXCOORD0; };

ShadowSkinnedVS_OUTPUT main(VS_INPUT v)
{
    ShadowSkinnedVS_OUTPUT o;
    int boneIdx = int(v.N.w * 255.0 + 0.3);
    float4 skinned = mul(get_bone(boneIdx), v.P);
    float3 world = mul(m_W, skinned).xyz;
    o.position = mul(shadow_lightVP[shadow_cascadeIdx], float4(world, 1.0));
    o.texcoord = v.tc;
    o.materialID = g_SkinnedMaterialID;
    return o;
}
