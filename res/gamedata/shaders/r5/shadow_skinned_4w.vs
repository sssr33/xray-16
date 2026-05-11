#define SM_6_0
#include "shadow_skinned_common.h"

struct VS_INPUT { float4 P : POSITION; float4 N : NORMAL; float4 T : TANGENT; float4 B : BINORMAL; float2 tc : TEXCOORD0; float4 ind : BLENDINDICES; };

ShadowSkinnedVS_OUTPUT main(VS_INPUT v)
{
    ShadowSkinnedVS_OUTPUT o;
    float w0 = v.N.w;
    float w1 = v.T.w;
    float w2 = v.B.w;
    float w3 = 1.0 - w0 - w1 - w2;
    int id0 = int(v.ind.x * 255.0 + 0.3);
    int id1 = int(v.ind.y * 255.0 + 0.3);
    int id2 = int(v.ind.z * 255.0 + 0.3);
    int id3 = int(v.ind.w * 255.0 + 0.3);
    float4x4 bone = get_bone(id0) * w0 + get_bone(id1) * w1 + get_bone(id2) * w2 + get_bone(id3) * w3;
    float4 skinned = mul(bone, v.P);
    float3 world = mul(m_W, skinned).xyz;
    o.position = mul(shadow_lightVP, float4(world, 1.0));
    o.texcoord = v.tc;
    o.materialID = g_SkinnedMaterialID;
    return o;
}
