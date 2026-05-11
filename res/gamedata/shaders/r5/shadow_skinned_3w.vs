#define SM_6_0
#include "shadow_skinned_common.h"

struct VS_INPUT { float4 P : POSITION; float4 N : NORMAL; float4 T : TANGENT; float4 B : BINORMAL; float4 tc : TEXCOORD0; };

ShadowSkinnedVS_OUTPUT main(VS_INPUT v)
{
    ShadowSkinnedVS_OUTPUT o;
    float w0 = v.N.w;
    float w1 = v.T.w;
    float w2 = 1.0 - w0 - w1;
    int id_0 = int(v.tc.z);
    int id_1 = int(v.tc.w);
    int id_2 = int(v.B.w * 255.0 + 0.3);
    float4x4 bone = get_bone(id_0) * w0 + get_bone(id_1) * w1 + get_bone(id_2) * w2;
    float4 skinned = mul(bone, v.P);
    float3 world = mul(m_W, skinned).xyz;
    o.position = mul(shadow_lightVP[shadow_cascadeIdx], float4(world, 1.0));
    o.texcoord = v.tc.xy;
    o.materialID = g_SkinnedMaterialID;
    return o;
}
