struct GPUObjectData
{
    float3 position;
    float radius;
    uint batchIndex;
    uint flags;
    float2 padding;
};

struct IndirectDrawArgs
{
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};

static const uint MAX_CASCADES = 4;

cbuffer ShadowCullParams : register(b5)
{
    float4 g_CascadePlanes[6 * MAX_CASCADES];
    uint g_ObjectCount;
    uint g_NumCascades;
    uint2 g_Padding;
};

StructuredBuffer<GPUObjectData> g_Objects : register(t0);
RWStructuredBuffer<IndirectDrawArgs> g_ShadowDrawArgs : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 dtID : SV_DispatchThreadID)
{
    uint idx = dtID.x;
    if (idx >= g_ObjectCount)
        return;

    GPUObjectData obj = g_Objects[idx];

    [unroll]
    for (uint c = 0; c < MAX_CASCADES; c++)
    {
        if (c >= g_NumCascades)
            break;

        bool visible = true;
        uint planeBase = c * 6;

        [unroll]
        for (uint p = 0; p < 5; p++)
        {
            float4 plane = g_CascadePlanes[planeBase + p];
            float d = dot(plane.xyz, obj.position) + plane.w;
            if (d > obj.radius)
            {
                visible = false;
                break;
            }
        }

        g_ShadowDrawArgs[c * g_ObjectCount + idx].instanceCount = visible ? 1 : 0;
    }
}
