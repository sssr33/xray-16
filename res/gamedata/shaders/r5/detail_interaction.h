#ifndef DETAIL_INTERACTION_H
#define DETAIL_INTERACTION_H

static const float PACK_MAX_SCALE = 4.0;
static const float TWO_PI = 6.28318530718;
static const float M_PI = 3.1415926;

static const float HEIGHT_VARIATION_MIN = 0.7;
static const float HEIGHT_VARIATION_MAX = 1.15;
static const float HEIGHT_NOISE_SCALE = 0.05;

struct InstanceData
{
	float3 pos;
	uint packed;
};

struct GPUSlotData
{
	float world_min_x;
	float world_min_z;
	float y_base;
	float y_height;
	uint packed_ids;
	uint packed_palette_01;
	uint packed_palette_23;
	float hemi;
};

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

struct PulledVertex
{
	float px, py, pz;
	float u, v;
};

struct v_blade_sdf
{
	float3 pos : POSITION;
	float2 tc : TEXCOORD;
	float t : COLOR0;
	float width_scale : COLOR1;
};

struct BezierBladeResult
{
	float3 bezier_pos;
	float3 width_offset;
	float3 tangent;
	float3 facing;
	float blade_height;
};

BezierBladeResult EvaluateBezierBlade(
	float3 instance_pos,
	float rotation,
	float scale,
	float vertex_height_factor,
	float vertex_x,
	Texture3D perlin4d,
	SamplerState linear_sampler,
	float4 wind_direction,
	float4 wave_params,
	float wind_displacement,
	float interaction_displacement,
	float blade_height_multiplier,
	float4 interaction)
{
	BezierBladeResult result;

	float2 height_uv = instance_pos.xz * HEIGHT_NOISE_SCALE + float2(0.37, 0.73);
	float height_noise = perlin4d.SampleLevel(linear_sampler, float3(height_uv, 0), 0).r;
	float height_multiplier = lerp(HEIGHT_VARIATION_MIN, HEIGHT_VARIATION_MAX, height_noise);
	result.blade_height = scale * height_multiplier * blade_height_multiplier;

	float3 P0 = instance_pos;
	float3 P1 = instance_pos + float3(0, result.blade_height * 0.33, 0);
	float3 P2 = instance_pos + float3(0, result.blade_height * 0.67, 0);
	float3 P3 = instance_pos + float3(0, result.blade_height, 0);

	result.facing = normalize(float3(sin(rotation), 0.0, cos(rotation)));

	float wind_speed = max(wind_direction.y, 0.1);
	float time = wave_params.w;

	float2 dir_uv = P0.zx * (0.005 / wind_speed) + time * (0.005 * wind_speed);
	float wind_dir_noise = perlin4d.SampleLevel(linear_sampler, float3(dir_uv, 0), 0).r;

	float2 str_uv = P0.xz * (0.025 / wind_speed) + time * 0.05;
	float wind_str_noise = perlin4d.SampleLevel(linear_sampler, float3(str_uv, 0), 0).r;

	float height_factor = vertex_height_factor;
	float2 turb_uv = P0.xz * (0.025 / wind_speed) + (time + height_factor * height_factor * 0.25) * 0.05;
	float wind_turb_noise = perlin4d.SampleLevel(linear_sampler, float3(turb_uv, 0), 0).r;

	float fbm_wind_strength = lerp(0.25, 1.0, wind_str_noise);
	fbm_wind_strength *= fbm_wind_strength;
	fbm_wind_strength *= wind_speed;

	float wind_turbulence = lerp(0.25, 1.0, wind_turb_noise);
	wind_turbulence *= wind_turbulence;
	wind_turbulence *= min(wind_speed, 1.0) * 0.3;

	float fbm_turbulence = (wind_dir_noise * 2.0 - 1.0) * 0.3;

	float wind_angle_rad = wind_direction.x * (M_PI / 180.0);
	float2 global_wind_dir = float2(sin(wind_angle_rad), cos(wind_angle_rad));
	float2 perpendicular_dir = float2(-global_wind_dir.y, global_wind_dir.x);
	float2 wind_dir_with_turbulence = normalize(global_wind_dir + perpendicular_dir * fbm_turbulence);

	float2 interaction_push_dir = interaction.rg * 2.0 - 1.0;
	float interaction_strength = length(interaction_push_dir);
	if (interaction_strength > 0.001)
		interaction_push_dir = interaction_push_dir / interaction_strength;

	float2 facing_dir_xz = result.facing.xz;
	float2 wind_dir_xz = normalize(float2(wind_dir_with_turbulence.x, wind_dir_with_turbulence.y));

	float alignment = dot(wind_dir_xz, facing_dir_xz);
	float resistance = lerp(0.3, 1.0, abs(alignment));

	float wind_bend_strength = fbm_wind_strength * wind_displacement * resistance;

	float interaction_bend_strength = 0.0;
	float3 interaction_bend_dir = float3(0, 0, 0);
	if (interaction_strength > 0.001)
	{
		float2 interaction_dir_xz = normalize(interaction_push_dir);
		float interaction_alignment = dot(interaction_dir_xz, facing_dir_xz);
		float interaction_resistance = lerp(0.3, 1.0, abs(interaction_alignment));
		interaction_bend_strength = interaction_strength * interaction_displacement * interaction_resistance;
		interaction_bend_dir = float3(interaction_dir_xz.x, 0.0, interaction_dir_xz.y);
	}

	float3 wind_bend_dir = float3(wind_dir_xz.x, 0.0, wind_dir_xz.y);

	float3 total_bend_force = wind_bend_dir * wind_bend_strength + interaction_bend_dir * interaction_bend_strength;
	float total_bend_strength = length(total_bend_force);

	float3 bend_dir = float3(0, 0, 0);
	if (total_bend_strength > 0.001)
		bend_dir = normalize(total_bend_force);

	float max_bend_displacement = result.blade_height * 0.8;
	float bend_ratio = saturate(total_bend_strength / max_bend_displacement);
	float bend_angle = bend_ratio * 1.2 + wind_turbulence * vertex_height_factor;

	if (total_bend_strength > 0.001)
	{
		float tip_horizontal = result.blade_height * sin(bend_angle);
		float tip_vertical_drop = result.blade_height * (1.0 - cos(bend_angle));
		P3 += bend_dir * tip_horizontal;
		P3.y -= tip_vertical_drop;

		float p1_angle = bend_angle * 0.33;
		P1 += bend_dir * (result.blade_height * 0.33 * sin(p1_angle));
		P1.y -= result.blade_height * 0.33 * (1.0 - cos(p1_angle));

		float p2_angle = bend_angle * 0.67;
		P2 += bend_dir * (result.blade_height * 0.67 * sin(p2_angle));
		P2.y -= result.blade_height * 0.67 * (1.0 - cos(p2_angle));

		float interaction_vertical = interaction.b * interaction_displacement * -0.5;
		if (abs(interaction_vertical) > 0.01)
		{
			P1.y += interaction_vertical * 0.33;
			P2.y += interaction_vertical * 0.67;
			P3.y += interaction_vertical;
		}
	}

	float t = vertex_height_factor;
	float mt = 1.0 - t;
	float mt2 = mt * mt;
	float mt3 = mt2 * mt;
	float t2 = t * t;
	float t3 = t2 * t;

	result.bezier_pos = mt3 * P0 + 3.0 * mt2 * t * P1 + 3.0 * mt * t2 * P2 + t3 * P3;

	float3 tangent_raw = 3.0 * mt2 * (P1 - P0) + 6.0 * mt * t * (P2 - P1) + 3.0 * t2 * (P3 - P2);
	float tangent_len = length(tangent_raw);
	result.tangent = (tangent_len > 0.001) ? (tangent_raw / tangent_len) : float3(0, 1, 0);

	float cos_rot = cos(rotation);
	float sin_rot = sin(rotation);
	float3 right = float3(cos_rot, 0.0, -sin_rot);
	result.width_offset = right * (vertex_x * scale);

	return result;
}

struct BillboardWindResult
{
	float4 world_pos;
};

BillboardWindResult ApplyBillboardWind(
	float4 world_pos,
	float height_factor,
	Texture3D perlin4d,
	SamplerState linear_sampler,
	float4 wind_direction,
	float4 wave_params,
	float wind_displacement)
{
	BillboardWindResult result;

	float wind_speed = max(wind_direction.y, 0.1);
	float time = wave_params.w;

	float wind_angle_rad = wind_direction.x * (M_PI / 180.0);
	float2 global_wind_dir = float2(sin(wind_angle_rad), cos(wind_angle_rad));

	float2 dir_uv = world_pos.zx * (0.005 / wind_speed) + time * (0.005 * wind_speed);
	float wind_dir_noise = perlin4d.SampleLevel(linear_sampler, float3(dir_uv, 0), 0).r;

	float2 str_uv = world_pos.xz * (0.025 / wind_speed) + time * 0.05;
	float wind_str_noise = perlin4d.SampleLevel(linear_sampler, float3(str_uv, 0), 0).r;

	float fbm_wind_strength = lerp(0.25, 1.0, wind_str_noise);
	fbm_wind_strength *= fbm_wind_strength;
	fbm_wind_strength *= wind_speed;

	float fbm_turbulence = (wind_dir_noise * 2.0 - 1.0) * 0.3;
	float2 perpendicular_dir = float2(-global_wind_dir.y, global_wind_dir.x);
	float2 wind_dir = normalize(global_wind_dir + perpendicular_dir * fbm_turbulence);

	float displacement = fbm_wind_strength * wind_displacement * height_factor;
	result.world_pos = world_pos;
	result.world_pos.x += displacement * wind_dir.x;
	result.world_pos.z += displacement * wind_dir.y;

	return result;
}

#endif
