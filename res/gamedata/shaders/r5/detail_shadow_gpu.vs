#include "detail_interaction.h"

cbuffer ShadowViewCB : register(b5) {
	float4x4 g_LightVP;
	uint g_CascadeIndex;
	float g_SmapSize;
	float g_Pad0;
	float g_Pad1;
};

cbuffer DetailGlobals : register(b3)
{
	float4 consts;
	float4 wave;
	float4 dir2D;
	float4 dir2D_2;
	float4x4 g_detail_VP;
	float4 detail_params;
	float4 g_wind_direction;
	float grass_wind_displacement;
	float grass_interaction_displacement;
	uint interaction_atlas_index;
	uint perlin4d_texture_index;
	float4 grass_color_tip;
	float4 grass_color_base;
	float4 grass_sss_color;
	float grass_color_variation;
	float grass_blade_height;
	float _pad0, _pad1;
};

Texture3D g_Perlin4D : register(t12);
SamplerState g_LinearSampler : register(s0);

StructuredBuffer<uint> visible_indices : register(t33);
StructuredBuffer<InstanceData> all_instances : register(t37);

float4 main(v_blade_sdf I, uint instance_id : SV_InstanceID) : SV_Position
{
	uint src_idx = visible_indices[instance_id];
	InstanceData raw = all_instances[src_idx];

	float rotation = float((raw.packed >> 8) & 0x3FF) / 1023.0 * TWO_PI;
	float scale = float((raw.packed >> 18) & 0x3FF) / 1023.0 * PACK_MAX_SCALE;

	float4 no_interaction = float4(0.5, 0.5, 0, 0);

	BezierBladeResult blade = EvaluateBezierBlade(
		raw.pos, rotation, scale, I.t, I.pos.x,
		g_Perlin4D, g_LinearSampler,
		g_wind_direction, wave,
		grass_wind_displacement, grass_interaction_displacement,
		grass_blade_height, no_interaction);

	float4 pos = float4(blade.bezier_pos + blade.width_offset, 1.0);
	return mul(g_LightVP, pos);
}
