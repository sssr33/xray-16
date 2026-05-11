#include "cull_utils.h"

struct SlotAABB
{
    float3 aabb_min;
    float padding0;
    float3 aabb_max;
    float padding1;
    uint instance_base;
    uint instance_count;
    int slot_x;
    int slot_z;
    float4 padding2;
};

struct InstanceData
{
    float3 pos;
    uint packed;
};

static const float PACK_MAX_SCALE = 4.0;

struct DetailModelGPU
{
    float minScale;
    float maxScale;
    float flags;
    float geomExtentX;
    float geomExtentZ;
    float uv_min_x;
    float uv_min_y;
    float uv_max_x;
    float uv_max_y;
    uint pulledVertexBase;
    uint pulledIndexCount;
    float geomExtentY;
};

cbuffer GrassShadowCullParams : register(b5)
{
    float4 g_light_frustum_planes[6];
    float3 g_camera_pos;
    float g_fade_distance_sqr;
    float g_lod_distance_close_sqr;
    float g_lod_distance_mid_sqr;
    uint g_total_slot_count;
    uint g_visible_capacity;
    uint g_grass_mode;
    uint g_visible_decal_capacity;
    uint g_pad0, g_pad1;
};

StructuredBuffer<SlotAABB> g_slot_aabbs : register(t0);
StructuredBuffer<InstanceData> g_all_instances : register(t1);
StructuredBuffer<DetailModelGPU> g_detail_models : register(t2);

RWStructuredBuffer<uint> g_visible_lod0 : register(u0);
RWByteAddressBuffer g_indirect_args_lod0 : register(u1);
RWStructuredBuffer<uint> g_visible_lod1 : register(u2);
RWByteAddressBuffer g_indirect_args_lod1 : register(u3);
RWStructuredBuffer<uint> g_visible_lod2 : register(u4);
RWByteAddressBuffer g_indirect_args_lod2 : register(u5);
RWStructuredBuffer<uint> g_visible_decals : register(u6);
RWByteAddressBuffer g_indirect_args_decal : register(u7);

static const uint DO_NO_WAVING = 0x0001;

[numthreads(256, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint slot_idx = dispatch_thread_id.x;

    if (slot_idx >= g_total_slot_count)
        return;

    SlotAABB slot = g_slot_aabbs[slot_idx];

    if (slot.instance_count == 0)
        return;

    if (!FrustumTestAABB(slot.aabb_min, slot.aabb_max, g_light_frustum_planes))
        return;

    for (uint i = 0; i < slot.instance_count; i++)
    {
        uint inst_idx = slot.instance_base + i;
        InstanceData inst = g_all_instances[inst_idx];
        uint object_id = inst.packed & 0x3F;
        DetailModelGPU mdl = g_detail_models[object_id];
        uint flags = asuint(mdl.flags);
        bool is_static = (flags & DO_NO_WAVING) != 0;

        if (is_static)
        {
            uint idx;
            g_indirect_args_decal.InterlockedAdd(4, 1, idx);
            if (idx < g_visible_decal_capacity)
                g_visible_decals[idx] = inst_idx;
        }
        else if (g_grass_mode == 0)
        {
            uint idx;
            g_indirect_args_lod0.InterlockedAdd(4, 1, idx);
            if (idx < g_visible_capacity)
                g_visible_lod0[idx] = inst_idx;
        }
        else
        {
            float3 to_camera = inst.pos - g_camera_pos;
            float dist_sqr = dot(to_camera, to_camera);

            uint idx;
            if (dist_sqr < g_lod_distance_close_sqr)
            {
                g_indirect_args_lod0.InterlockedAdd(4, 1, idx);
                if (idx < g_visible_capacity)
                    g_visible_lod0[idx] = inst_idx;
            }
            else if (dist_sqr < g_lod_distance_mid_sqr)
            {
                g_indirect_args_lod1.InterlockedAdd(4, 1, idx);
                if (idx < g_visible_capacity)
                    g_visible_lod1[idx] = inst_idx;
            }
            else
            {
                g_indirect_args_lod2.InterlockedAdd(4, 1, idx);
                if (idx < g_visible_capacity)
                    g_visible_lod2[idx] = inst_idx;
            }
        }
    }
}
